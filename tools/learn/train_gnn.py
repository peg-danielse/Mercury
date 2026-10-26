"""First GNN for Mercury's gnn policy: per-thread demand regression by imitation of ground truth.

python train_gnn.py <run_dir> [--every N] [--epochs E] [--out results/<name>]
Graph per tick: one node per gnnload worker plus one aggregate node for the daemon's other
(idle) threads. Edges: same cgroup (all pairs) and NUMA proximity of the threads' current nodes,
weight 10/distance (dtn-01 distances: 10 local, 16 same socket, 32 cross socket). Model:
LocalGNN_DB-style delayed graph filters (lab 5): layer output sum_k S^k X(t-k) W_k, so hop k
sees data k ticks old, as it would when it travels one hop per tick. Targets: per-thread demand;
required cores are derived (FFD bin-packing and ceil(sum/0.75)). Split by case.
"""
import json, math, os, random, sys, time
import numpy as np
import torch
import torch.nn as nn
from mdata import cases, ffd_cores, load_case

DIST = None  # NUMA distance matrix, filled from dataset meta or default dtn-01


def dtn01_distance():
    d = np.full((8, 8), 32.0)
    for a in range(8):
        for b in range(8):
            if a == b:
                d[a, b] = 10
            elif a // 4 == b // 4:
                d[a, b] = 16
    return d


F_NAMES = ["task_clock", "run", "wait", "ctx_switch_k", "migrations_100", "cg_occupancy", "cg_pressure",
           "cpuset_16", "is_aggregate"] + [f"node{i}" for i in range(8)]


def case_tensors(d):
    """X [T, N, F], S [T, N, N], y [T, N] (aggregate node target 0), valid [T]."""
    T, W = d["dem"].shape
    N = W + 1
    X = np.zeros((T, N, len(F_NAMES)), np.float32)
    X[:, :W, 0] = d["tc"]; X[:, :W, 1] = d["run"]; X[:, :W, 2] = d["wait"]
    X[:, :W, 3] = d["cs"] / 1000; X[:, :W, 4] = d["mig"] / 100
    X[:, W, 1] = d["other_run"]; X[:, W, 2] = d["other_wait"]; X[:, W, 0] = d["other_run"]
    X[:, :, 5] = d["cg_occ"][:, None]; X[:, :, 6] = d["cg_psi"][:, None]; X[:, :, 7] = d["cpus"][:, None] / 16
    X[:, W, 8] = 1
    nodes = np.concatenate([d["node"], d["node"][:, :1]], 1)  # aggregate placed on worker 0's node
    for i in range(8):
        X[:, :, 9 + i] = nodes == i
    dist = DIST[np.clip(nodes, 0, 7)[:, :, None], np.clip(nodes, 0, 7)[:, None, :]]
    A = 0.5 * (1.0 + 10.0 / dist)  # same cgroup (1) + NUMA proximity (10/dist), averaged
    A[:, np.arange(N), np.arange(N)] = 0
    S = A / np.maximum(A.sum(-1, keepdims=True), 1e-9)  # row-normalised: mean over neighbours
    y = np.concatenate([d["dem"], np.zeros((T, 1))], 1).astype(np.float32)
    return torch.tensor(X), torch.tensor(S.astype(np.float32)), torch.tensor(y), torch.tensor(d["valid"])


class DelayedGraphFilter(nn.Module):
    """y(t) = sum_{k<K} S(t)...S(t-k+1) x(t-k) W_k (+b): the lab 5 GraphFilter_DB without edge features."""

    def __init__(self, fin, fout, K):
        super().__init__()
        self.K = K
        self.W = nn.Parameter(torch.randn(K, fin, fout) / math.sqrt(fin * K))
        self.b = nn.Parameter(torch.zeros(fout))

    def forward(self, x, S):  # x [T, N, F], S [T, N, N]
        out = x @ self.W[0]
        z = x
        for k in range(1, self.K):
            z = torch.cat([torch.zeros_like(z[:1]), z[:-1]], 0)  # delay one tick
            z = S @ z                                             # one hop, at the receiving tick
            out = out + z @ self.W[k]
        return out + self.b


class LocalGNNDB(nn.Module):
    def __init__(self, F, H=16, K=3):
        super().__init__()
        self.g1 = DelayedGraphFilter(F, H, K)
        self.g2 = DelayedGraphFilter(H, H, K)
        self.readout = nn.Linear(H, 1)

    def forward(self, x, S):
        h = torch.relu(self.g1(x, S))
        h = torch.relu(self.g2(h, S))
        return torch.clamp(nn.functional.softplus(self.readout(h).squeeze(-1)), max=1.0)


def split(names, seed=0, test_frac=0.25):
    rng = random.Random(seed)
    by = {}
    for c in names:
        by.setdefault(os.path.basename(c).split("-")[0], []).append(c)
    test = []
    for p, lst in sorted(by.items()):
        lst = sorted(lst); rng.shuffle(lst)
        test += lst[:max(1, round(len(lst) * test_frac))]
    return [c for c in names if c not in test], sorted(test)


def main():
    global DIST
    run = sys.argv[1]  # one run dir, or several joined by "," (DAgger: aggregate saturate + gnn-controlled runs)
    arg = lambda k, dflt: type(dflt)(sys.argv[sys.argv.index(k) + 1]) if k in sys.argv else dflt
    every, epochs = arg("--every", 1), arg("--epochs", 30)
    drop_cpuset = "--drop-cpuset" in sys.argv  # zero feature 7 (cpuset size / 16) in training and export
    out = arg("--out", os.path.join("results", os.path.basename(run.split(",")[0].rstrip("/")) + f"-every{every}"))
    os.makedirs(out, exist_ok=True)
    torch.manual_seed(0); np.random.seed(0)
    torch.set_num_threads(8)
    DIST = dtn01_distance()
    names = [os.path.join(r, c) for r in run.split(",") for c in cases(r)]
    train, test = split(names)
    if "--test-cases" in sys.argv:  # fixed held-out case names (e.g. the same daemons across DAgger rounds)
        keep = set(arg("--test-cases", "").split(","))
        test = sorted(c for c in names if os.path.basename(c) in keep); train = [c for c in names if c not in test]
    data = {}
    t0 = time.time()
    for c in names:
        d = load_case(c, every)
        X, S, y, v = case_tensors(d)
        if drop_cpuset:
            X[:, :, 7] = 0
        data[c] = (d, (X, S, y, v))
    tick_ms = float(np.median(np.concatenate([data[c][0]["iv"] for c in names])) / 1e6)
    print(f"loaded {len(names)} cases ({len(train)} train / {len(test)} test) in {time.time()-t0:.0f}s, tick {tick_ms:.1f} ms")
    model = LocalGNNDB(len(F_NAMES))
    nparams = sum(p.numel() for p in model.parameters())
    opt = torch.optim.Adam(model.parameters(), lr=3e-3)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, epochs)
    hist = []
    for ep in range(epochs):
        model.train(); random.shuffle(train); tot = 0
        for c in train:
            X, S, y, v = data[c][1]
            pred = model(X, S)
            m = v[:, None] & (y >= 0)  # label -1 = contended thread-tick of a DAgger relabel: not scored
            loss = ((pred - y)[m] ** 2).mean()
            opt.zero_grad(); loss.backward(); opt.step(); tot += loss.item()
        sched.step()
        hist.append(tot / len(train))
        if ep % 5 == 4 or ep == epochs - 1:
            print(f"epoch {ep+1} train mse {hist[-1]:.5f}")
    model.eval()
    res = {"tick_ms": tick_ms, "params": nparams, "train_cases": train, "test_cases": test, "features": F_NAMES, "per_case": {}}
    agg = {k: [] for k in ["thr_gnn_contended", "thr_runwait_contended", "thr_tc_contended", "thr_gnn", "thr_runwait", "thr_tc", "cores_gnn_ffd", "cores_gnn_u075", "cores_saturate", "cores_runwait_u075", "cores_runwait_ffd"]}
    plot_case = None
    with torch.no_grad():
        for c in test:
            d, (X, S, y, v) = data[c]
            p = model(X, S).numpy()[:, :-1]
            vv = v.numpy()[:, None] & (d["dem"] >= 0)  # thread-tick mask (self-labelled DSB: -1 = unlabelled)
            dem = d["dem"]; rw = np.clip(d["run"] + d["wait"], 0, 1)
            req = d["req_ffd"]
            cg = {"cores_gnn_ffd": np.array([ffd_cores(r) for r in p]),
                  "cores_gnn_u075": np.ceil(p.sum(1) / 0.75 - 1e-6),
                  "cores_saturate": d["cpus"],
                  "cores_runwait_u075": np.ceil((d["run"] + d["wait"]).sum(1) / 0.75 - 1e-6),
                  "cores_runwait_ffd": np.array([ffd_cores(r) for r in rw])}
            row = {"thr_gnn": np.abs(p - dem)[vv].mean(), "thr_runwait": np.abs(rw - dem)[vv].mean(),
                   "thr_tc": np.abs(d["tc"] - dem)[vv].mean()}
            for k, est in cg.items():
                row[k] = np.abs(est - req)[v.numpy()].mean()
            agg["thr_gnn"].append(np.abs(p - dem)[vv].ravel()); agg["thr_runwait"].append(np.abs(rw - dem)[vv].ravel())
            agg["thr_tc"].append(np.abs(d["tc"] - dem)[vv].ravel())
            cont = (d["wait"] >= 0.02) & vv  # contended thread-ticks
            agg["thr_gnn_contended"].append(np.abs(p - dem)[cont]); agg["thr_runwait_contended"].append(np.abs(rw - dem)[cont])
            agg["thr_tc_contended"].append(np.abs(d["tc"] - dem)[cont])
            for k, est in cg.items():
                agg[k].append(np.abs(est - req)[v.numpy()])
            res["per_case"][c] = {k: float(x) for k, x in row.items()}
            if plot_case is None or os.path.basename(c).startswith("bursty-t8"):
                plot_case = (c, d, p, cg)
    res["test_mae"] = {k: float(np.concatenate(x).mean()) for k, x in agg.items()}
    print(json.dumps(res["test_mae"], indent=1))
    # inference latency: batch 1, one tick graph, K-tick history (what the controller evaluates per tick)
    lat = {}
    for N in (3, 9, 17, 65):
        X1 = torch.randn(model.g1.K, N, len(F_NAMES)); S1 = torch.softmax(torch.randn(model.g1.K, N, N), -1)
        torch.set_num_threads(1)
        with torch.no_grad():
            for _ in range(50):
                model(X1, S1)
            ts = []
            for _ in range(2000):
                a = time.perf_counter_ns(); model(X1, S1); ts.append(time.perf_counter_ns() - a)
        ts = np.array(ts) / 1e3
        # numpy forward of the same weights (proxy for the handwritten C++ forward)
        W = {k: v.detach().numpy() for k, v in model.state_dict().items()}
        Xn, Sn = X1.numpy(), S1.numpy()
        def npf():
            def gf(x, pre):
                o = x @ W[pre + ".W"][0]; z = x
                for k in range(1, W[pre + ".W"].shape[0]):
                    z = np.concatenate([np.zeros_like(z[:1]), z[:-1]], 0); z = Sn @ z; o = o + z @ W[pre + ".W"][k]
                return o + W[pre + ".b"]
            h = np.maximum(gf(Xn, "g1"), 0); h = np.maximum(gf(h, "g2"), 0)
            return h @ W["readout.weight"].T + W["readout.bias"]
        tn = []
        for _ in range(2000):
            a = time.perf_counter_ns(); npf(); tn.append(time.perf_counter_ns() - a)
        tn = np.array(tn) / 1e3
        lat[N] = {"torch_us_median": float(np.median(ts)), "torch_us_p99": float(np.percentile(ts, 99)),
                  "numpy_us_median": float(np.median(tn)), "numpy_us_p99": float(np.percentile(tn, 99))}
    res["latency"] = lat
    print(json.dumps(lat, indent=1))
    if drop_cpuset:  # the input is always zero in training; zero its weights so the C++ forward ignores it too
        with torch.no_grad():
            model.g1.W[:, 7, :] = 0
    res["drop_cpuset"] = drop_cpuset
    torch.save(model.state_dict(), os.path.join(out, "model.pt"))
    json.dump({k: v.tolist() for k, v in model.state_dict().items()}, open(os.path.join(out, "weights.json"), "w"))
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1, default=float)
    import matplotlib; matplotlib.use("Agg"); import matplotlib.pyplot as plt
    c, d, p, cg = plot_case
    t = d["t"]
    fig, ax = plt.subplots(3, 1, figsize=(11, 9), sharex=True)
    for i in range(min(4, p.shape[1])):
        ax[0].plot(t, d["dem"][:, i], color=f"C{i}", lw=1.5); ax[0].plot(t, p[:, i], color=f"C{i}", lw=0.6, ls="--")
    ax[0].set_ylabel("demand (cores)"); ax[0].set_title(f"{c} (test): per-thread demand, solid = truth, dashed = GNN (first 4 workers)")
    ax[1].step(t, d["req_ffd"], where="post", color="k", lw=2, label="required (FFD truth)")
    ax[1].step(t, cg["cores_gnn_ffd"], where="post", label="GNN -> FFD")
    ax[1].step(t, cg["cores_saturate"], where="post", label="saturate cpuset")
    ax[1].step(t, cg["cores_runwait_u075"], where="post", lw=0.7, label="ceil((run+wait)/0.75)")
    ax[1].set_ylabel("cores"); ax[1].legend(fontsize=8)
    ax[2].semilogy(np.arange(1, len(hist) + 1) * (t[-1] / len(hist)), hist); ax[2].set_ylabel("train MSE"); ax[2].set_xlabel("time (s) / epochs (scaled)")
    fig.tight_layout(); fig.savefig(os.path.join(out, "test_case.png"), dpi=90); plt.close(fig)
    m = res["test_mae"]
    fig, ax = plt.subplots(1, 2, figsize=(12, 4))
    ax[0].bar(["GNN", "run+wait", "task-clock"], [m["thr_gnn"], m["thr_runwait"], m["thr_tc"]]); ax[0].set_title("per-thread demand MAE (cores), test cases")
    ks = ["cores_gnn_ffd", "cores_gnn_u075", "cores_saturate", "cores_runwait_u075", "cores_runwait_ffd"]
    ax[1].bar(["GNN FFD", "GNN /0.75", "saturate", "(r+w)/0.75", "(r+w) FFD"], [m[k] for k in ks]); ax[1].set_title("required-cores MAE vs FFD truth")
    fig.tight_layout(); fig.savefig(os.path.join(out, "mae.png"), dpi=90); plt.close(fig)
    print("wrote", out)


if __name__ == "__main__":
    main()
