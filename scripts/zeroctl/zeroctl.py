#!/usr/bin/env python3
"""zeroctl model step for Mercury (port of infrabench rca/controller/llm).

Mercury runs this program (MERCURY_ZEROCTL) every ``zeroctl.every`` seconds with
the worker state as JSON on stdin. It renders the generate prompt, asks the model
for one action, parses the reply, and prints one JSON action on stdout:

  {"action": "move", "workload": "svc0", "node": "node.1", "reason": "..."}

Mercury validates and applies it; anything illegal is recorded as FAILED and
ignored. On any error this prints {"action": "none", "error": "..."}.

Providers (same as infrabench rca/controller/llm/client.py):
  --llm openai  OPENAI_API_KEY, OPENAI_MODEL (default gpt-4o-mini), OPENAI_BASE_URL
                (default https://api.openai.com/v1; any OpenAI-compatible endpoint)
  --llm gemini  GEMINI_API_KEY, GEMINI_MODEL (default gemini-2.5-flash)
  --llm mock    no network: a fixed rule (move the busiest daemon off a hot group),
                or replay ZEROCTL_MOCK_RESPONSE (a file holding raw model text)

Optional: ZEROCTL_SITUATION_FILE, a JSON file with {"now": {...}, "reference": {...},
"load": "..."} (p50_ms, p95_ms, error_rate, requests_per_s) written by the experiment
harness, rendered as the Situation section.

Python 3.8+, stdlib only. API keys are read from the environment and never logged.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / "generate.txt"

GOAL = (
    "Analyse monitoring data and propose one change to the Mercury worker that mitigates "
    "the degradation using CPU placement, CPU scaling, and optional-component degradation. "
    "Respect every constraint."
)

# Mercury's action space, in the same shape as infrabench actions/kinds.py.
ACTIONS = (
    ("move", "rewrites the daemon's cpuset onto another CPU group, away from what it shares a saturated group with. Same CPU count, same pid."),
    ("scale_out", "sets the daemon's CPU count (min = max = cpus). Extra CPUs come from the least-loaded cores."),
    ("degrade", "disables one optional component of a daemon; the program sheds that work while it is off."),
    ("restore", "re-enables a disabled optional component once there is headroom."),
    ("none", "when the server needs no change now. A busy group is not a reason to stop."),
    ("stop", "the application needs no further change."),
)
ACTION_NAMES = tuple(name for name, _ in ACTIONS)
FIELDS = ("action", "workload", "node", "cpus", "component", "reason")
TIMEOUT_S = 100


# --------------------------------------------------------------------------- prompt


def _pct(value: Any) -> str:
    return f"{float(value) * 100:.1f}%"


def _measured(values: dict[str, Any]) -> str:
    parts = []
    if "p50_ms" in values:
        parts.append(f"p50 {values['p50_ms']} ms")
    if "p95_ms" in values:
        parts.append(f"p95 {values['p95_ms']} ms")
    if "error_rate" in values:
        parts.append(f"errors {_pct(values['error_rate'])}")
    if "requests_per_s" in values:
        parts.append(f"{values['requests_per_s']} req/s")
    return ", ".join(parts)


def format_situation(situation: dict[str, Any] | None, state: dict[str, Any]) -> str:
    """Same layout as infrabench llm/situation.py format_situation."""
    situation = situation or {}
    names = [d["daemon_id"] for d in state.get("daemons") or []]
    lines = [f"application: {situation.get('application') or 'mercury daemons ' + ', '.join(names)}"]
    now = situation.get("now")
    if now:
        lines.append(f"now (last {now.get('seconds', '?')}s, {now.get('requests', '?')} requests): {_measured(now)}")
    else:
        lines.append("now: no latency measurement supplied")
    reference = situation.get("reference")
    if reference:
        lines.append(f"healthy reference: {_measured(reference)}")
    if situation.get("ranking"):
        lines.append(f"where the degradation shows, most affected first: {situation['ranking']}")
    if situation.get("load"):
        lines.append(f"load: {situation['load']}")
    if situation.get("note"):
        lines.append(str(situation["note"]))
    return "\n".join(lines)


def _loads(values: list[Any]) -> str:
    return " ".join("--" if v is None else f"{float(v) * 100:.0f}%" for v in values)


def format_groups(state: dict[str, Any]) -> str:
    groups = state.get("groups") or []
    cores = [g for g in groups if g.get("level") == "core"]
    show_all_cores = len(cores) <= 16
    lines = []
    for group in groups:
        if group.get("level") == "core" and not show_all_cores and not group.get("daemons"):
            continue
        daemons = ", ".join(group.get("daemons") or []) or "-"
        cpus = group.get("linux_cpus") or []
        span = f"cpus {cpus[0]}-{cpus[-1]}" if len(cpus) > 1 else f"cpu {cpus[0]}" if cpus else "no cpus"
        lines.append(
            f"{group['name']}  {group.get('level')}  load {_loads(group.get('loads') or [])}  "
            f"allocatable {group.get('allocatable', 0)}  {span}  daemons: {daemons}"
        )
    if not show_all_cores:
        lines.append(f"({len(cores)} cores; only cores with daemons are listed)")
    return "\n".join(lines) or "not captured"


def format_daemons(state: dict[str, Any]) -> str:
    level = state.get("move_level") or "node"
    by_name = {g["name"]: g for g in state.get("groups") or []}
    lines = []
    for daemon in state.get("daemons") or []:
        on = [g for g in daemon.get("groups") or [] if (by_name.get(g) or {}).get("level") == level]
        scale = daemon.get("scale") or {}
        limits = f"scale {scale.get('min')}..{scale.get('max')}" if scale.get("enabled") else "scale off"
        parts = [
            f"{c['name']} {'on' if c.get('enabled') else 'OFF'} cost {c.get('cost')}" for c in daemon.get("components") or []
        ]
        components = "; ".join(parts) if parts else "no optional components"
        cpus = daemon.get("cpus") or []
        lines.append(
            f"{daemon['daemon_id']}  cpus {','.join(str(c) for c in cpus) or '?'} ({len(cpus)})  on {', '.join(on) or '?'}  "
            f"{limits}  {components}"
        )
    return "\n".join(lines) or "no daemons attached"


def format_history(rows: list[dict[str, Any]] | None) -> str:
    """Same as infrabench llm/view.py format_history."""
    if not rows:
        return "no action taken yet"
    lines = []
    for row in rows:
        when = row.get("at_seconds")
        text = f"t+{float(when):.0f}s  " if when is not None else ""
        text += str(row.get("action") or "")
        for key in ("workload", "node", "cpus", "component"):
            if row.get(key) not in (None, ""):
                text += f" {row[key]}"
        if row.get("error"):
            text += f"  FAILED: {row['error']}"
        elif row.get("action") in {"none", "stop"}:
            if row.get("reason"):
                text += f"  ({row['reason']})"
        else:
            text += "  applied"
            if row.get("reason"):
                text += f"  ({row['reason']})"
        lines.append(text)
    return "\n".join(lines)


def format_events(rows: list[dict[str, Any]] | None) -> str:
    rows = [r for r in rows or [] if r.get("policy") != "zeroctl" and r.get("action") not in ("attach", "detach")]
    if not rows:
        return "none"
    out = []
    for row in rows[-10:]:
        bits = [str(row.get("action")), str(row.get("daemon_id", ""))]
        for key in ("from", "to", "component", "parameter", "value"):
            if row.get(key) not in (None, ""):
                bits.append(f"{key} {row[key]}")
        out.append(f"tick {row.get('tick')}: {' '.join(bits)}")
    return "\n".join(out)


def constraint_lines(state: dict[str, Any]) -> str:
    names = [d["daemon_id"] for d in state.get("daemons") or []]
    lines = [f"- Daemons you may change: {', '.join(names) or 'none'}."]
    optional = sorted({c["name"] for d in state.get("daemons") or [] for c in d.get("components") or []})
    if optional:
        lines.append(f"- Optional components: {', '.join(optional)}. Everything else in a program is critical.")
    lines.append("- Never stop or restart a daemon; Mercury keeps the pid.")
    return "\n".join(lines)


def build_prompt(state: dict[str, Any], situation: dict[str, Any] | None = None) -> str:
    template = TEMPLATE.read_text(encoding="utf-8")
    return template.format(
        situation=format_situation(situation, state),
        group_state=format_groups(state),
        daemon_state=format_daemons(state),
        events=format_events(state.get("events")),
        history=format_history(state.get("history")),
        constraints=constraint_lines(state),
        move_level=state.get("move_level") or "node",
        mercury_cpu=state.get("mercury_cpu", 0),
        max_cpus=state.get("max_cpus", 16),
        action_guide="\n".join(f"{name} {summary}" for name, summary in ACTIONS),
        actions=" | ".join(ACTION_NAMES),
    )


# --------------------------------------------------------------------------- client


def _http_json(url: str, payload: dict[str, Any], headers: dict[str, str]) -> dict[str, Any]:
    body = json.dumps(payload).encode("utf-8")
    request = Request(url, data=body, headers={"Content-Type": "application/json", **headers}, method="POST")
    try:
        with urlopen(request, timeout=TIMEOUT_S) as response:
            return json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")[:500]
        raise RuntimeError(f"LLM HTTP {exc.code}: {detail}") from exc
    except URLError as exc:
        raise RuntimeError(f"LLM request failed: {exc}") from exc


def _openai(prompt: str) -> str:
    key = os.environ.get("OPENAI_API_KEY")
    if not key:
        raise RuntimeError("OPENAI_API_KEY is not set")
    model = os.environ.get("OPENAI_MODEL", "gpt-4o-mini")
    base = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com/v1").rstrip("/")
    data = _http_json(
        f"{base}/chat/completions",
        {"model": model, "messages": [{"role": "system", "content": GOAL}, {"role": "user", "content": prompt}]},
        {"Authorization": f"Bearer {key}"},
    )
    return str(((data.get("choices") or [{}])[0].get("message") or {}).get("content") or "")


def _gemini(prompt: str) -> str:
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        raise RuntimeError("GEMINI_API_KEY is not set")
    model = os.environ.get("GEMINI_MODEL", "gemini-2.5-flash")
    base = os.environ.get("GEMINI_BASE_URL", "https://generativelanguage.googleapis.com/v1beta").rstrip("/")
    # Key in a header rather than the query string, so it never lands in a URL or log.
    data = _http_json(
        f"{base}/models/{model}:generateContent",
        {"contents": [{"parts": [{"text": prompt}]}]},
        {"x-goog-api-key": key},
    )
    parts = (((data.get("candidates") or [{}])[0].get("content") or {}).get("parts") or [])
    return "".join(str(part.get("text") or "") for part in parts)


def mock_reply(state: dict[str, Any]) -> str:
    """Deterministic stand-in for a model. Replays ZEROCTL_MOCK_RESPONSE when set."""
    canned = os.environ.get("ZEROCTL_MOCK_RESPONSE")
    if canned:
        return Path(canned).read_text(encoding="utf-8")
    level = state.get("move_level") or "node"
    groups = {g["name"]: g for g in state.get("groups") or []}
    level_groups = [g for g in groups.values() if g.get("level") == level and g.get("allocatable", 0) > 0]

    def latest(group: dict[str, Any]) -> float:
        loads = [v for v in group.get("loads") or [] if v is not None]
        return float(loads[-1]) if loads else 0.0

    for daemon in state.get("daemons") or []:
        here = [groups[g] for g in daemon.get("groups") or [] if g in groups and groups[g].get("level") == level]
        hot = [g for g in here if latest(g) > 0.75]
        if not hot:
            continue
        need = max(len(daemon.get("cpus") or []), 1)
        options = [
            g for g in level_groups
            if g["name"] not in (daemon.get("groups") or []) and latest(g) < 0.75 and g.get("allocatable", 0) >= need
        ]
        if options:
            target = min(options, key=lambda g: (latest(g), (g.get("linux_cpus") or [0])[0]))
            return (
                "```yaml\n"
                f"action: move\nworkload: {daemon['daemon_id']}\nnode: {target['name']}\n"
                f"reason: {hot[0]['name']} is at {latest(hot[0]) * 100:.0f}% and {target['name']} is the least loaded\n"
                "```\n"
            )
        for component in daemon.get("components") or []:
            if component.get("enabled"):
                return (
                    f"action: degrade\nworkload: {daemon['daemon_id']}\ncomponent: {component['name']}\n"
                    f"reason: no cooler {level} group; shed optional work\n"
                )
    return "action: none\nreason: no group above 75%\n"


def complete(prompt: str, llm: str, state: dict[str, Any]) -> str:
    kind = (llm or "openai").lower()
    if kind == "none":
        raise RuntimeError("LLM disabled")
    if kind == "mock":
        return mock_reply(state)
    if kind in {"gemini", "gimini"}:
        return _gemini(prompt)
    if kind == "openai":
        return _openai(prompt)
    raise RuntimeError(f"unknown llm {llm!r}")


# --------------------------------------------------------------------------- parse


def _extract_yaml(text: str) -> str:
    """Same as infrabench llm/parse.py: the first fenced block, else the whole text."""
    fenced = re.search(r"```(?:yaml|yml|json)?\s*\n(.*?)```", text, re.DOTALL | re.IGNORECASE)
    if fenced:
        return fenced.group(1).strip()
    return text.strip()


def _scalar(text: str) -> Any:
    text = text.strip()
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "'\"":
        return text[1:-1]
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if re.fullmatch(r"-?\d+\.\d*", text):
        return float(text)
    return text


def parse_docs(text: str) -> list[dict[str, Any]]:
    """Flat YAML documents (or one JSON object) from the model reply.

    The output schema is one flat mapping, so this reads ``key: value`` lines and
    ``---`` separators without needing PyYAML on the server.
    """
    payload = _extract_yaml(text)
    if payload.startswith("{"):
        try:
            doc = json.loads(payload)
            return [doc] if isinstance(doc, dict) else []
        except json.JSONDecodeError:
            pass
    docs: list[dict[str, Any]] = []
    current: dict[str, Any] = {}
    for raw in payload.splitlines():
        line = raw.split(" #", 1)[0].rstrip()
        if line.strip() in ("---", "..."):
            if current:
                docs.append(current)
            current = {}
            continue
        match = re.match(r"^\s*-?\s*([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$", line)
        if match:
            current[match.group(1)] = _scalar(match.group(2))
    if current:
        docs.append(current)
    return docs


def to_action(docs: list[dict[str, Any]]) -> dict[str, Any]:
    """The first document with an action, reduced to Mercury's fields. Bad output is none."""
    for doc in docs:
        if "action" not in doc:
            continue
        action = str(doc.get("action") or "").strip().lower()
        out: dict[str, Any] = {"action": action}
        for key in ("workload", "node", "component", "reason"):
            if doc.get(key) not in (None, ""):
                out[key] = str(doc[key]).strip()
        cpus = doc.get("cpus", doc.get("replicas"))
        if cpus not in (None, ""):
            try:
                out["cpus"] = int(cpus)
            except (TypeError, ValueError):
                return {"action": "none", "error": f"cpus {cpus!r} is not an integer"}
        if action not in ACTION_NAMES:
            return {"action": "none", "error": f"model proposed unknown action {action!r}"}
        return out
    return {"action": "none", "error": "model reply had no action document"}


# --------------------------------------------------------------------------- main


def step(state: dict[str, Any], llm: str, log_dir: Path | None = None) -> dict[str, Any]:
    situation = None
    situation_file = os.environ.get("ZEROCTL_SITUATION_FILE")
    if situation_file and Path(situation_file).is_file():
        try:
            situation = json.loads(Path(situation_file).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            situation = None
    prompt = build_prompt(state, situation)
    stamp = time.strftime("%Y%m%dT%H%M%S")
    if log_dir is not None:
        log_dir.mkdir(parents=True, exist_ok=True)
        (log_dir / f"{stamp}-prompt.txt").write_text(prompt, encoding="utf-8")
    try:
        reply = complete(prompt, llm, state)
    except Exception as exc:  # network, quota, missing key
        action = {"action": "none", "error": str(exc)}
    else:
        if log_dir is not None:
            (log_dir / f"{stamp}-response.txt").write_text(reply, encoding="utf-8")
        action = to_action(parse_docs(reply))
    if log_dir is not None:
        (log_dir / f"{stamp}-action.json").write_text(json.dumps(action) + "\n", encoding="utf-8")
    return action


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--llm", default=os.environ.get("ZEROCTL_LLM", "openai"), choices=("openai", "gemini", "mock", "none"))
    parser.add_argument("--log-dir", default=None, help="write <stamp>-prompt.txt / -response.txt / -action.json here")
    parser.add_argument("--state", default="-", help="state JSON file (default stdin)")
    parser.add_argument("--print-prompt", action="store_true", help="print the rendered prompt and exit")
    args = parser.parse_args(argv)
    try:
        text = sys.stdin.read() if args.state == "-" else Path(args.state).read_text(encoding="utf-8")
        state = json.loads(text)
    except Exception as exc:
        print(json.dumps({"action": "none", "error": f"bad state: {exc}"}))
        return 0
    if args.print_prompt:
        print(build_prompt(state))
        return 0
    action = step(state, args.llm, Path(args.log_dir) if args.log_dir else None)
    print(json.dumps(action))
    return 0


if __name__ == "__main__":
    sys.exit(main())
