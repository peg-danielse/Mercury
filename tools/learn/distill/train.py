#!/usr/bin/env python3
"""Distil the llm controller's decisions into a small transformer (on pd-gpu): episodes.jsonl from reconstruct.py in,
model.pt and results.json out.

  train.py episodes.jsonl OUT [--d 64] [--layers 3] [--steps 4000] [--test-frac 0.15] [--seed 0]

Tokens: one per L3 domain (16), one per service, one global (pool, budget, latency, shedding, previous action). The
global token's output gives the action (none, grow_pool, shrink_pool, swap_domain, move_daemon, scale_daemon,
add_replica, remove_replica, shed_route, restore_route). Pointer heads over the domain tokens give the target
domain(s) (grow/shrink also "auto"), over the service tokens the service to move, scale, or replicate, and a route
head the route to shed or restore. scale_daemon also has a head for its signed CPU count, -64..-1 and 1..64.
The label is the episode's first call that was accepted (ok true). A timed-out or refused call is skipped. A hold,
an episode with no call, and an episode whose calls all failed are "none". --arch mlp or gru uses the same tokens
and heads with a different trunk; tf is the transformer. Which runs are in episodes.jsonl is the other filter:
keep_runs.py keeps a run only when its critical p95 and its error rate are both under the given limits. Runs are
split, not episodes: whole runs are held out for the test, so the score is on runs the model never saw. Needs torch.
"""

from __future__ import annotations

import argparse
import collections
import json
import math
import os
import random

import torch
import torch.nn as nn
import torch.nn.functional as F

ACTIONS = ["none", "grow_pool", "shrink_pool", "swap_domain", "move_daemon", "scale_daemon",
           "add_replica", "remove_replica", "shed_route", "restore_route"]
# Signed CPU counts scale_daemon may ask for. 0 is not a call.
CPU_VALUES = [i for i in range(-64, 65) if i != 0]
ROUTES = ["recommendations", "restaurants", "museums", "cinema", "review", "follow", "read-user-timeline"]
NDOM, MAXD = 16, 32
DOM_F, SVC_F, GLB_F = 10, 6 + NDOM, 48


def lg(x: float | None) -> float:
    return math.log1p(max(x or 0.0, 0.0)) / 8


def dom_index(name: str | None) -> int:
    try:
        return int(str(name).split(".")[1])
    except (IndexError, ValueError):
        return -1


def accepted(calls: list | None) -> list:
    """Calls Mercury carried out. A missing ok is an old episode and counts as accepted."""
    return [c for c in (calls or []) if c.get("ok", True)]


def cpu_index(value) -> int:
    """Class of a signed CPU count. Values outside -64..64 clamp. 0 is not a class."""
    try:
        count = int(value)
    except (TypeError, ValueError):
        return -100
    if count == 0:
        return -100
    count = max(-64, min(64, count))
    return CPU_VALUES.index(count)


def encode(e: dict, names: dict[str, int], actions: list[str] | None = None, glb_f: int | None = None) -> dict:
    """One episode. actions and glb_f are the head and the global width this model trained with. The live loader
    passes the checkpoint's, so an older model still sees the feature layout it trained on."""
    actions = ACTIONS if actions is None else actions
    s = e["state"]
    pool = s.get("pool") or {}
    free = set(pool.get("free_domains") or [])
    dom = torch.zeros(NDOM, DOM_F)
    for b in s["l3"]:
        i = dom_index(b["name"])
        if 0 <= i < NDOM:
            n = max(len(b["cpus"]), 1)
            dom[i] = torch.tensor([float(b["in_pool"]), b["load"], len(b["free"]) / n, len(b["daemons"]) / 8,
                                   b.get("foreign_cpus", 0) / n, b.get("ipc", 0) / 3, lg(b.get("llc_per_ms")),
                                   b.get("cpu_wait", 0), float(b["name"] in free), 1.0])
    svc = torch.zeros(MAXD, SVC_F)
    svc_id = torch.zeros(MAXD, dtype=torch.long)
    svc_mask = torch.zeros(MAXD, dtype=torch.bool)
    order = []
    for k, d in enumerate(sorted(s["daemons"], key=lambda d: d["daemon_id"])[:MAXD]):
        cpus = d.get("cpus") or []
        share = torch.zeros(NDOM)
        for c in cpus:
            j = (c % 32) // 2
            share[j] += 1 / len(cpus)
        svc[k, :6] = torch.tensor([d.get("cores", len(cpus)) / 16, d.get("cpu_use", 0) / 16, d.get("occupancy", 0),
                                   d.get("pressure", 0), float(d.get("in_pool", True)), float(d["daemon_id"].endswith("loadgen"))])
        svc[k, 6:] = share
        short = d["daemon_id"].split("-", 1)[-1]
        svc_id[k] = names.setdefault(short, len(names) + 1) if len(names) < 255 else 0
        svc_mask[k] = True
        order.append(d["daemon_id"])
    lat = s.get("latency") or {}
    now = (lat.get("now") or {})
    hist = (lat.get("p95_history_ms") or [])[-6:]
    hist = [None] * (6 - len(hist)) + hist
    crit = lat.get("critical") or {}
    deg = s.get("degrade")
    shed = set((deg or {}).get("shed") or [])
    prev = torch.zeros(len(actions))
    prev_names = [c["name"] for c in accepted(e.get("previous")) if c["name"] in actions]
    for p in prev_names:
        prev[actions.index(p)] = 1
    mx = pool.get("max") or e.get("budget") or 0
    room = float(mx == 0 or pool.get("cores", 0) + 4 <= mx)
    glb = [pool.get("cores", 0) / 64, mx / 64, pool.get("start", 8) / 64, pool.get("busy", 0), pool.get("pressure", 0),
           pool.get("allocated", 0) / 100, room, len(free) / NDOM,
           lg(now.get("p95_ms")), lg(lat.get("reference_p95_ms")),
           (now.get("p95_ms") or 0) / max(lat.get("reference_p95_ms") or 1, 1) / 10, now.get("error_rate") or 0,
           *[lg(h) for h in hist], lg(crit.get("p95_ms")), lg(crit.get("reference_p95_ms")), crit.get("error_rate") or 0,
           float(deg is not None), *[float(r in shed) for r in ROUTES], min((e.get("t_s") or 0) / 660, 2.0)]
    width = GLB_F if glb_f is None else glb_f
    pad = width - len(prev) - len(glb)
    if pad < 0:
        raise ValueError(f"global features {len(glb) + len(prev)} exceed {width}")
    glb = torch.tensor(glb + [0.0] * pad + prev.tolist())
    # Label: the first call that happened. A later accepted call stands when the first one timed out.
    call = (accepted(e.get("calls")) or [{"name": "none", "input": {}}])[0]
    act = call["name"] if call["name"] in actions else "none"
    inp = call.get("input") or {}
    y = {"act": actions.index(act), "dom1": -100, "dom2": -100, "svc": -100, "route": -100, "cpus": -100}
    if act in ("grow_pool", "shrink_pool"):
        y["dom1"] = NDOM if inp.get("l3") in (None, "", "auto") else dom_index(inp.get("l3"))
    elif act == "swap_domain":
        y["dom1"], y["dom2"] = dom_index(inp.get("from_l3")), dom_index(inp.get("to_l3"))
    elif act == "move_daemon":
        y["svc"] = order.index(inp.get("daemon_id")) if inp.get("daemon_id") in order else -100
        y["dom1"], y["dom2"] = dom_index(inp.get("from_l3")), dom_index(inp.get("to_l3"))
    elif act == "scale_daemon":
        y["svc"] = order.index(inp.get("daemon_id")) if inp.get("daemon_id") in order else -100
        y["cpus"] = cpu_index(inp.get("cpus"))
    elif act in ("add_replica", "remove_replica"):
        y["svc"] = order.index(inp.get("daemon_id")) if inp.get("daemon_id") in order else -100
    elif act in ("shed_route", "restore_route"):
        y["route"] = ROUTES.index(inp.get("route")) if inp.get("route") in ROUTES else -100
    for k in ("dom1", "dom2"):
        if y[k] < 0:
            y[k] = -100
    return {"dom": dom, "svc": svc, "svc_id": svc_id, "svc_mask": svc_mask, "glb": glb, **{"y_" + k: torch.tensor(v) for k, v in y.items()}}


class Distilled(nn.Module):
    def __init__(self, d: int, layers: int, heads: int, drop: float, banned: list[int] | None = None,
                 actions: list[str] | None = None, arch: str = "tf", glb_f: int | None = None):
        super().__init__()
        self.actions = list(ACTIONS if actions is None else actions)
        self.arch = arch
        self.glb_f = GLB_F if glb_f is None else glb_f
        self.has_cpus = "scale_daemon" in self.actions
        mask = torch.zeros(len(self.actions))
        mask[banned or []] = -1e9
        self.register_buffer("act_mask", mask)  # actions this model may never choose (--no-shed: shed/restore)
        self.dom_in = nn.Linear(DOM_F, d)
        self.dom_pos = nn.Embedding(NDOM, d)
        self.svc_in = nn.Linear(SVC_F, d)
        self.svc_name = nn.Embedding(256, d)
        self.glb_in = nn.Linear(self.glb_f, d)
        self.kind = nn.Embedding(3, d)
        if arch == "tf":
            layer = nn.TransformerEncoderLayer(d, heads, 4 * d, drop, batch_first=True, norm_first=True)
            self.enc = nn.TransformerEncoder(layer, layers)
        elif arch == "gru":
            self.rnn = nn.GRU(d, d, layers, batch_first=True, dropout=drop if layers > 1 else 0.0)
        elif arch == "mlp":
            self.mlp = nn.ModuleList(nn.Sequential(nn.Linear(d, 4 * d), nn.GELU(), nn.Dropout(drop),
                                                   nn.Linear(4 * d, d)) for _ in range(layers))
        else:
            raise ValueError(f"unknown arch {arch}")
        self.act = nn.Linear(d, len(self.actions))
        self.cpus = nn.Linear(d, len(CPU_VALUES)) if self.has_cpus else None
        self.auto = nn.Linear(d, 1)  # grow/shrink "auto": an extra pointer slot
        self.q1, self.q2, self.qs = nn.Linear(d, d), nn.Linear(d, d), nn.Linear(d, d)
        self.route = nn.Linear(d, len(ROUTES))

    def forward(self, b):
        B = b["glb"].shape[0]
        dom = self.dom_in(b["dom"]) + self.dom_pos.weight[None] + self.kind.weight[0]
        svc = self.svc_in(b["svc"]) + self.svc_name(b["svc_id"]) + self.kind.weight[1]
        glb = (self.glb_in(b["glb"]) + self.kind.weight[2])[:, None]
        x = torch.cat([glb, dom, svc], 1)
        pad = torch.cat([torch.zeros(B, 1 + NDOM, dtype=torch.bool, device=x.device), ~b["svc_mask"]], 1)
        if self.arch == "tf":
            h = self.enc(x, src_key_padding_mask=pad)
            g = h[:, 0]
        elif self.arch == "gru":
            h, _ = self.rnn(x.masked_fill(pad[:, :, None], 0))
            last = (~pad).sum(1).clamp(min=1) - 1
            g = h[torch.arange(B, device=h.device), last]
        else:
            h = x.masked_fill(pad[:, :, None], 0)
            for block in self.mlp:
                h = h + block(h)
            g = h[:, 0]
        hd, hs = h[:, 1:1 + NDOM], h[:, 1 + NDOM:]
        scale = 1 / math.sqrt(h.shape[-1])
        d1 = torch.cat([(hd @ self.q1(g)[:, :, None]).squeeze(-1) * scale, self.auto(g)], 1)  # NDOM + auto
        d2 = (hd @ self.q2(g)[:, :, None]).squeeze(-1) * scale
        sv = (hs @ self.qs(g)[:, :, None]).squeeze(-1) * scale
        sv = sv.masked_fill(~b["svc_mask"], -1e9)
        out = {"act": self.act(g) + self.act_mask, "dom1": d1, "dom2": d2, "svc": sv, "route": self.route(g)}
        if self.cpus is not None:
            out["cpus"] = self.cpus(g)
        return out


def collate(items: list[dict], dev: str) -> dict:
    return {k: torch.stack([it[k] for it in items]).to(dev) for k in items[0]}


def loss_and_hits(out: dict, b: dict, w_act: torch.Tensor) -> tuple[torch.Tensor, dict]:
    loss = F.cross_entropy(out["act"], b["y_act"], weight=w_act)
    hits = {"act": (out["act"].argmax(1) == b["y_act"]).float().sum().item(), "n": len(b["y_act"])}
    for k in ("dom1", "dom2", "svc", "route", "cpus"):
        if k not in out:
            continue
        m = b["y_" + k] != -100
        if m.any():
            loss = loss + F.cross_entropy(out[k][m], b["y_" + k][m])
            hits[k] = (out[k][m].argmax(1) == b["y_" + k][m]).float().sum().item()
            hits[k + "_n"] = int(m.sum())
    return loss, hits


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("episodes"); ap.add_argument("out")
    ap.add_argument("--d", type=int, default=64); ap.add_argument("--layers", type=int, default=3)
    ap.add_argument("--heads", type=int, default=4); ap.add_argument("--drop", type=float, default=0.1)
    ap.add_argument("--steps", type=int, default=4000); ap.add_argument("--batch", type=int, default=64)
    ap.add_argument("--lr", type=float, default=3e-4); ap.add_argument("--test-frac", type=float, default=0.15)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--arch", choices=("tf", "gru", "mlp"), default="tf",
                    help="trunk over the same tokens: transformer, GRU, or per-token MLP")
    ap.add_argument("--no-shed", action="store_true",
                    help="only runs without shedding (llmN, not llmdegN); shed_route and restore_route are never chosen")
    ap.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    a = ap.parse_args()
    random.seed(a.seed); torch.manual_seed(a.seed)
    eps = [json.loads(l) for l in open(a.episodes)]
    if a.no_shed:
        eps = [e for e in eps if not e.get("shedding")]
    runs = sorted({e["run"] for e in eps})
    random.shuffle(runs)
    test_runs = set(runs[:max(1, int(len(runs) * a.test_frac))])
    names: dict[str, int] = {}
    if len(runs) == 1:  # one run (a smoke test): hold out its last episodes instead
        cut = int(len(eps) * (1 - a.test_frac))
        data = [(encode(e, names), i >= cut) for i, e in enumerate(eps)]
    else:
        data = [(encode(e, names), e["run"] in test_runs) for e in eps]
    tr = [x for x, t in data if not t]; te = [x for x, t in data if t]
    counts = collections.Counter(int(x["y_act"]) for x in tr)
    # Class weights: sqrt-inverse frequency, so rarer actions are not drowned by grow/move.
    w_act = torch.tensor([(len(tr) / max(counts.get(i, 0), 1)) ** 0.5 for i in range(len(ACTIONS))], device=a.device)
    w_act = w_act / w_act.mean()
    print(f"{len(eps)} episodes from {len(runs)} runs: train {len(tr)}, test {len(te)} ({len(test_runs)} runs held out)")
    print("train actions:", {ACTIONS[k]: v for k, v in sorted(counts.items())})
    model = Distilled(a.d, a.layers, a.heads, a.drop, banned=[ACTIONS.index("shed_route"), ACTIONS.index("restore_route")]
                      if a.no_shed else [], arch=a.arch).to(a.device)
    print("parameters:", sum(p.numel() for p in model.parameters()))
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, a.lr, total_steps=a.steps, pct_start=0.1)

    def evaluate(items: list[dict]) -> dict:
        model.eval()
        tot: collections.Counter = collections.Counter()
        conf = torch.zeros(len(ACTIONS), len(ACTIONS), dtype=torch.long)
        with torch.no_grad():
            for i in range(0, len(items), 256):
                b = collate(items[i:i + 256], a.device)
                out = model(b)
                _, h = loss_and_hits(out, b, w_act)
                tot.update(h)
                for t, p in zip(b["y_act"].tolist(), out["act"].argmax(1).tolist()):
                    conf[t, p] += 1
        model.train()
        res = {"act_acc": tot["act"] / max(tot["n"], 1)}
        for k in ("dom1", "dom2", "svc", "route", "cpus"):
            if tot[k + "_n"]:
                res[k + "_acc"] = tot[k] / tot[k + "_n"]
        recall = {ACTIONS[i]: round(conf[i, i].item() / max(conf[i].sum().item(), 1), 3) for i in range(len(ACTIONS)) if conf[i].sum()}
        res["recall"] = recall
        res["confusion"] = conf.tolist()
        return res

    log = []
    for step in range(1, a.steps + 1):
        b = collate(random.sample(tr, min(a.batch, len(tr))), a.device)
        loss, _ = loss_and_hits(model(b), b, w_act)
        opt.zero_grad(); loss.backward(); nn.utils.clip_grad_norm_(model.parameters(), 1.0); opt.step(); sched.step()
        if step % 500 == 0 or step == a.steps:
            r = evaluate(te)
            log.append({"step": step, "loss": loss.item(), **{k: v for k, v in r.items() if k != "confusion"}})
            print(step, f"loss {loss.item():.3f}", {k: (round(v, 3) if isinstance(v, float) else v) for k, v in r.items() if k != "confusion"})
    os.makedirs(a.out, exist_ok=True)
    torch.save({"model": model.state_dict(), "args": vars(a), "names": names, "actions": ACTIONS,
                "routes": ROUTES, "cpu_values": CPU_VALUES, "glb_f": GLB_F},
               os.path.join(a.out, "model.pt"))
    # Majority baseline: always the most common training action.
    maj = counts.most_common(1)[0][0]
    final = evaluate(te)
    json.dump({"final": final, "log": log, "test_runs": sorted(test_runs), "baseline_majority_act_acc":
               sum(int(x["y_act"]) == maj for x in te) / max(len(te), 1)}, open(os.path.join(a.out, "results.json"), "w"), indent=1)
    print("majority baseline act_acc", round(sum(int(x["y_act"]) == maj for x in te) / max(len(te), 1), 3))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
