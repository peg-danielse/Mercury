#!/usr/bin/env python3
"""llmctl: an LLM controller for Mercury's CPU placement (SPEC.md "LLM interface").

The model gets Mercury's placement moves as tools and runs a short episode: inspect the
machine, then move daemons off troubled L3 domains, add whole L3 domains ("nodes") or single
cores to daemons that need them, and stop with a one-paragraph summary.

  inspect        GET  /llm/state      host load, every L3 domain (load, free CPUs, daemons), every daemon
  move_daemon    POST /llm/act move   a daemon's CPUs in one L3 domain -> free CPUs of another
  add_l3_domain  POST /llm/act add_l3 all free CPUs of one more L3 domain
  add_cores      POST /llm/act add_cores
  remove_cores   POST /llm/act remove_cores

Division of work: the daemons' autoscaling (saturate) keeps deciding how many CPUs each daemon gets;
the model decides where they are. Its actions leave autoscaling attached.

Latency: with --latency (the load generator's loadgen.hist.jsonl) the state also carries the application's
end-to-end latency (the last second, a healthy reference, the last 30 s), and p95 above --latency-factor
times the reference is trouble. Mercury's state carries per-CPU hardware counters (busy, run-queue wait,
IPC, LLC misses) when it runs with cpustat, also aggregated per daemon and domain.

Cursor: every /llm/state reply carries a cursor; passing it back adds "since", the placement events
(l3 adds, throttle cuts, shift moves, model actions, saturate steps, starts and exits) and each L3
domain's load history after it. The model's inspect takes a cursor, and each episode opens with
what happened since the previous episode ended.

Cycle: with --every N, llmctl checks Mercury every N seconds without the model. When an L3 domain
is above --above or a daemon's occupancy is at or above --short (and its CPU pressure at --pressure) on --confirm checks in a row, it
runs an episode, then waits --cooldown seconds (default two windows) before the next one can start.
A hot domain counts only when an action could relieve it: shared by daemons, or holding one short
of CPUs. After the model holds, the same trouble does not call it again for --rehold seconds.
--duration ends the cycle after that many seconds (e.g. 180 for a 3-minute run).
For speed an episode is one model call by default (--max-turns 1): the opening message carries the
state, the changes since the last episode and that episode's results, the model's calls run, and
the next check verifies them. The model may call hold when no action is warranted.

  llmctl.py --url http://127.0.0.1:18180 [--goal TEXT] [--dry-run] [--log steps.jsonl]   one episode now
  llmctl.py --every 0.5 [--above 0.8] [--short 0.9] [--confirm 2] [--cooldown 2]      fast checking cycle

Shedding: with --shed ROUTES (the application's optional routes) and --shed-daemons IDS (the daemons that
shed them: the hotel frontend; social's SocialGraphService,UserTimelineService) the model also gets
shed_route and restore_route, which write the set of shed routes through Mercury (POST /llm/act degrade,
the degrade system's policy files). Only routes in --shed can be shed, so the critical ones never are. The
state then carries "degrade" (sheddable and shed routes) and, with --critical, critical and optional latency;
shed routes with healthy latency and an unsaturated pool call the model to restore them. Without --shed the
prompt, tools and state are as before.

Models (--llm):
  gemini (default)  GEMINI_API_KEY; --model or GEMINI_MODEL (default gemini-3.8-flash); GEMINI_BASE_URL;
                    --thinking low (default, fastest), high, or default.
                    Function calling over plain HTTPS, stdlib only. The key is sent in a header, never logged.
  claude            the anthropic SDK (pip install -U anthropic; --no-fallbacks for SDKs without the
                    fallbacks parameter); ANTHROPIC_API_KEY or an `ant auth login` profile.
  claude-cli        Claude Code's print mode (`claude -p`) on its logged-in account, e.g. a Claude subscription:
                    no API key. Claude Code's tools are off; the model answers with its calls as JSON
                    (--json-schema). A process per call (about 3 s); use --call-timeout 60.
  grok              xAI's chat completions (OpenAI format) over plain HTTPS: XAI_API_KEY (sent in a header, never
                    logged); --model or XAI_MODEL (default grok-4); XAI_BASE_URL.
  openrouter        OpenRouter's chat completions (OpenAI format), any model with tool calling (e.g. google/gemini-3.8-flash,
                    openai/gpt-6-luna, qwen/qwen3.8-flash): OPENROUTER_API_KEY; --model or OPENROUTER_MODEL; each
                    call's cost is logged; --thinking low/high is passed as a reasoning effort.
  distilled         a small local transformer distilled from an llm's decisions (tools/learn/distill/train.py): --model
                    MODEL.pt; needs torch; decides in milliseconds, no network.
  mock              no network: the prompt's rules as code (for tests).
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import socket
import ssl
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import HTTPSHandler, Request, build_opener, urlopen

MODEL = "claude-opus-5-5"
GEMINI_MODEL = "gemini-3.8-flash"

SYSTEM = """You manage CPU capacity and placement on one Linux server through Mercury, a daemon keeper.
Each daemon (service) runs in a cgroup whose cpuset decides which Linux CPUs it may use. CPUs are grouped into
L3 cache domains (cores sharing one last-level cache); domains sit in NUMA nodes and sockets.

The application gets a pool: the L3 domains its services may use (state "pool", and "in_pool" per domain). An
autoscaler keeps running for every service: it decides how many CPUs each one gets, but only from the pool, so
the pool is the application's capacity. You decide the capacity and where it is:
- grow_pool adds a whole free L3 domain to the pool. Do it only when the pool is saturated: its "pressure"
  (share of time the services' threads waited for a CPU) is high while "allocated" (pool CPUs the autoscaler has
  handed out, counted per service) exceeds "cores" (the services are doubled up), or its "busy" share is near 1.
  More pool lets the autoscaler give services more CPUs. Slow requests without those signals are not a capacity
  problem (a database or a neighbour can cause them): growing the pool then only costs CPUs.
- shrink_pool gives one domain back (the one added last, or a named pool domain; never below "start"). Do it
  when the pool has spare capacity: busy well under 1, pressure near 0, and latency at its healthy reference.
- swap_domain replaces one pool domain by a free domain outside the pool and moves the services on it along.
  Do it when a pool domain is loaded from outside Mercury ("foreign_cpus": CPUs' worth of load its services do
  not account for, a noisy neighbour such as a batch job): the capacity moves away from the neighbour. Prefer a
  free domain on the same NUMA node, else the same socket.
- move_daemon moves one service's CPUs between two domains inside the pool, to separate services that slow
  each other down on a hot domain.
A swap or a move does not add capacity, and more capacity does not escape a neighbour: saturation needs
grow_pool, a noisy neighbour needs swap_domain.

Objectives, in order:
1. Critical requests fast and without errors (end-to-end latency near its healthy reference).
2. Optional requests served (no errors, reasonable latency).
3. The smallest pool that keeps 1 and 2: every CPU in the pool is a cost. When 1 and 2 hold with room to spare,
   shrink the pool; grow it only when capacity signals show it is too small.

What you see:
- "latency": end-to-end latency of the application's requests: "now" (the last second), "reference_p95_ms" (a
  healthy level from the last minute), "p95_history_ms" (the last 30 s in 5 s slices, oldest first).
- "pool": start, max (your budget: the pool cannot grow past it; 0 means none), cores, cpus, busy, pressure,
  allocated, free_domains. Each "l3" domain: load, cpus, free CPUs, daemons, in_pool,
  foreign_cpus, and hardware counters (ipc, llc_per_ms, cpu_wait). "cpus": per CPU busy, wait (run-queue wait),
  ipc, llc_per_ms and the daemons on it. "daemons": each service's cpus, cpu_use, occupancy, pressure, ipc.
- A domain whose services' IPC drops while LLC misses rise and foreign_cpus is high suffers cache or memory
  interference from a neighbour: swap it away.
- "since" (with a cursor): the placement events since your last look, including what the autoscaler and the pool
  did, and each domain's load history.

How to act:
- The message that calls you holds the full state and what changed since your previous episode, with that
  episode's results. Decide from it directly; call inspect only for something it does not show.
- Doing nothing is often right: a short spike that has already passed, or latency at its reference. Then call
  hold with the reason.
- An action shows only after a window; the episode usually ends after your first set of calls and you are
  called back, with the results, if the trouble remains.
- Prefer few, well-chosen actions, all in one turn. Give each a short reason."""

TOOLS: list[dict[str, Any]] = [
    {
        "name": "inspect",
        "description": (
            "Read the machine: latency, pool, L3 domains, per-CPU counters and services. With since_cursor set to an "
            "earlier cursor it also returns the events and per-domain load history after it. wait_seconds sleeps first."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "wait_seconds": {"type": "number", "description": "0 to 20"},
                "since_cursor": {"type": "string", "description": "a cursor from an earlier inspect, or empty"},
            },
            "required": ["wait_seconds", "since_cursor"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "name": "grow_pool",
        "description": (
            "Add one whole free L3 domain to the pool (more capacity for the autoscaler). l3 names the domain (one of "
            "pool.free_domains), or \"auto\" takes the nearest free one."
        ),
        "input_schema": {
            "type": "object",
            "properties": {"l3": {"type": "string"}, "reason": {"type": "string"}},
            "required": ["l3", "reason"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "name": "shrink_pool",
        "description": (
            "Give one L3 domain back: l3 names a pool domain, or \"auto\" gives back the one added last. Its services "
            "move to the pool's remaining CPUs. Refused below the pool's start size."
        ),
        "input_schema": {
            "type": "object",
            "properties": {"l3": {"type": "string"}, "reason": {"type": "string"}},
            "required": ["l3", "reason"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "name": "swap_domain",
        "description": (
            "Replace pool domain from_l3 by free domain to_l3 (outside the pool); the services on from_l3 move to "
            "to_l3, CPU for CPU. Use it to take capacity away from a noisy neighbour."
        ),
        "input_schema": {
            "type": "object",
            "properties": {"from_l3": {"type": "string"}, "to_l3": {"type": "string"}, "reason": {"type": "string"}},
            "required": ["from_l3", "to_l3", "reason"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "name": "move_daemon",
        "description": (
            "Move a service's CPUs that lie in L3 domain from_l3 to the same number of free CPUs of domain to_l3; "
            "both domains must be in the pool. Its other CPUs stay."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "daemon_id": {"type": "string"},
                "from_l3": {"type": "string"},
                "to_l3": {"type": "string"},
                "reason": {"type": "string"},
            },
            "required": ["daemon_id", "from_l3", "to_l3", "reason"],
            "additionalProperties": False,
        },
        "strict": True,
    },
]
TOOLS.append({
    "name": "hold",
    "description": "Decide that no action is warranted now, with the reason. Ends the episode.",
    "input_schema": {
        "type": "object",
        "properties": {"reason": {"type": "string"}},
        "required": ["reason"],
        "additionalProperties": False,
    },
    "strict": True,
})
ACTIONS = {"move_daemon", "grow_pool", "shrink_pool", "swap_domain", "shed_route", "restore_route"}

# --app NAME: a description of the application that opens every episode, so the model knows what its routes are, which
# are critical, and what its daemons do. Without --app the prompt is the generic one (the hotel and social runs).
APPLICATIONS = {
    "media": """The application is DeathStarBench's media microservices, a movie review site: 13 C++ Thrift services
(CastInfo, ComposeReview, MovieId, MovieInfo, MovieReview, Page, Plot, Rating, ReviewStorage, Text, UniqueId, UserReview,
User), each in its own daemon named media-<service>. Their data is in MongoDB (media-mongod); each service has its own
memcached (media-11300 to media-11306); redis is media-redis-server. The load generator (media-loadgen) sends three routes:
- read-page (critical): PageService.ReadPage for one movie. Page calls MovieInfo, Plot, CastInfo and MovieReview.
- compose-review (critical): one review written by four uploads (User, Text, MovieId and UniqueId services). Each hands
  its part to ComposeReview, which stores the review through ReviewStorage, MovieReview and UserReview.
- read-movie-reviews (optional): MovieReview.ReadMovieReviews for one movie.
There is no front end and nothing can be shed: every route is served or it is not. The end-to-end latency in the state is
the load generator's; the critical routes' latency is what the objectives protect. Latency that does not show in the
services' CPU use usually waits in mongod or memcached, and more CPUs for the services do not fix it.""",
}

SHED_SYSTEM = """

Shedding is enabled: the application's optional routes ("degrade.sheddable") can be turned off. Requests to a
shed route are refused at once, so their work leaves the services; critical routes can never be shed.
- shed_route turns one optional route off. It gives up objective 2 for objective 1: use it only when critical
  requests suffer from saturation that capacity cannot fix, because the pool is at its budget ("pool.max"); it is
  refused while the pool can still grow. Compare critical latency with its own reference
  ("latency.critical.reference_p95_ms"), not with the optional routes'. Never shed to save pool CPUs (objective 3).
- restore_route turns a shed route on again. Restore as soon as there is room (critical latency near its
  reference and the pool not saturated), one route at a time, the one shed last first.
"degrade" shows the sheddable routes and those shed now; "latency" also splits "critical" and "optional"."""

SHED_TOOLS: list[dict[str, Any]] = [
    {
        "name": "shed_route",
        "description": "Turn one optional route off (one of degrade.sheddable): its requests are refused at once.",
        "input_schema": {
            "type": "object",
            "properties": {"route": {"type": "string"}, "reason": {"type": "string"}},
            "required": ["route", "reason"],
            "additionalProperties": False,
        },
        "strict": True,
    },
    {
        "name": "restore_route",
        "description": "Turn a shed route on again (one of degrade.shed).",
        "input_schema": {
            "type": "object",
            "properties": {"route": {"type": "string"}, "reason": {"type": "string"}},
            "required": ["route", "reason"],
            "additionalProperties": False,
        },
        "strict": True,
    },
]


# --------------------------------------------------------------------------- Mercury


class Mercury:
    def __init__(self, url: str, window_ms: int, dry_run: bool, latency: "Latency | None" = None,
                 shed: list[str] | None = None, shed_daemons: str = "") -> None:
        self.url = url.rstrip("/")
        self.window_ms = window_ms
        self.dry_run = dry_run
        self.latency = latency
        self.sheddable = shed or []  # empty: shedding is off
        self.shed_daemons = shed_daemons
        self.shed: list[str] = []

    def _call(self, path: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
        data = None if body is None else json.dumps(body).encode()
        request = Request(self.url + path, data=data, method="GET" if body is None else "POST")
        try:
            with urlopen(request, timeout=30) as reply:
                return json.loads(reply.read())
        except HTTPError as error:  # Mercury answers refusals with a JSON reason
            try:
                return json.loads(error.read())
            except ValueError:
                return {"ok": False, "error": f"HTTP {error.code}"}
        except (URLError, OSError, ValueError) as error:
            return {"ok": False, "error": f"mercury unreachable: {error}"}

    def state(self, cursor: str | None = None) -> dict[str, Any]:
        query = f"/llm/state?window_ms={self.window_ms}"
        if cursor:
            query += "&cursor=" + quote(cursor)
        state = self._call(query)
        if state.get("ok") and self.latency:
            if (summary := self.latency.summary()) is not None:
                state["latency"] = summary
        if state.get("ok") and self.sheddable:
            state["degrade"] = {"sheddable": self.sheddable, "shed": list(self.shed)}
        return state

    def set_shed(self, routes: list[str], change: str) -> dict[str, Any]:
        result = self.act({"action": "degrade", "daemon_id": self.shed_daemons, "disabled": ",".join(routes),
                           "change": change})
        if result.get("ok") and not self.dry_run:
            self.shed = routes
        return result

    def act(self, body: dict[str, Any]) -> dict[str, Any]:
        if self.dry_run:
            return {"ok": True, "dry_run": True, "would_post": body}
        return self._call("/llm/act", body)


class Latency:
    """End-to-end latency from the load generator's 500 ms histograms (loadgen.hist.jsonl, appended live: one line
    per route per window, with route, t0_ns, t1_ns, sent, done, errors, hist [[upper bound ns, count], ...]). Read
    incrementally. The lines of one window share t0_ns: a time slice is all routes' lines of one window."""

    def __init__(self, path: str | None, critical: list[str] | None = None) -> None:
        self.path = path
        self.offset = 0
        self.slices: dict[int, list[dict[str, Any]]] = {}  # t0_ns -> that window's lines
        self.critical = critical  # set: the summary also splits critical and optional routes

    def _read(self) -> None:
        try:
            with open(self.path, "rb") as f:
                f.seek(self.offset)
                data = f.read()
        except OSError:
            return
        end = data.rfind(b"\n") + 1  # a half-written last line waits for the next read
        self.offset += end
        for line in data[:end].splitlines():
            try:
                w = json.loads(line)
            except ValueError:
                continue
            self.slices.setdefault(w["t0_ns"], []).append(w)
        for t in sorted(self.slices)[:-240]:  # two minutes of 500 ms windows
            del self.slices[t]

    @staticmethod
    def _percentiles(windows: list[dict[str, Any]], qs: tuple[float, ...]) -> list[float | None]:
        hist: dict[int, int] = {}
        for w in windows:
            for bound, count in w.get("hist", []):
                hist[bound] = hist.get(bound, 0) + count
        total = sum(hist.values())
        out: list[float | None] = []
        for q in qs:
            if total == 0:
                out.append(None)
                continue
            seen, need = 0, q * total
            for bound in sorted(hist):
                seen += hist[bound]
                if seen >= need:
                    out.append(round(bound / 1e6, 1))
                    break
        return out

    def _part(self, slices: list[list[dict[str, Any]]], keep) -> dict[str, Any]:
        """now: the last two slices (a second); reference_p95: the 20th percentile of the slice p95s of the last
        minute (a healthy level that a few bad seconds do not move); p95_history: 5 s spans of the last 30 s."""
        sel = [[w for w in sl if keep(w)] for sl in slices]
        now = [w for sl in sel[-2:] for w in sl]
        p50, p95, p99 = self._percentiles(now, (0.5, 0.95, 0.99))
        span = sum(sl[0]["t1_ns"] - sl[0]["t0_ns"] for sl in slices[-2:]) / 1e9
        sent = sum(w.get("sent", 0) for w in now)
        slice_p95 = [p for sl in sel[-120:] for p in self._percentiles(sl, (0.95,)) if p is not None]
        last = sel[-60:]
        return {"now": {"p50_ms": p50, "p95_ms": p95, "p99_ms": p99,
                        "req_per_s": round(sum(w.get("done", 0) for w in now) / span) if span else None,
                        "error_rate": round(sum(w.get("errors", 0) for w in now) / sent, 4) if sent else 0},
                "reference_p95_ms": sorted(slice_p95)[len(slice_p95) // 5] if slice_p95 else None,
                "p95_history_ms": [self._percentiles([w for sl in last[i:i + 10] for w in sl], (0.95,))[0]
                                   for i in range(0, len(last), 10)]}

    def summary(self) -> dict[str, Any] | None:
        if not self.path:
            return None
        self._read()
        if len(self.slices) < 4:
            return None
        slices = [self.slices[t] for t in sorted(self.slices)]
        out = self._part(slices, lambda w: True)
        if self.critical:
            for name, crit in (("critical", True), ("optional", False)):
                part = self._part(slices, lambda w, crit=crit: (w.get("route") in self.critical) == crit)
                out[name] = {"p95_ms": part["now"]["p95_ms"], "error_rate": part["now"]["error_rate"],
                             "reference_p95_ms": part["reference_p95_ms"]}
        return out


def pool_at_budget(state: dict[str, Any]) -> bool:
    """The pool cannot grow: no free domain, or the smallest free one would take it past its budget (pool.max)."""
    pool = state.get("pool")
    if not pool:
        return True
    names = set(pool.get("free_domains", []))
    free = [len(b["cpus"]) for b in state.get("l3", []) if b["name"] in names]
    if not free:
        return True
    return bool(pool.get("max")) and pool.get("cores", 0) + min(free) > pool["max"]


def run_tool(mercury: Mercury, name: str, args: dict[str, Any]) -> dict[str, Any]:
    """Run one tool call against Mercury. Always returns a JSON object; ok false carries a reason."""
    if name == "inspect":
        wait = min(max(float(args.get("wait_seconds", 0)), 0.0), 20.0)
        time.sleep(wait)
        return mercury.state(args.get("since_cursor") or None)
    daemon_id = str(args.get("daemon_id", ""))
    if name == "move_daemon":
        body = {"action": "move", "daemon_id": daemon_id, "from": args.get("from_l3"), "to": args.get("to_l3")}
    elif name == "grow_pool":
        body = {"action": "grow_pool"}
        if args.get("l3") not in (None, "", "auto"):
            body["l3"] = args["l3"]
    elif name == "shrink_pool":
        body = {"action": "shrink_pool"}
        if args.get("l3") not in (None, "", "auto"):
            body["l3"] = args["l3"]
    elif name == "swap_domain":
        body = {"action": "swap_domain", "from": args.get("from_l3"), "to": args.get("to_l3")}
    elif name in ("shed_route", "restore_route") and mercury.sheddable:
        route = str(args.get("route", ""))
        if name == "shed_route":
            if not pool_at_budget(mercury.state()):
                return {"ok": False, "error": "shedding is refused while the pool can still grow: grow_pool first"}
            if route not in mercury.sheddable:
                return {"ok": False, "error": f"{route} is not sheddable: one of {', '.join(mercury.sheddable)}"}
            if route in mercury.shed:
                return {"ok": False, "error": f"{route} is already shed"}
            return mercury.set_shed(mercury.shed + [route], "disabled " + route)
        if route not in mercury.shed:
            return {"ok": False, "error": f"{route} is not shed"}
        return mercury.set_shed([r for r in mercury.shed if r != route], "enabled " + route)
    else:
        return {"ok": False, "error": f"unknown tool {name}"}
    return mercury.act(body)


# --------------------------------------------------------------------------- episodes


class Log:
    def __init__(self, path: str | None) -> None:
        self.file = open(path, "a", encoding="utf-8") if path else None

    def write(self, **record: Any) -> None:
        record["t"] = round(time.time(), 3)
        line = json.dumps(record, default=str)
        print(line, file=sys.stderr)
        if self.file:
            self.file.write(line + "\n")
            self.file.flush()


def opening_message(args: argparse.Namespace, trigger: str, opening: dict[str, Any]) -> str:
    previous = json.dumps(args.previous, separators=(",", ":")) if args.previous else "none"
    app = APPLICATIONS[args.app] + "\n\n" if args.app else ""
    return (f"{app}{args.goal}\n\nWhy you were called: {trigger}\n\n"
            f"Your previous episode's calls and their results: {previous}\n\n"
            "The machine now; \"since\" covers what happened after your previous episode (absent on the first), "
            "and \"cursor\" is the one to pass to inspect:\n" + json.dumps(opening, separators=(",", ":")))


def execute(args: argparse.Namespace, mercury: Mercury, log: Log, name: str, call_args: dict[str, Any]) -> dict[str, Any]:
    """One model tool call: the per-episode action limit, hold, and a record for the next episode's opening."""
    if name == "hold":
        result = {"ok": True, "held": True}
    elif name in ACTIONS and sum(c["tool"] in ACTIONS for c in args.current) >= args.max_actions:
        result = {"ok": False, "error": f"action limit of {args.max_actions} for this episode reached"}
    else:
        result = run_tool(mercury, name, call_args)
    log.write(event="tool", name=name, input=call_args, ok=result.get("ok"),
              result=result if name != "inspect" else {"ok": result.get("ok")})
    if name != "inspect":
        args.current.append({"tool": name, "input": call_args, "ok": result.get("ok"),
                             "result": result.get("detail") or result.get("error") or ("held" if name == "hold" else "dry run")})
    return result


def episode_done(args: argparse.Namespace, turn: int) -> str | None:
    """After a turn's calls ran: the summary when the episode ends here (hold, or the turn limit)."""
    if any(c["tool"] == "hold" for c in args.current) or turn + 1 >= args.max_turns:
        return "; ".join(f"{c['tool']} {c['input'].get('daemon_id') or c['input'].get('reason', '')}: {c['result']}"
                         for c in args.current) or "no action (the model only inspected)"
    return None


def claude_episode(args: argparse.Namespace, mercury: Mercury, log: Log, trigger: str, opening: dict[str, Any]) -> str:
    import anthropic  # only this mode needs the SDK

    client = anthropic.Anthropic(timeout=args.call_timeout, max_retries=0)
    messages: list[Any] = [{"role": "user", "content": opening_message(args, trigger, opening)}]
    for turn in range(args.max_turns):
        try:
            # A refusal is re-run server-side on Anthropic's recommended fallback model; --no-fallbacks
            # drops this for SDKs older than the fallbacks parameter.
            fallback = {} if args.no_fallbacks else {"betas": ["server-side-fallback-2026-07-01"], "fallbacks": "default"}
            response = client.beta.messages.create(
                model=args.model,
                max_tokens=16000,
                system=SYSTEM,
                tools=TOOLS,
                messages=messages,
                output_config={"effort": args.effort},
                **fallback,
            )
        except TypeError as error:
            if "fallbacks" not in str(error):
                raise
            log.write(event="error", kind="sdk", detail=str(error))
            return "this anthropic SDK predates fallbacks: pip install -U anthropic, or pass --no-fallbacks"
        except anthropic.RateLimitError as error:
            log.write(event="error", kind="rate_limit", detail=str(error))
            return "rate limited; episode stopped"
        except anthropic.APIStatusError as error:
            log.write(event="error", kind="api", status=error.status_code, detail=error.message)
            return f"API error {error.status_code}; episode stopped"
        except anthropic.APIConnectionError as error:
            log.write(event="error", kind="connection", detail=str(error))
            return "cannot reach the API; episode stopped"
        log.write(event="turn", turn=turn, stop_reason=response.stop_reason, model=response.model,
                  input_tokens=response.usage.input_tokens, output_tokens=response.usage.output_tokens)
        if response.stop_reason == "refusal":
            return "the model declined; episode stopped"
        if response.stop_reason == "max_tokens":
            return "the model ran out of output tokens; episode stopped"
        # Append the whole content unchanged (thinking and fallback blocks included).
        messages.append({"role": "assistant", "content": response.content})
        calls = [block for block in response.content if block.type == "tool_use"]
        if response.stop_reason != "tool_use" or not calls:
            return "".join(block.text for block in response.content if block.type == "text").strip()
        results = []
        for call in calls:
            result = execute(args, mercury, log, call.name, dict(call.input))
            results.append({"type": "tool_result", "tool_use_id": call.id, "content": json.dumps(result),
                            "is_error": not result.get("ok", False)})
        if (summary := episode_done(args, turn)) is not None:
            return summary
        messages.append({"role": "user", "content": results})
    return f"turn limit of {args.max_turns} reached"


def claude_cli_episode(args: argparse.Namespace, mercury: Mercury, log: Log, trigger: str, opening: dict[str, Any]) -> str:
    """One episode through Claude Code's print mode (`claude -p`, the logged-in subscription, no API key): the tools
    are described in the system prompt and the model answers with its calls as schema-checked JSON, which run
    through the same execute() as the other modes. Claude Code's own tools are off (--tools ""). One model call
    per turn; a further turn sends the results in a new call."""
    import shutil
    import subprocess
    import tempfile

    exe = shutil.which("claude")
    if not exe:
        return "claude (Claude Code) is not on PATH; episode stopped"
    names = [t["name"] for t in TOOLS if t["name"] != "inspect"]
    schema = {"type": "object", "required": ["calls"], "properties": {"calls": {"type": "array", "items": {
        "type": "object", "required": ["name", "args"],
        "properties": {"name": {"type": "string", "enum": names}, "args": {"type": "object"}}}}}}
    tools = json.dumps([{"name": t["name"], "description": t["description"], "args": t["input_schema"]["properties"]}
                        for t in TOOLS if t["name"] != "inspect"], separators=(",", ":"))
    system = (SYSTEM + "\n\nYour tools (answer with the calls to make, in order, each with its args; the opening "
              "message already holds the full state, so there is no inspect):\n" + tools)
    prompt = opening_message(args, trigger, opening)
    cwd = tempfile.mkdtemp(prefix="llmctl-claude-")  # an empty working directory: no project files or settings
    for turn in range(args.max_turns):
        cmd = [exe, "-p", "--tools", "", "--no-session-persistence", "--output-format", "json",
               "--system-prompt", system, "--json-schema", json.dumps(schema)]
        if args.model:
            cmd += ["--model", args.model]
        try:
            proc = subprocess.run(cmd, input=prompt, capture_output=True, text=True, cwd=cwd, timeout=args.call_timeout)
            reply = json.loads(proc.stdout)
        except subprocess.TimeoutExpired:
            log.write(event="error", kind="timeout", detail=f"claude -p took over {args.call_timeout:g} s")
            return f"claude -p took over {args.call_timeout:g} s; episode stopped"
        except ValueError:
            log.write(event="error", kind="cli", detail=(proc.stderr or proc.stdout)[-500:])
            return "claude -p error; episode stopped"
        usage = reply.get("usage") or {}
        log.write(event="turn", turn=turn, finish=reply.get("subtype"), model=args.model or "claude-code default",
                  input_tokens=sum(usage.get(k) or 0 for k in ("input_tokens", "cache_creation_input_tokens",
                                                               "cache_read_input_tokens")),
                  output_tokens=usage.get("output_tokens"), cost_usd=reply.get("total_cost_usd"),
                  ms=reply.get("duration_ms"))
        if reply.get("is_error"):
            log.write(event="error", kind="cli", status=reply.get("api_error_status"), detail=str(reply.get("result"))[:500])
            return f"claude -p error {reply.get('api_error_status')}; episode stopped"
        calls = (reply.get("structured_output") or {}).get("calls") or []
        if not calls:
            return "no action (the model returned no calls)"
        results = [{"name": c.get("name"), "result": execute(args, mercury, log, str(c.get("name")), dict(c.get("args") or {}))}
                   for c in calls]
        if (summary := episode_done(args, turn)) is not None:
            return summary
        prompt = ("Results of your calls: " + json.dumps(results, separators=(",", ":")) + "\n\nThe machine now:\n"
                  + json.dumps(mercury.state(), separators=(",", ":")))
    return f"turn limit of {args.max_turns} reached"


DISTILLED: dict[str, Any] = {}


def distilled_episode(args: argparse.Namespace, mercury: Mercury, log: Log, trigger: str, opening: dict[str, Any]) -> str:
    """One decision by a small local transformer distilled from the llm's decisions (tools/learn/distill/train.py; --model
    is its model.pt; needs torch). The live state is turned into the training features (tools/learn/distill/train.py
    encode), the action and its targets are decoded with every invalid choice masked (a shrink names a pool domain, a
    swap a pool domain and a free one, a move a service and a domain it is on), and the call runs through execute()
    like a model's. "none" is a hold."""
    import time as _time
    t0 = _time.perf_counter()
    if not DISTILLED:
        import torch
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "learn", "distill"))
        import train as distill
        torch.set_num_threads(1)
        ckpt = torch.load(args.model, map_location="cpu", weights_only=False)
        a = ckpt["args"]
        actions = ckpt["actions"]
        banned = [actions.index("shed_route"), actions.index("restore_route")] if a.get("no_shed") else []
        model = distill.Distilled(a["d"], a["layers"], a["heads"], 0.0, banned=banned)
        model.load_state_dict(ckpt["model"], strict=False)  # models trained before act_mask lack the buffer
        model.eval()
        DISTILLED.update(torch=torch, distill=distill, model=model, names=ckpt["names"], actions=actions,
                         routes=ckpt["routes"], start=time.time())
        t0 = _time.perf_counter()  # the decision's time, without the one-time import and load
    torch, distill, model = DISTILLED["torch"], DISTILLED["distill"], DISTILLED["model"]
    state = opening
    pool_cpus = set((state.get("pool") or {}).get("cpus") or [])
    for d in state.get("daemons", []):
        d.setdefault("cores", len(d.get("cpus") or []))
        d.setdefault("in_pool", bool(d.get("cpus")) and all(c in pool_cpus for c in d["cpus"]))
    episode = {"state": state, "budget": (state.get("pool") or {}).get("max", 0), "shedding": bool(mercury.sheddable),
               "t_s": time.time() - DISTILLED["start"],
               "previous": [{"name": c["tool"], "input": c["input"], "ok": c["ok"]} for c in args.previous]}
    x = distill.encode(episode, dict(DISTILLED["names"]))  # a copy: unseen services map to new ids, not saved
    batch = {k: v[None] for k, v in x.items() if not k.startswith("y_")}
    with torch.no_grad():
        out = {k: v[0] for k, v in model(batch).items()}
    actions = DISTILLED["actions"]
    l3 = {b["name"]: b for b in state.get("l3", [])}
    in_pool = [i for i in range(distill.NDOM) if (l3.get(f"l3.{i}") or {}).get("in_pool")]
    free = [distill.dom_index(n) for n in (state.get("pool") or {}).get("free_domains") or []]
    order = [d["daemon_id"] for d in sorted(state.get("daemons", []), key=lambda d: d["daemon_id"])][:distill.MAXD]

    def pick(logits, allowed):
        if not allowed:
            return None
        return max(allowed, key=lambda i: logits[i].item())

    act_logits = out["act"].clone()
    if not mercury.sheddable:
        act_logits[actions.index("shed_route")] = act_logits[actions.index("restore_route")] = -1e9
    for name in actions:  # actions whose targets do not exist now cannot be chosen
        if (name in ("grow_pool", "swap_domain") and not free) or (name == "shrink_pool" and not in_pool) or \
                (name == "restore_route" and not mercury.shed):
            act_logits[actions.index(name)] = -1e9
    act = actions[int(act_logits.argmax())]
    call: dict[str, Any] = {}
    if act == "grow_pool":
        i = pick(out["dom1"], free + [distill.NDOM])
        call = {"l3": "auto" if i in (None, distill.NDOM) else f"l3.{i}"}
    elif act == "shrink_pool":
        i = pick(out["dom1"], in_pool + [distill.NDOM])
        call = {"l3": "auto" if i in (None, distill.NDOM) else f"l3.{i}"}
    elif act == "swap_domain":
        i, j = pick(out["dom1"], in_pool), pick(out["dom2"], free)
        call = {"from_l3": f"l3.{i}", "to_l3": f"l3.{j}"} if i is not None and j is not None else {}
    elif act == "move_daemon":
        movable = [k for k, n in enumerate(order) if not n.endswith("loadgen")]
        k = pick(out["svc"], movable)
        if k is not None:
            mine = sorted({distill.dom_index(b["name"]) for b in state.get("l3", []) if order[k] in b["daemons"]} & set(in_pool))
            dest = [i for i in in_pool if i not in mine and (l3.get(f"l3.{i}") or {}).get("free")]
            i, j = pick(out["dom1"], mine), pick(out["dom2"], dest)
            if i is not None and j is not None:
                call = {"daemon_id": order[k], "from_l3": f"l3.{i}", "to_l3": f"l3.{j}"}
    elif act in ("shed_route", "restore_route"):
        routes = DISTILLED["routes"]
        allowed = [r for r, name in enumerate(routes) if (name in mercury.sheddable and name not in mercury.shed)
                   if act == "shed_route"] if act == "shed_route" else [r for r, name in enumerate(routes) if name in mercury.shed]
        r = pick(out["route"], allowed)
        call = {"route": routes[r]} if r is not None else {}
    ms = (_time.perf_counter() - t0) * 1000
    log.write(event="turn", turn=0, finish="local", model=os.path.basename(args.model), input_tokens=0, output_tokens=0,
              ms=round(ms, 2), action=act, call=call)
    if act == "none" or not call:
        execute(args, mercury, log, "hold", {"reason": f"distilled model: {act}"})
        return f"hold ({act}, {ms:.1f} ms)"
    call["reason"] = "distilled model"
    result = execute(args, mercury, log, act, call)
    return f"{act} {call}: {result.get('detail') or result.get('error')} ({ms:.1f} ms)"


def gemini_schema(schema: dict[str, Any]) -> dict[str, Any]:
    """A tool's JSON schema in Gemini's OpenAPI subset: upper-case types, no additionalProperties."""
    out: dict[str, Any] = {}
    for key, value in schema.items():
        if key == "additionalProperties":
            continue
        if key == "type":
            out[key] = str(value).upper()
        elif key == "properties":
            out[key] = {name: gemini_schema(prop) for name, prop in value.items()}
        else:
            out[key] = value
    return out


def gemini_tools() -> list[dict[str, Any]]:
    return [{"functionDeclarations": [
        {"name": tool["name"], "description": tool["description"], "parameters": gemini_schema(tool["input_schema"])}
        for tool in TOOLS
    ]}]


GEMINI_TOOLS = gemini_tools()


class FastConnection(http.client.HTTPSConnection):
    """HTTPS that tries IPv4 addresses first with a short connect timeout per address. Where IPv6
    resolves but does not route (pd-gpu), urllib would wait the full read timeout on every IPv6
    address before reaching IPv4; this bounds that to CONNECT_S per address."""

    CONNECT_S = 3.0

    def connect(self) -> None:
        infos = socket.getaddrinfo(self.host, self.port, type=socket.SOCK_STREAM)
        infos.sort(key=lambda info: info[0] != socket.AF_INET)
        error: OSError = OSError(f"no address for {self.host}")
        for family, kind, proto, _, address in infos:
            sock = socket.socket(family, kind, proto)
            sock.settimeout(self.CONNECT_S)
            try:
                sock.connect(address)
            except OSError as failed:
                sock.close()
                error = failed
                continue
            sock.settimeout(self.timeout)
            self.sock = self._context.wrap_socket(sock, server_hostname=self.host)
            return
        raise error


class FastHTTPSHandler(HTTPSHandler):
    def https_open(self, req: Request) -> Any:
        return self.do_open(FastConnection, req, context=ssl.create_default_context())


GEMINI_HTTP = build_opener(FastHTTPSHandler())


XAI_MODEL = "grok-4"
OPENROUTER_MODEL = "google/gemini-3.8-flash"
# OpenAI-format providers: name, key variable, base-URL variable, default base URL.
OPENAI_PROVIDERS = {
    "grok": ("Grok", "XAI_API_KEY", "XAI_BASE_URL", "https://api.x.ai/v1"),
    "openrouter": ("OpenRouter", "OPENROUTER_API_KEY", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1"),
}


def openai_tools() -> list[dict[str, Any]]:
    """TOOLS in the OpenAI chat-completions format, which xAI's and OpenRouter's APIs use."""
    return [{"type": "function", "function": {"name": t["name"], "description": t["description"],
                                              "parameters": t["input_schema"]}} for t in TOOLS]


def openai_episode(args: argparse.Namespace, mercury: Mercury, log: Log, trigger: str, opening: dict[str, Any]) -> str:
    """The same episode on an OpenAI-format chat completions API with function calling (xAI, OpenRouter), over plain
    HTTPS (no SDK). The key comes from the provider's variable (XAI_API_KEY, OPENROUTER_API_KEY) and is sent in a
    header, never logged; the base URL from XAI_BASE_URL / OPENROUTER_BASE_URL. OpenRouter also reports each call's
    cost (usage.include) and takes --thinking as a reasoning effort."""
    name, key_var, base_var, base = OPENAI_PROVIDERS[args.llm]
    key = os.environ.get(key_var)
    if not key:
        return f"{key_var} is not set; episode stopped"
    url = os.environ.get(base_var, base).rstrip("/") + "/chat/completions"
    messages: list[dict[str, Any]] = [{"role": "system", "content": SYSTEM},
                                      {"role": "user", "content": opening_message(args, trigger, opening)}]
    tools = openai_tools()
    for turn in range(args.max_turns):
        payload: dict[str, Any] = {"model": args.model, "messages": messages, "tools": tools, "tool_choice": "auto"}
        if args.llm == "openrouter":
            payload["usage"] = {"include": True}
            payload["provider"] = {"sort": "latency"}  # a decision that arrives late is stale for control
            payload["seed"] = args.seed  # best effort: the provider may ignore it
            payload["temperature"] = 0
            if args.thinking == "off":
                payload["reasoning"] = {"enabled": False}
            elif args.thinking != "default":
                payload["reasoning"] = {"effort": args.thinking}
        request = Request(url, data=json.dumps(payload).encode(), method="POST",
                          headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}",
                                   "X-Title": "Mercury llmctl"})
        try:
            with GEMINI_HTTP.open(request, timeout=args.call_timeout) as reply:
                data = json.loads(reply.read())
        except HTTPError as error:
            log.write(event="error", kind="api", status=error.code, detail=error.read().decode("utf-8", errors="replace")[:500])
            return f"{name} API error {error.code}; episode stopped"
        except TimeoutError:
            log.write(event="error", kind="timeout", detail=f"no answer within {args.call_timeout:g} s")
            return f"{name} took over {args.call_timeout:g} s; episode stopped"
        except (URLError, OSError, ValueError) as error:
            log.write(event="error", kind="connection", detail=str(error))
            return f"cannot reach the {name} API; episode stopped"
        if "error" in data:  # OpenRouter reports some upstream failures in a 200 body
            log.write(event="error", kind="api", status=(data["error"] or {}).get("code"), detail=str(data["error"])[:500])
            return f"{name} API error {(data['error'] or {}).get('code')}; episode stopped"
        choice = (data.get("choices") or [{}])[0]
        message = choice.get("message") or {}
        usage = data.get("usage") or {}
        log.write(event="turn", turn=turn, finish=choice.get("finish_reason"), model=data.get("model", args.model),
                  provider=data.get("provider"), fingerprint=data.get("system_fingerprint"), seed=payload.get("seed"), input_tokens=usage.get("prompt_tokens"),
                  output_tokens=usage.get("completion_tokens"),
                  reasoning_tokens=(usage.get("completion_tokens_details") or {}).get("reasoning_tokens"),
                  cost_usd=usage.get("cost"))
        calls = message.get("tool_calls") or []
        if not calls:
            return (message.get("content") or "no action (the model returned no calls)").strip()
        messages.append({"role": "assistant", "content": message.get("content"), "tool_calls": calls})
        for call in calls:
            fn = call.get("function") or {}
            try:
                call_args = json.loads(fn.get("arguments") or "{}")
            except ValueError:
                call_args = {}
            result = execute(args, mercury, log, str(fn.get("name")), call_args)
            messages.append({"role": "tool", "tool_call_id": call.get("id"), "content": json.dumps(result)})
        if (summary := episode_done(args, turn)) is not None:
            return summary
    return f"turn limit of {args.max_turns} reached"


def gemini_thinking(args: argparse.Namespace) -> dict[str, Any] | None:
    """The fastest thinking setting the model takes: a zero budget on 2.5 models, a level on 3.x."""
    if args.thinking == "default":
        return None
    if args.model.startswith("gemini-2"):
        return {"thinkingBudget": 0 if args.thinking in ("low", "off") else -1}
    return {"thinkingLevel": "low" if args.thinking == "off" else args.thinking}


def gemini_episode(args: argparse.Namespace, mercury: Mercury, log: Log, trigger: str, opening: dict[str, Any]) -> str:
    """The same episode on Gemini's generateContent function calling, over plain HTTPS (no SDK)."""
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        return "GEMINI_API_KEY is not set; episode stopped"
    base = os.environ.get("GEMINI_BASE_URL", "https://generativelanguage.googleapis.com/v1beta").rstrip("/")
    url = f"{base}/models/{args.model}:generateContent"
    contents: list[dict[str, Any]] = [{"role": "user", "parts": [{"text": opening_message(args, trigger, opening)}]}]
    for turn in range(args.max_turns):
        payload: dict[str, Any] = {
            "systemInstruction": {"parts": [{"text": SYSTEM}]},
            "contents": contents,
            "tools": GEMINI_TOOLS,
            "toolConfig": {"functionCallingConfig": {"mode": "AUTO"}},
        }
        if (thinking := gemini_thinking(args)) is not None:
            payload["generationConfig"] = {"thinkingConfig": thinking}
        # The key goes in a header, never in the URL, so it cannot land in a log.
        request = Request(url, data=json.dumps(payload).encode(), method="POST",
                          headers={"Content-Type": "application/json", "x-goog-api-key": key})
        try:
            with GEMINI_HTTP.open(request, timeout=args.call_timeout) as reply:
                data = json.loads(reply.read())
        except HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")[:500]
            log.write(event="error", kind="api", status=error.code, detail=detail)
            return f"Gemini API error {error.code}; episode stopped"
        except TimeoutError:
            log.write(event="error", kind="timeout", detail=f"no answer within {args.call_timeout:g} s")
            return f"Gemini took over {args.call_timeout:g} s; episode stopped"
        except (URLError, OSError, ValueError) as error:
            log.write(event="error", kind="connection", detail=str(error))
            return "cannot reach the Gemini API; episode stopped"
        candidates = data.get("candidates") or []
        usage = data.get("usageMetadata") or {}
        if not candidates:
            blocked = (data.get("promptFeedback") or {}).get("blockReason", "no candidate")
            log.write(event="turn", turn=turn, finish=blocked)
            return f"Gemini returned no answer ({blocked}); episode stopped"
        candidate = candidates[0]
        content = candidate.get("content") or {"role": "model", "parts": []}
        finish = candidate.get("finishReason")
        log.write(event="turn", turn=turn, finish=finish, model=args.model,
                  input_tokens=usage.get("promptTokenCount"), output_tokens=usage.get("candidatesTokenCount"))
        parts = content.get("parts") or []
        calls = [part["functionCall"] for part in parts if "functionCall" in part]
        if not calls:
            if finish not in (None, "STOP"):
                return f"Gemini stopped ({finish}); episode stopped"
            return "".join(str(part.get("text") or "") for part in parts).strip()
        # Keep the model's turn unchanged (thought signatures included) before answering its calls.
        contents.append(content)
        responses = []
        for call in calls:
            name = call.get("name", "")
            result = execute(args, mercury, log, name, dict(call.get("args") or {}))
            response: dict[str, Any] = {"name": name, "response": result}
            if call.get("id"):
                response["id"] = call["id"]
            responses.append({"functionResponse": response})
        if (summary := episode_done(args, turn)) is not None:
            return summary
        contents.append({"role": "user", "parts": responses})
    return f"turn limit of {args.max_turns} reached"


def mock_episode(args: argparse.Namespace, mercury: Mercury, log: Log, trigger: str, opening: dict[str, Any]) -> str:
    """The rules from SYSTEM as code: one pass over the opening state, same tools, no model. For tests."""
    state = opening
    if not state.get("ok"):
        return f"inspect failed: {state.get('error')}"
    done: list[str] = []

    def act(name: str, call: dict[str, Any]) -> None:
        result = execute(args, mercury, log, name, call)
        done.append(f"{name} {call.get('daemon_id')}: " + (result.get("detail") or result.get("error") or "dry run"))

    daemons = {d["daemon_id"]: d for d in state["daemons"]}
    l3 = {b["name"]: b for b in state["l3"]}
    pool = state.get("pool")
    degrade = state.get("degrade")
    if degrade and pool:
        latency = state.get("latency") or {}
        now, reference = (latency.get("now") or {}).get("p95_ms"), latency.get("reference_p95_ms")
        slow = bool(now and reference and now > args.latency_factor * reference)
        saturated = pool.get("pressure", 0) >= args.pool_pressure or pool.get("busy", 0) > args.above
        at_budget = pool_at_budget(state)
        # Shed the next optional route only at the budget; restore the one shed last when there is room.
        if slow and saturated and at_budget:
            left = [r for r in degrade["sheddable"] if r not in degrade["shed"]]
            if left:
                act("shed_route", {"route": left[0], "reason": "saturated at the pool budget"})
                return "; ".join(done)
        if degrade["shed"] and not slow and not saturated:
            act("restore_route", {"route": degrade["shed"][-1], "reason": "room to serve it again"})
            return "; ".join(done)
    if pool:
        # A pool domain with a neighbour outside Mercury: swap it for the nearest free domain.
        for block in state["l3"]:
            if block.get("in_pool") and block.get("foreign_cpus", 0) >= args.foreign * len(block["cpus"]):
                free = [l3[n] for n in pool.get("free_domains", []) if n in l3]
                free.sort(key=lambda b: (b["node"] != block["node"], b["socket"] != block["socket"], b["load"], b["cpus"][0]))
                if free:
                    act("swap_domain", {"from_l3": block["name"], "to_l3": free[0]["name"], "reason": "noisy neighbour"})
                    return "; ".join(done)
        # A pool larger than needed: give the domain added last back.
        if (pool.get("cores", 0) > pool.get("start", 0) and pool.get("busy", 1) <= args.pool_idle
                and pool.get("pressure", 1) <= 0.05):
            act("shrink_pool", {"l3": "auto", "reason": "pool underused"})
            return "; ".join(done)
        # A saturated pool: one more domain.
        if ((pool.get("pressure", 0) >= args.pool_pressure and pool.get("allocated", 0) > pool.get("cores", 0))
                or pool.get("busy", 0) > args.above):
            act("grow_pool", {"l3": "auto", "reason": "pool saturated"})
            return "; ".join(done)
    moved: set[str] = set()
    for block in sorted(state["l3"], key=lambda b: -b["load"]):
        if block["load"] <= 0.8 or len(block["daemons"]) < 2 or len(done) >= args.max_actions:
            continue
        # The lightest daemon leaves: its CPUs fit a cool domain most easily.
        for daemon_id in sorted(block["daemons"], key=lambda d: daemons[d]["cpu_use"]):
            mine = [c for c in daemons[daemon_id]["cpus"] if c in block["cpus"]]
            moved_use = daemons[daemon_id]["cpu_use"] * len(mine) / max(1, len(daemons[daemon_id]["cpus"]))
            dests = [b for b in l3.values() if b["name"] != block["name"] and len(b["free"]) >= len(mine)
                     and (b["load"] * len(b["cpus"]) + moved_use) / len(b["cpus"]) <= 0.8
                     and (not pool or b.get("in_pool"))]
            dests.sort(key=lambda b: (b["load"], b["node"] != block["node"], b["cpus"][0]))
            if dests:
                act("move_daemon", {"daemon_id": daemon_id, "from_l3": block["name"], "to_l3": dests[0]["name"],
                                    "reason": "shared hot domain"})
                moved.add(daemon_id)
                break
    if not done:
        execute(args, mercury, log, "hold", {"reason": "no saturated pool, noisy neighbour or shared hot domain"})
        return "hold: no change needed"
    return "; ".join(done)


def short_of_cpus(daemon: dict[str, Any], args: argparse.Namespace) -> bool:
    """Busy and waiting, the same test as saturate's add: occupancy at --short and threads queued."""
    if daemon["occupancy"] < args.short:
        return False
    if "pressure" in daemon:
        return daemon["pressure"] >= args.pressure
    return daemon.get("runnable", 0) >= len(daemon["cpus"]) + 0.5


def foreign_cpus(block: dict[str, Any], daemons: dict[str, dict[str, Any]]) -> float:
    """CPUs' worth of a domain's load that its daemons do not account for: processes outside Mercury.
    A daemon's use on the domain is taken as its cpu_use times the share of its CPUs there."""
    ours = 0.0
    for name in block["daemons"]:
        d = daemons.get(name)
        if d and d["cpus"]:
            inside = sum(cpu in block["cpus"] for cpu in d["cpus"])
            ours += d["cpu_use"] * inside / len(d["cpus"])
    return max(0.0, block["load"] * len(block["cpus"]) - ours)


def trouble(state: dict[str, Any], args: argparse.Namespace) -> dict[str, str]:
    """What would call the model, keyed by domain or daemon: hot L3 domains that an action could
    relieve (shared by daemons, holding a daemon short of CPUs, or loaded by a process outside Mercury
    beside a daemon) and daemons short of CPUs. A hot domain filled by one daemon that is not short is
    left out: neither a move nor more CPUs helps."""
    daemons = {d["daemon_id"]: d for d in state.get("daemons", [])}
    short = {name: d for name, d in daemons.items() if short_of_cpus(d, args)}
    found = {}
    for b in state.get("l3", []):
        if b["load"] <= args.above or not b["daemons"]:
            continue
        on = ", ".join(b["daemons"])
        foreign = foreign_cpus(b, daemons)
        if len(b["daemons"]) > 1 or any(d in short for d in b["daemons"]):
            found[b["name"]] = f"{b['name']} at {b['load']:.0%} " + (f"shared by {on}" if len(b["daemons"]) > 1 else f"with {on}")
        elif foreign >= args.foreign * len(b["cpus"]):
            found[b["name"]] = f"{b['name']} at {b['load']:.0%} with {on} and {foreign:.1f} CPUs of load from outside Mercury"
    for name, d in short.items():
        found[name] = (f"{name} short of CPUs: occupancy {d['occupancy']:.2f}, pressure {d.get('pressure', 0):.2f} "
                       f"on {len(d['cpus'])} CPUs")
    pool = state.get("pool")
    if pool and ((pool.get("pressure", 0) >= args.pool_pressure and pool.get("allocated", 0) > pool.get("cores", 0))
                 or pool.get("busy", 0) > args.above):
        found["pool"] = (f"the pool is saturated: {pool['cores']} CPUs at {pool.get('busy', 0):.0%} busy, "
                         f"services waiting {pool.get('pressure', 0):.0%} of the time")
    for b in state.get("l3", []):
        if b.get("in_pool") and b.get("foreign_cpus", 0) >= args.foreign * len(b["cpus"]) and b["name"] not in found:
            found[b["name"]] = f"pool domain {b['name']} has {b['foreign_cpus']:.1f} CPUs of load from outside Mercury"
    latency = state.get("latency") or {}
    now, reference = (latency.get("now") or {}).get("p95_ms"), latency.get("reference_p95_ms")
    healthy = not now or not reference or now <= 1.2 * reference
    if (pool and pool.get("cores", 0) > pool.get("start", 0) and pool.get("busy", 1) <= args.pool_idle
            and pool.get("pressure", 1) <= 0.05 and healthy and not found):
        found["pool_underused"] = (f"the pool may be larger than needed: {pool['cores']} CPUs (start "
                                   f"{pool.get('start')}) at {pool.get('busy', 0):.0%} busy, services not waiting")
    if now and reference and now > args.latency_factor * reference:
        found["latency"] = f"end-to-end p95 {now:.0f} ms, {now / reference:.1f}x the healthy {reference:.0f} ms"
    # Shed routes call the model back whenever critical latency is near its own reference and the pool is not
    # saturated, also beside other trouble: otherwise a standing alarm keeps them shed for the rest of the run.
    shed = (state.get("degrade") or {}).get("shed")
    crit = latency.get("critical") or {}
    crit_ok = not crit.get("p95_ms") or not crit.get("reference_p95_ms") or \
        crit["p95_ms"] <= args.latency_factor * crit["reference_p95_ms"]
    if shed and crit_ok and not (pool and pool.get("pressure", 0) >= args.pool_pressure):
        found["shed"] = (f"optional routes {', '.join(shed)} are shed while the pool is not saturated"
                         + (f" (critical p95 {crit['p95_ms']:.0f} ms, reference {crit['reference_p95_ms']:.0f} ms)"
                            if crit.get("p95_ms") and crit.get("reference_p95_ms") else "") + ": they may be restored")
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--url", default="http://127.0.0.1:18180", help="Mercury worker")
    parser.add_argument("--llm", choices=("gemini", "gimini", "claude", "claude-cli", "grok", "openrouter", "distilled", "mock"), default="gemini")
    parser.add_argument("--model", help=f"default: GEMINI_MODEL or {GEMINI_MODEL} for gemini, {MODEL} for claude, "
                                        f"Claude Code's own default for claude-cli, XAI_MODEL or {XAI_MODEL} for grok, "
                                        f"OPENROUTER_MODEL or {OPENROUTER_MODEL} for openrouter")
    parser.add_argument("--effort", choices=("low", "medium", "high", "xhigh", "max"), default="medium")
    parser.add_argument("--app", choices=("",) + tuple(APPLICATIONS), default="",
                        help="the application this controller manages (its routes and daemons), opening every episode")
    parser.add_argument("--goal", default="Inspect the server and fix any L3 domain above 80% or daemon short of CPUs.")
    parser.add_argument("--window-ms", type=int, default=1000, help="averaging window of inspect")
    parser.add_argument("--max-actions", type=int, default=6, help="placement actions per episode")
    parser.add_argument("--max-turns", type=int, default=1,
                        help="model turns per episode (1: one decision, checked by the next cycle; more lets it inspect and retry)")
    parser.add_argument("--seed", type=int, default=1, help="openrouter: the request's seed (best effort; logged per turn)")
    parser.add_argument("--thinking", choices=("off", "low", "high", "default"), default="low",
                        help="Gemini thinking: low is fastest (thinkingLevel low on 3.x, thinkingBudget 0 on 2.5); "
                             "openrouter: a reasoning effort, off disables reasoning (Gemini treats off as low)")
    parser.add_argument("--every", type=float, default=0, help="check every N seconds and call the model on trouble (0: one episode now; 0.5 is a fast cycle)")
    parser.add_argument("--above", type=float, default=0.8, help="an L3 domain above this load is trouble")
    parser.add_argument("--short", type=float, default=0.9, help="a daemon at or above this occupancy ...")
    parser.add_argument("--pressure", type=float, default=0.10, help="... and this CPU pressure is short of CPUs")
    parser.add_argument("--latency", help="the load generator's loadgen.hist.jsonl: end-to-end latency in the state and as a trigger")
    parser.add_argument("--latency-factor", type=float, default=2.0,
                        help="end-to-end p95 above this many times the healthy reference is trouble")
    parser.add_argument("--pool-idle", type=float, default=0.35,
                        help="a pool above its start size at or below this busy share, with no waiting, may shrink")
    parser.add_argument("--pool-pressure", type=float, default=0.25,
                        help="a pool whose services wait this share of the time is saturated")
    parser.add_argument("--call-timeout", type=float, default=20,
                        help="seconds a model call may take; a slower answer ends the episode (stale for control)")
    parser.add_argument("--foreign", type=float, default=0.25,
                        help="a hot domain is trouble when this share of its CPUs is busy outside Mercury's daemons")
    parser.add_argument("--duration", type=float, default=0, help="stop the cycle after this many seconds (0: never)")
    parser.add_argument("--confirm", type=int, default=2, help="checks in a row with trouble before an episode")
    parser.add_argument("--cooldown", type=float, default=None, help="seconds after an episode before the next (default: two windows)")
    parser.add_argument("--max-episodes", type=int, default=0, help="stop the cycle after this many episodes (0: never)")
    parser.add_argument("--rehold", type=float, default=60, help="after a hold, seconds before the same trouble calls the model again")
    parser.add_argument("--shed", default="", help="optional routes the model may shed (comma-separated; empty: no shedding)")
    parser.add_argument("--shed-daemons", default="", help="daemon ids that shed them (comma-separated)")
    parser.add_argument("--critical", default="", help="critical routes: the latency summary splits critical and optional")
    parser.add_argument("--dry-run", action="store_true", help="show actions without applying them")
    parser.add_argument("--no-fallbacks", action="store_true", help="omit server-side refusal fallbacks (older SDKs)")
    parser.add_argument("--log", help="append every check, turn and tool call as JSON lines")
    args = parser.parse_args()
    if args.cooldown is None:
        args.cooldown = 2 * args.window_ms / 1000
    args.previous, args.current = [], []
    if args.llm == "gimini":
        args.llm = "gemini"
    if not args.model and args.llm == "grok":
        args.model = os.environ.get("XAI_MODEL", XAI_MODEL)
    elif not args.model and args.llm == "openrouter":
        args.model = os.environ.get("OPENROUTER_MODEL", OPENROUTER_MODEL)
    elif args.llm == "distilled" and not args.model:
        parser.error("--llm distilled needs --model MODEL.pt")
    elif not args.model and args.llm != "claude-cli":
        args.model = os.environ.get("GEMINI_MODEL", GEMINI_MODEL) if args.llm == "gemini" else MODEL
    if args.max_turns == 1:
        # One turn: the opening message holds the full state and the episode ends after the calls, so an inspect can
        # only waste the episode (Gemini spent ~7% of its calls on it, the OpenRouter models most of theirs).
        global SYSTEM, GEMINI_TOOLS
        TOOLS[:] = [t for t in TOOLS if t["name"] != "inspect"]
        SYSTEM = SYSTEM.replace("Decide from it directly; call inspect only for something it does not show.",
                                "Decide from it directly: it is all there is to see this episode.")
        GEMINI_TOOLS = gemini_tools()
    shed = [r for r in args.shed.split(",") if r]
    if shed:
        if not args.shed_daemons:
            parser.error("--shed needs --shed-daemons")
        SYSTEM += SHED_SYSTEM
        TOOLS[-1:-1] = SHED_TOOLS  # before hold
        GEMINI_TOOLS = gemini_tools()
    critical = [r for r in args.critical.split(",") if r] or None
    mercury = Mercury(args.url, args.window_ms, args.dry_run,
                      Latency(args.latency, critical if shed else None) if args.latency else None, shed, args.shed_daemons)
    log = Log(args.log)
    episode = {"gemini": gemini_episode, "claude": claude_episode, "claude-cli": claude_cli_episode,
               "grok": openai_episode, "openrouter": openai_episode, "distilled": distilled_episode,
               "mock": mock_episode}[args.llm]

    def run(trigger: str, since: str | None) -> str | None:
        """One episode opening at the state since cursor since; returns the cursor after it."""
        opening = mercury.state(since)
        log.write(event="episode", llm=args.llm, model=None if args.llm == "mock" else args.model,
                  dry_run=args.dry_run, trigger=trigger)
        # The exact state the model is shown (with the previous episode's calls), for distilling its decisions into a
        # local model later: about 30 KB per episode. Written only to the --log file, not echoed.
        if log.file:
            log.file.write(json.dumps({"event": "state", "t": round(time.time(), 3), "previous": args.previous,
                                       "state": opening}, separators=(",", ":"), default=str) + "\n")
            log.file.flush()
        args.current = []
        summary = episode(args, mercury, log, trigger, opening)
        args.previous = args.current
        log.write(event="summary", text=summary)
        print(summary, flush=True)
        return mercury.state().get("cursor")

    if args.every <= 0:
        run("asked to check now", None)
        return 0
    check_cursor = episode_cursor = None
    streak = episodes = 0
    held_keys: set[str] = set()
    held_until = 0.0
    quiet_until = 0.0
    stop_at = time.time() + args.duration if args.duration > 0 else float("inf")
    while time.time() < stop_at:
        state = mercury.state(check_cursor)
        if not state.get("ok"):
            log.write(event="check", ok=False, error=state.get("error"))
            time.sleep(args.every)
            continue
        # The first episode opens with what happened since the cycle started.
        episode_cursor = episode_cursor or check_cursor or state["cursor"]
        check_cursor = state["cursor"]
        found = trouble(state, args)
        # After a hold, the same trouble does not call the model again until it changes or --rehold passes.
        if found and set(found) <= held_keys and time.time() < held_until:
            found = {}
        streak = streak + 1 if found else 0
        since = state.get("since", {})
        log.write(event="check", host_load=state["host"]["load"], trouble=list(found.values()), streak=streak,
                  events=[f"{e['daemon_id']} {e['kind']}: {e['detail']}" for e in since.get("events", [])])
        if streak >= args.confirm and time.time() >= quiet_until:
            episode_cursor = run("; ".join(found.values()), episode_cursor) or episode_cursor
            if args.previous and all(c["tool"] in ("hold", "inspect") for c in args.previous):
                held_keys, held_until = set(found), time.time() + args.rehold
            else:
                held_keys, held_until = set(), 0.0
            check_cursor = episode_cursor
            streak = 0
            episodes += 1
            quiet_until = time.time() + args.cooldown
            if args.max_episodes and episodes >= args.max_episodes:
                return 0
        time.sleep(args.every)
    log.write(event="stop", reason="duration", episodes=episodes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
