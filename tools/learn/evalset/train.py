#!/usr/bin/env python3
"""Train MLP, GRU and the memory transformer on one evalset build, and compare them.

  python train.py DATA OUT --model mlp|gru|tf [options]
  python train.py --summary OUT_PARENT

DATA is a directory written by build.py. Repeat 2 is the test set for every model,
and the loss is train_seq.py's: with T = max(C* - 0.3, 0.3),
(1 + e) relu(T - P)^2 + lambda relu(P - T), plus a hinge on changes of P larger
than 0.25 cores. The transformer is the only one that sees the other daemons
(--ctx). Width 64 matches the deployed GRU; pass --d to change it.

The checkpoint is not a C++ autoscaler weights file: build.py's features come
from cgroup rings, not from GruFeatures' per-thread nodes.
"""

import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "slo"))
from common import DEADBAND, OFF, REQ_MAX, metrics  # noqa: E402
from seq_models import GRUBase, MLPBase, MemoryTransformer, n_params  # noqa: E402


def load_cases(root):
    manifest = json.load(open(Path(root) / "manifest.json"))
    names = ["daemon.f16", "token.f16", "ctx.f16", "label.f16", "excess.f16", "valid.u8"]
    if (Path(root) / "outcome.f16").is_file():
        names.append("outcome.f16")
    arrays = {name: np.load(Path(root) / name, mmap_mode="r") for name in names}
    cases = []
    for case in manifest["cases"]:
        if case["n"] <= 0:
            continue
        start, length = case["start"], case["length"]
        cases.append({
            "name": case["name"], "group": case["group"], "test": case["test"],
            "n": case["n"], "T": length,
            "daemon": torch.from_numpy(np.array(arrays["daemon.f16"][start:start + length], np.float32)),
            "token": torch.from_numpy(np.array(arrays["token.f16"][start:start + length], np.float32)),
            "ctx": torch.from_numpy(np.array(arrays["ctx.f16"][start:start + length], np.float32)),
            "ctx_mask": torch.tensor(case["ctx_mask"], dtype=torch.bool),
            "C": torch.from_numpy(np.array(arrays["label.f16"][start:start + length], np.float32)),
            "E": torch.from_numpy(np.array(arrays["excess.f16"][start:start + length], np.float32)),
            "v": torch.from_numpy(np.array(arrays["valid.u8"][start:start + length], np.uint8).astype(bool)),
        })
        if "outcome.f16" in arrays:
            correct = np.array(arrays["outcome.f16"][start:start + length, 5]) > 0.5
            cases[-1]["ok"] = torch.from_numpy(correct)
    return manifest, cases


def threads_of(token):
    """One real token plus one aggregate token, in the 17 slots MemoryTransformer expects.

    token is [T, 17] or [B, T, 17]. Position 0 is the cgroup token. Position 16 is the
    same numbers marked as the aggregate. The slots in between stay masked off.
    """
    positions = 17
    if token.dim() == 2:
        out = token.new_zeros(token.shape[0], positions, token.shape[-1])
        out[:, 0] = token
        out[:, positions - 1] = token
        out[:, positions - 1, 8] = 1.0
        mask = torch.zeros(token.shape[0], positions, dtype=torch.bool, device=token.device)
    else:
        out = token.new_zeros(token.shape[0], token.shape[1], positions, token.shape[-1])
        out[:, :, 0] = token
        out[:, :, positions - 1] = token
        out[:, :, positions - 1, 8] = 1.0
        mask = torch.zeros(token.shape[0], token.shape[1], positions, dtype=torch.bool, device=token.device)
    mask[..., 0] = True
    mask[..., positions - 1] = True
    return out, mask


def loss_fn(pred, label, excess, valid, lam, lchg, hz, emax):
    # A collapsed run can be ~1000x the best p95. Past emax it counts the same as emax,
    # so one stalled scenario does not drown every other tick. emax 9 means 10x or worse.
    target = torch.clamp(label - OFF, min=OFF)
    weight = 1 + excess.clamp(max=emax)
    loss = weight * torch.relu(target - pred) ** 2 + lam * torch.relu(pred - target)
    out = loss[valid].mean() if valid.any() else pred.sum() * 0
    if lchg > 0:
        both = valid[..., 1:] & valid[..., :-1]
        if both.any():
            out = out + lchg * torch.relu((pred[..., 1:] - pred[..., :-1]).abs() - DEADBAND)[both].mean() * hz
    return out


def ceil_req(pred):
    return np.clip(np.ceil(pred - 1e-3), 1, REQ_MAX)


def grouped(cases, req_of, dt, mask_of=None):
    """mask_of(case) is an extra boolean over ticks, AND-ed with the valid mask."""
    reqs = {id(c): req_of(c) for c in cases}

    def pairs(subset):
        out = []
        for c in subset:
            valid = c["v"].numpy()
            if mask_of is not None:
                valid = valid & np.asarray(mask_of(c))
            out.append((reqs[id(c)], c["C"].numpy(), valid))
        return out

    out = {"all": metrics(pairs(cases), dt)}
    for group in sorted({c["group"] for c in cases}):
        out[group] = metrics(pairs([c for c in cases if c["group"] == group]), dt)
    return out


def hist_of(daemon, start, span):
    lo = start - span
    hist = daemon[max(lo, 0):start]
    if lo < 0:
        hist = torch.cat([torch.zeros(-lo, daemon.shape[1]), hist])
    return hist


def _pad_time(tensor, length, fill=0):
    pad = length - tensor.shape[0]
    if pad <= 0:
        return tensor
    return torch.cat([tensor, torch.full((pad, *tensor.shape[1:]), fill, dtype=tensor.dtype)])


@torch.no_grad()
def predict_all(model, cases, args, device):
    """Requests for every case. The transformer scores windows in batches; MLP and GRU score a whole case."""
    model.eval()
    if args.model != "tf":
        out = []
        for case in cases:
            if args.model == "mlp":
                pred = model(None, None, case["daemon"].to(device))[0].float().cpu().numpy()
            else:
                pred = model(None, None, case["daemon"][None].to(device))[0][0].float().cpu().numpy()
            out.append(ceil_req(pred))
        return out
    length, span = args.fine, args.mem * args.bucket
    step = max(1, length // 2)
    preds = [np.zeros(c["T"], np.float32) for c in cases]
    jobs = []
    for i, case in enumerate(cases):
        total = case["T"]
        done, end = 0, min(length, total)
        while True:
            jobs.append((i, max(0, end - length), end, done))
            done = end
            if end >= total:
                break
            end = min(end + step, total)
    batch_n = max(args.batch, 1)
    for b0 in range(0, len(jobs), batch_n):
        chunk = jobs[b0:b0 + batch_n]
        tokens, daemons, hists, ctxs, ctx_masks = [], [], [], [], []
        for i, start, end, _done in chunk:
            case = cases[i]
            tokens.append(_pad_time(case["token"][start:end], length))
            daemons.append(_pad_time(case["daemon"][start:end], length))
            hists.append(hist_of(case["daemon"], start, span))
            ctxs.append(_pad_time(case["ctx"][start:end], length))
            ctx_masks.append(case["ctx_mask"])
        threads, mask = threads_of(torch.stack(tokens).to(device))
        pred = model(threads, mask, torch.stack(daemons).to(device), torch.stack(hists).to(device),
                     torch.stack(ctxs).to(device), torch.stack(ctx_masks).to(device)).float().cpu().numpy()
        for row, (i, start, end, done) in enumerate(chunk):
            preds[i][done:end] = pred[row, done - start:end - start]
    return [ceil_req(p) for p in preds]


def evaluate(model, cases, args, device, dt):
    got = predict_all(model, cases, args, device)
    by_id = {id(case): req for case, req in zip(cases, got)}
    score = grouped(cases, lambda c: by_id[id(c)], dt)
    if cases and "ok" in cases[0]:
        # The same request, counted only where the critical-route window was a correct
        # decision: low errors, throughput held, and the latency tail still the body.
        score["where_correct"] = grouped(cases, lambda c: by_id[id(c)], dt, mask_of=lambda c: c["ok"].numpy())
    return score


def make_model(args):
    if args.model == "mlp":
        return MLPBase(args.d)
    if args.model == "gru":
        return GRUBase(args.d, args.layers)
    return MemoryTransformer(args.d, args.layers, args.heads, args.fine, args.mem, args.bucket, args.drop, ctx=args.ctx)


def batch(train, args, rng, device):
    weights = np.array([c["n"] for c in train], np.float64)
    weights /= weights.sum()
    length = args.window
    keys = ("token", "daemon", "C", "E", "v", "hist", "ctx", "ctx_mask")
    out = {k: [] for k in keys}
    for index in rng.choice(len(train), args.batch, p=weights):
        case = train[index]
        total = case["T"]
        start = int(rng.integers(0, max(1, total - length + 1)))
        end = min(start + length, total)
        pad = length - (end - start)

        def take(tensor, fill=0):
            piece = tensor[start:end]
            if pad:
                piece = torch.cat([piece, torch.full((pad, *piece.shape[1:]), fill, dtype=piece.dtype)])
            return piece

        out["token"].append(take(case["token"]))
        out["daemon"].append(take(case["daemon"]))
        out["C"].append(take(case["C"]))
        out["E"].append(take(case["E"]))
        out["v"].append(take(case["v"], False))
        if args.model == "tf":
            out["hist"].append(hist_of(case["daemon"], start, args.mem * args.bucket))
            out["ctx"].append(take(case["ctx"]))
            out["ctx_mask"].append(case["ctx_mask"])
    stacked = {k: torch.stack(v).to(device) for k, v in out.items() if v}
    return stacked


def run(args):
    os.makedirs(args.out, exist_ok=True)
    torch.manual_seed(args.seed)
    rng = np.random.default_rng(args.seed)
    if args.device == "cuda" and not torch.cuda.is_available():
        sys.exit("CUDA was requested and is not available. Pass --device cpu to train on the CPU.")
    device = torch.device(args.device)
    manifest, cases = load_cases(args.data)
    dt = float(manifest["dt_s"])
    hz = 1.0 / dt
    train = [c for c in cases if not c["test"]]
    test = [c for c in cases if c["test"]]
    print(f"{len(train)} train / {len(test)} test cases, "
          f"{sum(c['n'] for c in train)} / {sum(c['n'] for c in test)} ticks, dt {dt * 1e3:.1f} ms", flush=True)
    model = make_model(args).to(device)
    result = {"args": vars(args), "params": n_params(model), "dt_s": dt,
              "baseline_own_alloc": manifest.get("baseline_own_alloc", {}),
              "baseline_correct": manifest.get("baseline_correct", {}), "evals": []}
    print(f"{args.model}: {result['params']:,} parameters on {device}", flush=True)
    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=0.01)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=args.lr, total_steps=args.steps, pct_start=0.05)
    started = time.time()
    for step in range(1, args.steps + 1):
        model.train()
        window = batch(train, args, rng, device)
        if args.model == "tf":
            threads, mask = threads_of(window["token"])
            pred = model(threads, mask, window["daemon"], window["hist"], window["ctx"], window["ctx_mask"])
        else:
            pred = model(None, None, window["daemon"])[0]
        loss = loss_fn(pred.float(), window["C"], window["E"], window["v"], args.lam, args.lchg, hz, args.emax)
        opt.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()
        sched.step()
        if step % args.eval_every == 0 or step == args.steps:
            ev = evaluate(model, test, args, device, dt)
            ev["step"] = step
            ev["loss"] = float(loss.item())
            ev["s"] = time.time() - started
            result["evals"].append(ev)
            print(json.dumps(ev), flush=True)
            torch.save(model.state_dict(), os.path.join(args.out, "model.pt"))
    result["final"] = result["evals"][-1]
    json.dump(result, open(os.path.join(args.out, "results.json"), "w"), indent=1, default=float)
    print("wrote", args.out)


def summary(parent):
    """Print the held-out numbers for every model trained under parent, plus the own-allocation baseline."""
    rows = []
    baseline = None
    baseline_ok = None
    for path in sorted(Path(parent).glob("*/results.json")):
        doc = json.load(open(path))
        final = doc.get("final") or {}
        baseline = baseline or doc.get("baseline_own_alloc", {}).get("test")
        baseline_ok = baseline_ok or doc.get("baseline_correct", {}).get("test")
        rows.append((path.parent.name, doc.get("params"), final))
    if baseline:
        _print_row("held cores", None, baseline)
    if baseline_ok:
        _print_row("held correct", None, baseline_ok)
    for name, params, final in rows:
        _print_row(name, params, final)
        if final.get("where_correct"):
            _print_row(f"{name} correct", params, final["where_correct"])


def _print_row(name, params, block):
    overall = block.get("all", {})
    parts = [f"{name:12} params={params if params is not None else '-':>10}  "
             f"under={overall.get('under_%', float('nan')):5.2f}%  over={overall.get('over_%', float('nan')):5.1f}%  "
             f"core-s {overall.get('core_s_pred', float('nan')):.0f} / label {overall.get('core_s_label', float('nan')):.0f}  "
             f"changes/s {overall.get('req_changes_per_s', float('nan')):.2f}"]
    for group in ("hotel", "social", "media"):
        g = block.get(group)
        if g:
            parts.append(f"    {group:8} under={g['under_%']:5.2f}%  over={g['over_%']:5.1f}%  "
                         f"changes/s {g['req_changes_per_s']:.2f}")
    print("\n".join(parts))


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "--summary":
        summary(sys.argv[2])
        return
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("data")
    parser.add_argument("out")
    parser.add_argument("--model", choices=["mlp", "gru", "tf"], required=True)
    parser.add_argument("--d", type=int, default=64)
    parser.add_argument("--layers", type=int, default=2)
    parser.add_argument("--heads", type=int, default=4)
    parser.add_argument("--drop", type=float, default=0.1)
    parser.add_argument("--fine", type=int, default=256)
    parser.add_argument("--mem", type=int, default=20)
    parser.add_argument("--bucket", type=int, default=20)
    parser.add_argument("--window", type=int, default=None)
    parser.add_argument("--steps", type=int, default=2000)
    parser.add_argument("--batch", type=int, default=32)
    parser.add_argument("--lr", type=float, default=3e-4)
    parser.add_argument("--lam", type=float, default=0.03)
    parser.add_argument("--lchg", type=float, default=0.1)
    parser.add_argument("--emax", type=float, default=9.0,
                        help="cap on the p95-ratio excess weight; 9 counts a 10x slower run the same as anything worse")
    parser.add_argument("--eval-every", type=int, default=500)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--ctx", action="store_true")
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    args = parser.parse_args()
    args.window = args.window or (args.fine if args.model == "tf" else 256)
    if args.model == "tf" and not args.ctx:
        print("note: tf without --ctx does not see the other daemons", flush=True)
    run(args)


if __name__ == "__main__":
    main()
