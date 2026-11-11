#!/usr/bin/env python3
"""Build a Luna imitation set from the 2026-11-05 mix seeks.

Each case is one daemon of one finished llm40-luna seek
(data/dsb/compare/{hotel,social,media}-luna-seek-20261105/rN). The label is
Luna's cpuset size, and the allocation slots of the feature vector are the
previous tick's size, the same as build_luna.py. A tick is unlabelled when the
load-generator window covering it has an error rate of 10% or more, summed
over every route: that is the seek's own stop line, and Luna's counts after
the system has already fallen over are not a target. Repeats r1 and r2 train.
r3 of each application is the test set.

  python build_luna_seek.py [OUT_DIR]

OUT_DIR defaults to <repo>/data/learn/luna-seek-20261105. Numpy only; no torch.
"""

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as rings  # noqa: E402
import build_luna as luna  # noqa: E402
from common import APPS, CRITICAL  # noqa: E402

# The seek stops raising the level at this error rate. Windows at or above it
# stay in the series so time is continuous, and their valid bit is 0.
ERR_KEEP = 0.10

REPO = Path(__file__).resolve().parents[3]
COMPARE = REPO / "data" / "dsb" / "compare"
TAG = "luna-seek-20261105"


def discover():
    """One finished saturate run per application per repeat. r3 is the test set."""
    found = []
    for app in APPS:
        base = COMPARE / f"{app}-{TAG}"
        if not base.is_dir():
            continue
        for rep_dir in sorted(p for p in base.iterdir() if p.is_dir() and p.name.startswith("r")):
            runs = sorted(rep_dir.glob("*/1-saturate"))
            if len(runs) != 1 or not (runs[0] / "rings").is_dir():
                print(f"skip {rep_dir}: expected one saturate run with rings, found {len(runs)}")
                continue
            found.append({
                "app": app,
                "arm": "luna",
                "policy": "llm40-luna",
                "rep": rep_dir.name,
                "dir": runs[0],
                "test": rep_dir.name == "r3",
            })
    return found


def window_error(path):
    """Error rate of every load-generator window, all routes summed. Time is the window start."""
    grouped = {}
    for line in open(path):
        row = json.loads(line)
        start = int(row["t0_ns"])
        slot = grouped.get(start)
        if slot is None:
            slot = {"sent": 0, "errors": 0}
            grouped[start] = slot
        slot["sent"] += int(row.get("sent", 0))
        slot["errors"] += int(row.get("errors", 0))
    if not grouped:
        return np.zeros(0, np.int64), np.zeros(0, np.float32)
    times = sorted(grouped)
    err = np.array([
        (grouped[t]["errors"] / grouped[t]["sent"]) if grouped[t]["sent"] else 1.0
        for t in times
    ], np.float32)
    return np.array(times, np.int64), err


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "data" / "learn" / TAG
    out.mkdir(parents=True, exist_ok=True)
    runs = discover()
    print(f"{len(runs)} runs", flush=True)
    held_out = sorted({(r["app"], r["rep"]) for r in runs if r["test"]})
    print("test:", " ".join(f"{a}/{rep}" for a, rep in held_out), flush=True)

    daemon_chunks, token_chunks, ctx_chunks = [], [], []
    label_chunks, excess_chunks, valid_chunks, outcome_chunks = [], [], [], []
    correct_chunks = []
    cases = []
    offset = 0
    dt_samples = []
    moved = 0
    labelled = 0
    kept = 0
    for run in runs:
        directory = run["dir"]
        prepared = {}
        for path in sorted((directory / "rings").glob("*.mring")):
            name = path.name[:-len(".mring")]
            if name.startswith("cpu-") or "loadgen" in name or "jaeger" in name:
                continue
            series = rings.ring_series(path)
            if series is not None:
                prepared[name] = series
        if not prepared:
            print(f"skip {run['app']}/{run['rep']}: no daemon rings")
            continue
        outcome_t, outcome = luna.critical_outcome(directory / "loadgen.hist.jsonl", set(CRITICAL[run["app"]]))
        err_t, err = window_error(directory / "loadgen.hist.jsonl")
        stamps_s, wait_s, busy_s, threads_s = rings.sample_table(directory / "sample.jsonl")
        aligned = {}
        medians = []
        for name, (stamps, clock, pressure, cores) in prepared.items():
            medians.append((float(np.median(clock)), name))
            aligned[name] = rings.daemon_features(
                stamps, clock, pressure, luna.lag(cores),
                rings._column(stamps_s, wait_s.get(name), stamps),
                rings._column(stamps_s, busy_s.get(name), stamps),
                rings._column(stamps_s, threads_s.get(name), stamps),
            )
        order = [name for _, name in sorted(medians, key=lambda item: (-item[0], item[1]))]
        run_kept = 0
        run_n = 0
        for name, (stamps, _clock, _pressure, cores) in prepared.items():
            daemon, token, _own = aligned[name]
            peers = [peer for peer in order if peer != name][:9]
            context = np.zeros((len(stamps), 9, 5), np.float32)
            mask = np.zeros(9, np.int64)
            for slot, peer in enumerate(peers):
                context[:, slot] = rings.at_times(prepared[peer][0], aligned[peer][2], stamps)
                mask[slot] = 1
            label = np.asarray(cores, np.float32)
            covered = rings.at_times(err_t, err[:, None], stamps)[:, 0]
            valid = (covered < ERR_KEEP).astype(np.uint8)
            here = rings.at_times(outcome_t, outcome, stamps)
            correct_chunks.append((here[:, 5] > 0.5).astype(np.uint8))
            if len(stamps) > 1:
                dt_samples.append(float(np.median(np.diff(stamps))) / 1e9)
                moved += int(np.count_nonzero(label[1:] != label[:-1]))
            labelled += len(stamps)
            kept += int(valid.sum())
            run_kept += int(valid.sum())
            run_n += len(stamps)
            cases.append({
                "name": f"{run['app']}/{run['arm']}/{run['policy']}/{run['rep']}/{name}",
                "group": run["app"], "test": run["test"], "start": offset, "length": len(stamps),
                "n": int(len(stamps)), "ctx_mask": mask.tolist(),
            })
            daemon_chunks.append(daemon.astype(np.float16))
            token_chunks.append(token.astype(np.float16))
            ctx_chunks.append(context.astype(np.float16))
            label_chunks.append(label.astype(np.float16))
            excess_chunks.append(np.zeros(len(stamps), np.float16))
            valid_chunks.append(valid)
            outcome_chunks.append(here.astype(np.float16))
            offset += len(stamps)
        print(f"featurized {run['app']}/{run['rep']}: {len(prepared)} daemons, "
              f"kept {100.0 * run_kept / max(run_n, 1):.0f}% under {ERR_KEEP:.0%} errors"
              f"{'  test' if run['test'] else ''}", flush=True)

    dt = float(np.median(dt_samples)) if dt_samples else 0.05
    rings._write(out / "daemon.f16", daemon_chunks, np.float16)
    rings._write(out / "token.f16", token_chunks, np.float16)
    rings._write(out / "ctx.f16", ctx_chunks, np.float16)
    rings._write(out / "label.f16", label_chunks, np.float16)
    rings._write(out / "excess.f16", excess_chunks, np.float16)
    rings._write(out / "valid.u8", valid_chunks, np.uint8)
    rings._write(out / "outcome.f16", outcome_chunks, np.float16)

    baseline = {
        "train": rings._baseline(cases, daemon_chunks, label_chunks, valid_chunks, dt, False),
        "test": rings._baseline(cases, daemon_chunks, label_chunks, valid_chunks, dt, True),
    }
    correct_valid = [v & c for v, c in zip(valid_chunks, correct_chunks)]
    baseline_correct = {
        "train": rings._baseline(cases, daemon_chunks, label_chunks, correct_valid, dt, False),
        "test": rings._baseline(cases, daemon_chunks, label_chunks, correct_valid, dt, True),
    }
    manifest = {
        "label": "luna_cpuset",
        "source": TAG,
        "feature_alloc": "previous_tick",
        "valid_when": {"error_rate_max": ERR_KEEP, "routes": "all"},
        "outcome": ["error_rate", "throughput", "p50_ms", "p95_ms", "p99_ms", "correct"],
        "correct_when": {"error_rate_max": luna.ERR_MAX, "throughput_min": luna.TPUT_MIN,
                         "p95_max": "max(4 * p50_ms, 100)"},
        "dt_s": dt,
        "stride": rings.STRIDE,
        "req_max": rings.REQ_MAX,
        "cases": cases,
        "ticks_changed": moved,
        "ticks_valid": kept,
        "baseline_own_alloc": baseline,
        "baseline_correct": baseline_correct,
    }
    json.dump(manifest, open(out / "manifest.json", "w"), indent=1)
    train_n = sum(c["n"] for c in cases if not c["test"])
    test_n = sum(c["n"] for c in cases if c["test"])
    print(f"wrote {out}: {sum(not c['test'] for c in cases)} train / {sum(c['test'] for c in cases)} test cases, "
          f"{train_n} / {test_n} ticks, kept {kept} of {labelled} under {ERR_KEEP:.0%} errors, "
          f"dt {dt * 1e3:.1f} ms, "
          f"changed {moved} of {max(labelled, 1)} ticks ({100.0 * moved / max(labelled, 1):.2f}%)")
    print("baseline, predict the previous tick's cpuset")
    print("  train", json.dumps(baseline["train"].get("all", {})))
    print("  test ", json.dumps(baseline["test"].get("all", {})))
    print("baseline on correct decisions only")
    print("  train", json.dumps(baseline_correct["train"].get("all", {})))
    print("  test ", json.dumps(baseline_correct["test"].get("all", {})))
    if test_n == 0 or train_n == 0 or kept == 0:
        sys.exit("no labelled ticks")


if __name__ == "__main__":
    main()
