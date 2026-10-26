"""Train and compare sequence models (slo/seq_models.py) on train_slo.py's cases, against the GNN on the same split.

python slo/train_seq.py <out_dir> --model mlp|gru|tf [--gnn <weights.json> ...] [options] case=... | --cases FILE

Same cases, labels and loss as train_slo.py at the daemon level: per tick, with T = max(C* - 0.3, 0.3) and P the model's
core demand, (1 + e) relu(T - P)^2 + lambda relu(P - T), plus lambda_chg * 100 * relu(|P(t) - P(t-1)| - 0.25) on
consecutive valid ticks. (train_slo.py's per-thread shape term has no counterpart: these models predict P directly.)
Metrics on the held-out cases, per valid tick: req = ceil(P) clamped to 1..16 against the label round(C*): under %,
over %, core-s requested vs labelled, requested-core changes per second. --gnn weights are evaluated on the same ticks
twice: with train_slo.py's FFD of the per-thread outputs (its own metric) and with ceil(sum of outputs) (this script's).

Results are reported overall and per group (slo2 for cases under a /slo2/ path, v1 otherwise). --cache FILE keeps the
built cases (with the --gnn requests) so later runs skip loading traces; --eval-only reports the --gnn baselines only.

Options: --fine L (fine context ticks, tf) --mem M --bucket B (coarse memory: M tokens of B ticks) --d --layers --heads
--steps --batch --lr --lam [0.03] --lchg [0.1] --device [cuda if available] --eval-every --seed
--init model.pt (start from it; --steps 0 only evaluates it, e.g. on another system's cases).
"""
import argparse, hashlib, json, math, os, re, struct, sys, time
import numpy as np
import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import train_gnn as tg
from mdata import ffd_cores, load_self
from seq_models import CTX_F, CTX_MAX, GRUBase, MLPBase, MemoryTransformer, featurize, n_params

OFF, DB = 0.3, 0.25


def read_mring(path):
    data = open(path, "rb").read()
    _, count, max_counters, record = struct.unpack_from("<4I", data, 8)
    committed, offset = struct.unpack_from("<2Q", data, 24)
    names, at = [], 40
    for _ in range(count):
        (size,) = struct.unpack_from("<I", data, at); names.append(data[at + 4:at + 4 + size].decode()); at += 4 + size
    committed = min(committed, (len(data) - offset) // record)
    rows = np.frombuffer(data, dtype=np.uint64, count=committed * record // 8, offset=offset).reshape(committed, record // 8)
    return rows[:, 0].astype(np.int64), {n: rows[:, 4 + i].astype(np.float64) for i, n in enumerate(names)}


def collection_meta(trace):
    """(group, swept daemons, GK) from collect_slo2.sh's meta.txt four levels above the trace's run dir, else None."""
    c = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(trace))))))
    p = os.path.join(c, "meta.txt")
    if not os.path.exists(p):
        return None
    t = open(p).read()
    route = re.search(r"ROUTES=(\S+)", t).group(1)
    if "-arrivals poisson" in t:  # open-loop collections (collect_open.sh) get their own groups: review@open
        route += "@open"
    return (route, re.search(r'SWEEP="([^"]*)"', t).group(1).split(), int(re.search(r"GK=(\d+)", t).group(1)))


def ring_ctx(run_dir, others, ts, prefix="hotel-"):
    """Context per tick for the other daemons on the route, from their ring archives: [T, CTX_MAX, CTX_F], mask."""
    ctx = np.zeros((len(ts), CTX_MAX, CTX_F), np.float32); m = np.zeros(CTX_MAX, bool)
    for j, o in enumerate(others[:CTX_MAX]):
        p = os.path.join(run_dir, "rings", f"{prefix}{o}.mring")
        if not os.path.exists(p):
            continue
        rts, cols = read_mring(p)
        if len(rts) < 2 or "cores" not in cols:
            continue
        gap = np.maximum(np.diff(rts), 1).astype(np.float64)
        busy = cols["task-clock"][1:] / gap
        psi = cols["pressure"][1:] / gap if "pressure" in cols else np.zeros_like(busy)
        cores = cols["cores"][1:]; dc = np.diff(cols["cores"])
        f = np.stack([busy / 16, psi, cores / 16, dc / 4, busy / np.maximum(cores, 1)], 1).astype(np.float32)
        idx = np.searchsorted(rts[1:], ts, side="right") - 1
        ok = idx >= 0
        ctx[ok, j] = f[idx[ok]]; m[j] = True
    return torch.from_numpy(ctx), torch.from_numpy(m)


def build(spec, cap_weight=0.2):
    """train_slo.build, keeping the allocation (cpuset size per tick) for the decision features."""
    trace, wins, daemon, *flag = spec.split("@")
    d = load_self(os.path.dirname(trace), "runshare") if os.path.basename(trace) == "trace.jsonl" else None
    if d is None:
        tmp = os.path.join("/tmp", "slo-case-" + str(abs(hash(trace))))
        os.makedirs(tmp, exist_ok=True)
        if not os.path.lexists(os.path.join(tmp, "trace.jsonl")):
            os.symlink(os.path.abspath(trace), os.path.join(tmp, "trace.jsonl"))
        d = load_self(tmp, "runshare")
    with open(trace) as f:
        for l in f:
            r = json.loads(l)
            if r["interval_ns"] > 0:
                t0 = r["ts_ns"]; break
    rows = json.load(open(wins)).get(daemon, [])
    ts = t0 + (d["t"] * 1e9).astype(np.int64)
    C = np.full(len(ts), np.nan); E = np.zeros(len(ts))
    for s, e, c, sc, _ in rows:
        m = (ts > s) & (ts <= e); C[m] = c; E[m] = 0 if not np.isfinite(sc) else max(0.0, sc - 1)
    valid = d["valid"] & np.isfinite(C)
    X, S, _, v = tg.case_tensors(d)
    alloc = np.asarray(d["cpus"], dtype=np.float32)
    threads, mask, dfeat = featurize(X, alloc)
    run = os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(trace)))))
    meta = collection_meta(trace)
    prefix = daemon[:daemon.index("-") + 1]  # hotel- or social-
    me = daemon.removeprefix(prefix)
    if meta:
        route, sweep, gk = meta
        ctx, ctx_mask = ring_ctx(os.path.dirname(trace), [o for o in sweep if o != me], ts, prefix)
        group = route
    else:
        gk = 10 ** 9; ctx = torch.zeros((len(ts), CTX_MAX, CTX_F)); ctx_mask = torch.zeros(CTX_MAX, dtype=torch.bool)
        group = "slo2" if "/slo2/" in trace else "v1"
    capped = np.nan_to_num(C) >= gk  # C* = the generous size: "at least GK" (closed-loop ceiling), down-weighted
    W = np.where(capped, cap_weight, 1.0).astype(np.float32)
    return {"name": f"{group}/{run}/{daemon}", "group": group, "daemon_id": daemon, "test": bool(flag),
            "X": X, "S": S, "threads": threads, "mask": mask, "daemon": dfeat, "ctx": ctx, "ctx_mask": ctx_mask,
            "C": torch.tensor(np.nan_to_num(C), dtype=torch.float32), "E": torch.tensor(E, dtype=torch.float32),
            "W": torch.from_numpy(W), "capped": torch.from_numpy(capped),
            "v": torch.tensor(valid), "n": int(valid.sum()), "T": len(ts)}


def loss_fn(P, C, E, v, lam, lchg, W=None):
    T = torch.clamp(C - OFF, min=OFF)
    l = (1 + E) * torch.relu(T - P) ** 2 + lam * torch.relu(P - T)
    if W is None:
        W = torch.ones_like(l)
    out = (l * W)[v].sum() / W[v].sum().clamp(min=1e-9) if v.any() else P.sum() * 0
    if lchg > 0:
        vv = v[..., 1:] & v[..., :-1]
        if vv.any():
            out = out + lchg * torch.relu((P[..., 1:] - P[..., :-1]).abs() - DB)[vv].mean() * 100
    return out


def metrics(pairs):
    """pairs: list of (P [T] numpy, C [T], v [T] bool) or (req, C, v) with req already integer when tagged."""
    under = over = n = 0; cs_req = cs_lab = 0.0; changes = 0; secs = 0.0
    for req, C, v in pairs:
        lab = np.round(C)
        ch = (req[1:] != req[:-1]) & v[1:] & v[:-1]
        changes += int(ch.sum()); secs += 0.01 * float((v[1:] & v[:-1]).sum())
        under += int((req[v] < lab[v]).sum()); over += int((req[v] > lab[v]).sum()); n += int(v.sum())
        cs_req += float(req[v].sum()) * 0.01; cs_lab += float(lab[v].sum()) * 0.01
    return {"ticks": n, "under_%": 100 * under / max(n, 1), "over_%": 100 * over / max(n, 1), "core_s_pred": cs_req,
            "core_s_label": cs_lab, "req_changes_per_s": changes / max(secs, 1e-9)}


def ceil_req(P):
    return np.clip(np.ceil(P - 1e-3), 1, 16)


def grouped(cs, req_of):
    """metrics overall, overall without capped labels (C* = GK), and per group; req_of(case) -> request per tick."""
    reqs = {id(c): req_of(c) for c in cs}
    out = {"all": metrics([(reqs[id(c)], c["C"].numpy(), c["v"].numpy()) for c in cs]),
           "all_uncapped": metrics([(reqs[id(c)], c["C"].numpy(), c["v"].numpy() & ~c["capped"].numpy()) for c in cs])}
    for g in sorted({c["group"] for c in cs}):
        out[g] = metrics([(reqs[id(c)], c["C"].numpy(), c["v"].numpy()) for c in cs if c["group"] == g])
    return out


def gnn_requests(path, c, cache={}):
    """(FFD of per-thread outputs, ceil of their sum) per tick for one case; computed while X and S are loaded."""
    if path not in cache:
        W = json.load(open(path))
        m = tg.LocalGNNDB(len(tg.F_NAMES)); m.load_state_dict({k: torch.tensor(v) for k, v in W.items()}); m.eval()
        cache[path] = m
    with torch.no_grad():
        X = c["X"].clone(); X[:, :, 7] = 0
        p = cache[path](X, c["S"])[:, :-1].numpy()
    return np.array([ffd_cores(r) for r in p], dtype=np.float64), ceil_req(p.sum(1))


def gnn_eval(path, cs):
    return {"ffd": grouped(cs, lambda c: c["gnn"][path][0]), "ceil_sum": grouped(cs, lambda c: c["gnn"][path][1])}


def make_model(a):
    if a.model == "mlp":
        return MLPBase(a.d)
    if a.model == "gru":
        return GRUBase(a.d, a.layers)
    return MemoryTransformer(a.d, a.layers, a.heads, a.fine, a.mem, a.bucket, a.drop, ctx=a.ctx)


def hist_of(daemon, s, span):
    """daemon features of the `span` ticks before s, zero-padded before the case start."""
    lo = s - span
    h = daemon[max(lo, 0):s]
    if lo < 0:
        h = torch.cat([torch.zeros(-lo, daemon.shape[1]), h])
    return h


@torch.no_grad()
def predict(model, c, a, dev):
    """P for every tick of a case."""
    model.eval()
    T = c["T"]
    if a.model == "mlp":
        return model(None, None, c["daemon"].to(dev))[0].float().cpu().numpy()
    if a.model == "gru":
        return model(None, None, c["daemon"][None].to(dev))[0][0].float().cpu().numpy()
    # Windows end every L/2 ticks; each fills only the ticks after the previous window's end, so every tick past
    # the first window sees at least L/2 fine ticks plus the coarse memory before its window.
    L, span = a.fine, a.mem * a.bucket
    P = np.zeros(T, np.float32)
    step, done, e = max(1, L // 2), 0, min(L, T)
    while True:
        s0 = max(0, e - L)
        with torch.autocast(dev.type, dtype=torch.bfloat16, enabled=dev.type == "cuda"):
            out = model(c["threads"][None, s0:e].to(dev), c["mask"][None, s0:e].to(dev), c["daemon"][None, s0:e].to(dev),
                        hist_of(c["daemon"], s0, span)[None].to(dev), c["ctx"][None, s0:e].to(dev),
                        c["ctx_mask"][None].to(dev))[0].float().cpu().numpy()
        P[done:e] = out[done - s0:]
        done = e
        if e >= T:
            return P
        e = min(e + step, T)


def evaluate(model, cs, a, dev):
    reqs = {id(c): ceil_req(predict(model, c, a, dev)) for c in cs}
    return grouped(cs, lambda c: reqs[id(c)])


def batch(tr, a, rng, dev):
    """B windows of a.window ticks, cases drawn by valid-tick count."""
    w = np.array([c["n"] for c in tr], dtype=np.float64); w /= w.sum()
    L = a.window
    out = {k: [] for k in ("threads", "mask", "daemon", "hist", "C", "E", "v", "W", "ctx", "ctx_mask")}
    for i in rng.choice(len(tr), a.batch, p=w):
        c = tr[i]; T = c["T"]
        s = int(rng.integers(0, max(1, T - L + 1)))
        e = min(s + L, T); pad = L - (e - s)

        def take(x, fill=0):
            y = x[s:e]
            if pad:
                y = torch.cat([y, torch.full((pad, *y.shape[1:]), fill, dtype=y.dtype)])
            return y
        out["threads"].append(take(c["threads"])); out["mask"].append(take(c["mask"], False))
        out["daemon"].append(take(c["daemon"])); out["C"].append(take(c["C"])); out["E"].append(take(c["E"]))
        out["v"].append(take(c["v"], False)); out["W"].append(take(c["W"]))
        if a.model == "tf":
            out["hist"].append(hist_of(c["daemon"], s, a.mem * a.bucket))
            out["ctx"].append(take(c["ctx"])); out["ctx_mask"].append(c["ctx_mask"])
    return {k: torch.stack(v).to(dev) for k, v in out.items() if v}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--model", choices=["mlp", "gru", "tf"], default="tf")
    ap.add_argument("--gnn", nargs="*", default=[])
    ap.add_argument("--cases", default=None)
    ap.add_argument("--d", type=int, default=128); ap.add_argument("--layers", type=int, default=4)
    ap.add_argument("--heads", type=int, default=4); ap.add_argument("--drop", type=float, default=0.1)
    ap.add_argument("--fine", type=int, default=1024); ap.add_argument("--mem", type=int, default=120)
    ap.add_argument("--bucket", type=int, default=50)
    ap.add_argument("--window", type=int, default=None, help="training window ticks (default: --fine for tf, 1024 otherwise)")
    ap.add_argument("--steps", type=int, default=3000); ap.add_argument("--batch", type=int, default=16)
    ap.add_argument("--lr", type=float, default=3e-4); ap.add_argument("--lam", type=float, default=0.03)
    ap.add_argument("--lchg", type=float, default=0.1); ap.add_argument("--eval-every", type=int, default=500)
    ap.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu"); ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--cache", default=None); ap.add_argument("--eval-only", action="store_true")
    ap.add_argument("--ctx", action="store_true", help="tf: cross-daemon context from the route's other daemons' rings")
    ap.add_argument("--cap-weight", type=float, default=0.2, help="loss weight of ticks whose label is the generous size")
    ap.add_argument("--init", default=None, help="model.pt to start from (same architecture); with --steps 0: evaluate it only")
    a, rest = ap.parse_known_args()
    a.window = a.window or (a.fine if a.model == "tf" else 1024)
    specs = [x.split("=", 1)[1] for x in rest if x.startswith("case=")]
    if a.cases:
        specs += [l.strip().split("=", 1)[1] for l in open(a.cases) if l.startswith("case=")]
    os.makedirs(a.out, exist_ok=True)
    torch.manual_seed(a.seed); rng = np.random.default_rng(a.seed); dev = torch.device(a.device)
    tg.DIST = tg.dtn01_distance()
    t0 = time.time()
    key = hashlib.sha1(json.dumps([specs, sorted(a.gnn), a.cap_weight, "ctx-v1"]).encode()).hexdigest()
    cs = None
    if a.cache and os.path.exists(a.cache):
        blob = torch.load(a.cache, weights_only=False)
        cs = blob["cases"] if blob.get("key") == key else None
    if cs is None:
        cs = []
        for sp in specs:
            c = build(sp, a.cap_weight)
            c["gnn"] = {g: gnn_requests(g, c) for g in a.gnn} if c["test"] else {}
            del c["X"], c["S"]  # graph inputs are large (mongod: ~0.5 GB per case); the sequence models do not use them
            cs.append(c)
        if a.cache:
            torch.save({"key": key, "cases": cs}, a.cache)
    tr = [c for c in cs if not c["test"] and c["n"] > 0]; te = [c for c in cs if c["test"] and c["n"] > 0]
    print(f"{len(tr)} train / {len(te)} test cases, {sum(c['n'] for c in tr)} / {sum(c['n'] for c in te)} ticks, {time.time()-t0:.0f}s", flush=True)
    res = {"args": vars(a), "train": [c["name"] for c in tr], "test": [c["name"] for c in te], "gnn": {}, "evals": []}
    for g in a.gnn:
        res["gnn"][g] = gnn_eval(g, te)
        print("gnn", g, json.dumps(res["gnn"][g]), flush=True)
    if a.eval_only:
        json.dump(res, open(os.path.join(a.out, "results.json"), "w"), indent=1, default=float)
        print("wrote", a.out); return
    model = make_model(a).to(dev)
    res["params"] = n_params(model)
    if a.init:
        model.load_state_dict(torch.load(a.init, map_location=dev)); res["init"] = a.init
    if a.steps == 0:
        ev = evaluate(model, te, a, dev); ev["step"] = 0; res["evals"].append(ev); res["final"] = ev
        print(json.dumps(ev), flush=True)
        json.dump(res, open(os.path.join(a.out, "results.json"), "w"), indent=1, default=float)
        print("wrote", a.out); return
    print(f"{a.model}: {res['params']:,} parameters on {dev}", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=a.lr, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=a.lr, total_steps=a.steps, pct_start=0.05)
    best = None
    for step in range(1, a.steps + 1):
        model.train()
        b = batch(tr, a, rng, dev)
        with torch.autocast(dev.type, dtype=torch.bfloat16, enabled=dev.type == "cuda"):
            if a.model == "tf":
                P = model(b["threads"], b["mask"], b["daemon"], b["hist"], b["ctx"], b["ctx_mask"])
            else:
                P = model(None, None, b["daemon"])[0]
        loss = loss_fn(P.float(), b["C"], b["E"], b["v"], a.lam, a.lchg, b["W"])
        opt.zero_grad(); loss.backward(); torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0); opt.step(); sched.step()
        if step % a.eval_every == 0 or step == a.steps:
            ev = evaluate(model, te, a, dev); ev["step"] = step; ev["loss"] = loss.item(); ev["s"] = time.time() - t0
            res["evals"].append(ev)
            print(json.dumps(ev), flush=True)
            torch.save(model.state_dict(), os.path.join(a.out, "model.pt"))
    res["final"] = res["evals"][-1]
    json.dump(res, open(os.path.join(a.out, "results.json"), "w"), indent=1, default=float)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
