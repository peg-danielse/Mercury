#!/usr/bin/env python3
"""Export a GRU autoscaler (gnn-example/mercury/slo/train_seq.py --model gru) for Mercury's gru autoscaler.

  export_gru.py MODEL.pt OUT.json [--check CHECK.json] [--seq-models DIR] [--d 128] [--layers 2]

OUT.json holds the state_dict as nested lists (the format of models/gnn_*.json); Mercury loads it as the gru
autoscaler (MERCURY_GNN_MODEL, policy gnn). --check writes a verification file for src/test/gru.cpp: a synthetic
per-thread sequence (train_gnn.case_tensors layout: X [T, N, 17], the last node the aggregate), the cpuset size per
tick, seq_models.featurize's daemon features for it, and GRUBase's demand P per tick over that sequence. The C++
feature step and forward pass must reproduce both.

Needs torch and gnn-example (--seq-models: the directory holding seq_models.py, default ../gnn-example/mercury/slo
next to this repository).
"""

from __future__ import annotations

import argparse
import json
import os
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("out")
    ap.add_argument("--check")
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument("--seq-models", default=os.path.join(here, "..", "..", "gnn-example", "mercury", "slo"))
    ap.add_argument("--d", type=int, default=128)
    ap.add_argument("--layers", type=int, default=2)
    ap.add_argument("--ticks", type=int, default=400)
    a = ap.parse_args()

    import torch
    sys.path.insert(0, a.seq_models)
    from seq_models import DAEMON_F, GRUBase, featurize

    model = GRUBase(a.d, a.layers)
    model.load_state_dict(torch.load(a.model, map_location="cpu"))
    model.eval()
    state = {k: v.detach().cpu().tolist() for k, v in model.state_dict().items()}
    with open(a.out, "w") as f:
        json.dump(state, f)
    print(f"wrote {a.out}: d={a.d} layers={a.layers} features={DAEMON_F}")

    if a.check:
        g = torch.Generator().manual_seed(7)
        T, W = a.ticks, 12
        X = torch.zeros(T, W + 1, 17)
        busy = (torch.rand(T, W, generator=g) < 0.6).float()
        X[:, :W, 1] = busy * torch.rand(T, W, generator=g)                  # run
        X[:, :W, 2] = busy * 0.3 * torch.rand(T, W, generator=g)            # wait
        X[:, :W, 0] = X[:, :W, 1] * (0.9 + 0.1 * torch.rand(T, W, generator=g))  # task-clock
        X[:, :W, 3] = busy * 2 * torch.rand(T, W, generator=g)              # context switches / 1000
        X[:, :W, 4] = busy * 0.5 * torch.rand(T, W, generator=g)            # migrations / 100
        X[:, W, 1] = 0.2 * torch.rand(T, generator=g)                       # aggregate run
        X[:, W, 2] = 0.05 * torch.rand(T, generator=g)                      # aggregate wait
        X[:, W, 0] = X[:, W, 1]
        X[:, :, 5] = torch.rand(T, generator=g)[:, None]                    # cgroup occupancy
        X[:, :, 6] = 0.5 * torch.rand(T, generator=g)[:, None]              # cgroup pressure
        X[:, W, 8] = 1
        numa = torch.randint(0, 8, (W,), generator=g)
        for i in range(W):
            X[:, i, 9 + int(numa[i])] = 1
        X[:, W, 9 + int(numa[0])] = 1
        alloc = torch.tensor([2 + (t // 37) % 5 for t in range(T)], dtype=torch.float32)  # changes every 37 ticks
        _, _, daemon = featurize(X, alloc.numpy())
        with torch.no_grad():
            P = model(None, None, daemon[None])[0][0]
        check = {"threads": X[:, :W, :5].tolist(), "numa": numa.tolist(), "other_run": X[:, W, 1].tolist(),
                 "other_wait": X[:, W, 2].tolist(), "occupancy": X[:, 0, 5].tolist(), "pressure": X[:, 0, 6].tolist(),
                 "alloc": alloc.int().tolist(), "features": daemon.tolist(), "demand": P.tolist()}
        with open(a.check, "w") as f:
            json.dump(check, f)
        print(f"wrote {a.check}: {T} ticks, demand {float(P.min()):.3f}..{float(P.max()):.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
