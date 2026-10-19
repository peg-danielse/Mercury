"""Train gnn-slo-v1 (DESIGN.md): fine-tune v5 on DSB traces with allocation labels from the lowest-p95 sweep.
python slo/train_slo.py <out_dir> --init <weights.json> --lambdas 0.03,0.1,0.3,1 --epochs E \
    case=<trace.jsonl>@<windows.json>@<daemon>[@test] ...
Loss per daemon-tick: (1+e) relu(T-P)^2 + lambda relu(P-T) + mu mean_i (p_i-y_i)^2
  + lambda_chg * 100 * relu(|P(t)-P(t-1)| - 0.25), T = C* - 0.3, P = sum of thread preds.
Picks lambda (largest whose held-out under-allocation rate <= 1.5x that of the smallest lambda), then lambda_chg (largest
with under-allocation <= 1.2x (+0.5 pt) and core-s <= 1.05x of lambda_chg = 0), writes weights.json,
results.json and lambda.png."""
import json, os, sys, time
import numpy as np, torch
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import train_gnn as tg
from mdata import ffd_cores, load_self

MU, OFF = 0.1, 0.3


def build(spec):
    trace, wins, daemon, *flag = spec.split("@")
    d = load_self(os.path.dirname(trace), "runshare") if os.path.basename(trace) == "trace.jsonl" else None
    if d is None:  # trace-<daemon>.jsonl inside a run dir: load through a temporary case dir
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
    X, S, _, v = tg.case_tensors(d); X[:, :, 7] = 0
    run = torch.tensor(np.clip(d["run"], 0, 1), dtype=torch.float32)
    return {"name": f"{os.path.basename(os.path.dirname(os.path.abspath(trace)))}/{daemon}", "test": bool(flag), "X": X, "S": S,
            "C": torch.tensor(np.nan_to_num(C), dtype=torch.float32), "E": torch.tensor(E, dtype=torch.float32),
            "v": torch.tensor(valid), "run": run, "n": int(valid.sum())}


DB = 0.25  # change deadband (cores): tick-to-tick moves of P below this are free


def loss_fn(model, c, lam, lchg=0.0):
    p = model(c["X"], c["S"])[:, :-1]  # drop aggregate node (no other threads in self-labelled cases)
    v = c["v"]
    if v.sum() == 0:
        return None
    T = torch.clamp(c["C"] - OFF, min=OFF)
    P = p.sum(1)
    w = torch.clamp(c["run"], min=0) + 0.01
    y = torch.clamp(T[:, None] * w / w.sum(1, keepdim=True), max=1.0)
    l = (1 + c["E"]) * torch.relu(T - P) ** 2 + lam * torch.relu(P - T) + MU * ((p - y) ** 2).mean(1)
    l = l[v].mean()
    if lchg > 0:  # temporal consistency: hinge on |P(t) - P(t-1)| above the deadband, x100 ticks/s = per-second rate
        vv = v[1:] & v[:-1]
        if vv.any():
            l = l + lchg * torch.relu((P[1:] - P[:-1]).abs() - DB)[vv].mean() * 100
    return l


def evaluate(model, cs):
    under = over = n = 0; core_s_pred = core_s_lab = core_s_run = 0.0; changes = 0; secs = 0.0
    with torch.no_grad():
        for c in cs:
            if c["n"] == 0:
                continue
            p = model(c["X"], c["S"])[:, :-1].numpy(); v = c["v"].numpy(); C = c["C"].numpy()
            req = np.array([ffd_cores(r) for r in p[v]]); lab = np.round(C[v])
            reqall = np.array([ffd_cores(r) for r in p]); ch = (reqall[1:] != reqall[:-1]) & v[1:] & v[:-1]
            changes += int(ch.sum()); secs += 0.01 * float((v[1:] & v[:-1]).sum())
            under += int((req < lab).sum()); over += int((req > lab).sum()); n += int(v.sum())
            core_s_pred += float(req.sum()) * 0.01; core_s_lab += float(lab.sum()) * 0.01; core_s_run += float(c["run"].numpy()[v].sum()) * 0.01
    return {"ticks": n, "under_%": 100 * under / max(n, 1), "over_%": 100 * over / max(n, 1), "core_s_pred": core_s_pred,
            "core_s_label": core_s_lab, "core_s_run": core_s_run,
            "req_changes_per_s": changes / max(secs, 1e-9)}


def main():
    out = sys.argv[1]; os.makedirs(out, exist_ok=True)
    arg = lambda k, d: type(d)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else d
    init, epochs = arg("--init", ""), arg("--epochs", 20)
    lams = [float(x) for x in arg("--lambdas", "0.03,0.1,0.3,1").split(",")]
    lchgs = [float(x) for x in arg("--lambda-chg", "0,0.01,0.03,0.1").split(",")]
    tg.DIST = tg.dtn01_distance(); torch.manual_seed(0); torch.set_num_threads(8)
    t0 = time.time()
    cs = [build(a.split("=", 1)[1]) for a in sys.argv[2:] if a.startswith("case=")]
    tr = [c for c in cs if not c["test"] and c["n"] > 0]; te = [c for c in cs if c["test"] and c["n"] > 0]
    print(f"{len(tr)} train / {len(te)} test cases, {sum(c['n'] for c in tr)} / {sum(c['n'] for c in te)} ticks, {time.time()-t0:.0f}s")
    W0 = json.load(open(init))
    res = {"init": init, "mu": MU, "offset": OFF, "epochs": epochs, "train": [c["name"] for c in tr], "test": [c["name"] for c in te], "lambda": {}}
    m0 = tg.LocalGNNDB(len(tg.F_NAMES)); m0.load_state_dict({k: torch.tensor(v) for k, v in W0.items()}); m0.eval()
    res["init_eval"] = evaluate(m0, te)
    models = {}

    def fit(lam, lchg):
        m = tg.LocalGNNDB(len(tg.F_NAMES)); m.load_state_dict({k: torch.tensor(v) for k, v in W0.items()})
        opt = torch.optim.Adam(m.parameters(), lr=1e-3); tot = 0
        for ep in range(epochs):
            m.train(); tot = 0; order = np.random.RandomState(ep).permutation(len(tr))
            for i in order:
                l = loss_fn(m, tr[i], lam, lchg)
                if l is None:
                    continue
                opt.zero_grad(); l.backward(); opt.step(); tot += l.item()
                with torch.no_grad():
                    m.g1.W[:, 7, :] = 0
        m.eval(); ev = evaluate(m, te); ev["train_loss"] = tot / max(len(tr), 1)
        print(lam, lchg, json.dumps(ev), flush=True)
        return m, ev

    # step 1: lambda_alloc at lambda_chg = 0; largest lambda with held-out under-allocation <= 1.5x the smallest lambda's
    for lam in lams:
        models[(lam, 0.0)], res["lambda"][lam] = fit(lam, 0.0)
    base = res["lambda"][min(lams)]["under_%"]
    pick = max(l for l in lams if res["lambda"][l]["under_%"] <= 1.5 * base + 1e-9); res["picked_lambda"] = pick
    # step 2: lambda_chg at that lambda_alloc; largest lambda_chg with under-allocation <= 1.2x and core-s <= 1.05x of lchg = 0
    res["lambda_chg"] = {0.0: res["lambda"][pick]}
    for lc in [x for x in lchgs if x > 0]:
        models[(pick, lc)], res["lambda_chg"][lc] = fit(pick, lc)
    b = res["lambda_chg"][0.0]
    okc = [lc for lc, ev in res["lambda_chg"].items() if ev["under_%"] <= 1.2 * b["under_%"] + 0.5 and ev["core_s_pred"] <= 1.05 * b["core_s_pred"]]
    pickc = max(okc); res["picked_lambda_chg"] = pickc; res["deadband"] = DB
    m = models[(pick, pickc)] if pickc > 0 else models[(pick, 0.0)]
    json.dump({k: v.tolist() for k, v in m.state_dict().items()}, open(os.path.join(out, "weights.json"), "w"))
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1, default=float)
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 3, figsize=(16, 4))
    ax[0].plot(lams, [res["lambda"][l]["under_%"] for l in lams], "o-", label="under %"); ax[0].plot(lams, [res["lambda"][l]["over_%"] for l in lams], "o-", label="over %")
    ax[0].axhline(res["init_eval"]["under_%"], ls="--", c="C0", lw=0.8, label="v5 under %"); ax[0].axhline(res["init_eval"]["over_%"], ls="--", c="C1", lw=0.8, label="v5 over %")
    ax[0].set_xscale("log"); ax[0].set_xlabel("lambda"); ax[0].legend(); ax[0].set_title(f"held-out ticks vs label C* (picked {pick})")
    ax[1].bar(["label C*", "run (used)", "v5"] + [f"l={l}" for l in lams], [res["init_eval"]["core_s_label"], res["init_eval"]["core_s_run"], res["init_eval"]["core_s_pred"]] + [res["lambda"][l]["core_s_pred"] for l in lams])
    ax[1].set_title("held-out core-seconds"); ax[1].tick_params(axis="x", rotation=30)
    lc = sorted(res["lambda_chg"]); ax[2].plot(lc, [res["lambda_chg"][x]["req_changes_per_s"] for x in lc], "o-", label="core-count changes/s")
    ax[2].plot(lc, [res["lambda_chg"][x]["under_%"] for x in lc], "o-", label="under %"); ax[2].axhline(res["init_eval"]["req_changes_per_s"], ls="--", lw=0.8, label="v5 changes/s")
    ax[2].set_xlabel("lambda_chg"); ax[2].legend(); ax[2].set_title(f"lambda_alloc={pick}: picked lambda_chg={pickc}")
    fig.tight_layout(); fig.savefig(os.path.join(out, "lambda.png"), dpi=90)
    print("picked", pick, pickc, "->", out)


if __name__ == "__main__":
    main()
