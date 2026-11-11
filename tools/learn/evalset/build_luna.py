#!/usr/bin/env python3
"""Build the Luna imitation set from the 2026-11-02 hotel, social and media runs.

Each case is one daemon of one finished llm40-luna or llm24-luna run, on the same
50 ms grid as build.py. The label of a tick is the cpuset size Luna left that
daemon with (the ring's cores column). With SC_LLM_OWNS the autoscaler is
detached, so that size is her scale_daemon count, held until she changes it.

The allocation slots of the feature vector are the previous tick's size. The
current size is the label, so a model cannot copy it out of its own input. The
loss is unchanged from train.py: there is no second policy here, so the excess
weight is zero. The last repeat of each application is the test set (hotel and
social r7, media r6), for both budgets. outcome.f16 adds the critical-route
window of each tick: error rate, throughput against the offered rate, and the
latency body and tail (p50, p95, p99 in milliseconds). Its last column is 1
when that window was a correct decision, which is an error rate of at most 1%,
throughput of at least 90% of the offered rate, and a p95 within
max(4 * p50, 100 ms). train.py learns every tick and also scores the match to
Luna on those correct ticks alone.

  python build_luna.py [OUT_DIR]

OUT_DIR defaults to <repo>/data/learn/luna-20261102. Numpy only; no torch.
"""

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as rings  # noqa: E402
from common import APPS, CRITICAL  # noqa: E402

# A window counts as a correct decision when requests land, the offered rate is
# served, and the tail has not split away from the body.
ERR_MAX = 0.01
TPUT_MIN = 0.90
TAIL = 4.0
TAIL_FLOOR_MS = 100.0

REPO = Path(__file__).resolve().parents[3]
COMPARE = REPO / "data" / "dsb" / "compare"


def discover():
    """Finished runs only: compare_systems.sh writes DONE after a complete run."""
    found = []
    for app in APPS:
        base = COMPARE / f"{app}-luna-20261102"
        if not base.is_dir():
            continue
        for done in sorted(base.glob("*/*/DONE")):
            run_dir = Path(done.read_text().strip())
            if not (run_dir / "rings").is_dir():
                print(f"skip {done.parent}: DONE points at {run_dir}, which has no rings")
                continue
            found.append({
                "app": app,
                "arm": "luna",
                "policy": done.parent.parent.name,
                "rep": done.parent.name,
                "dir": run_dir,
            })
    last = {}
    for run in found:
        last[run["app"]] = max(last.get(run["app"], 0), int(run["rep"][1:]))
    for run in found:
        run["test"] = int(run["rep"][1:]) == last[run["app"]]
    return found


def _percentile_ms(counts, q):
    bound = rings.percentile(counts, q)
    if bound is None:
        return 0.0
    return bound / 1e6


def critical_outcome(path, routes):
    """One row per load-generator window, on the critical routes only.

    Columns: error rate, completions/offered, p50 ms, p95 ms, p99 ms, correct.
    The row's time is the window start. Ticks before the first window stay zero.
    """
    grouped = {}
    for line in open(path):
        row = json.loads(line)
        if row.get("route") not in routes:
            continue
        start = int(row["t0_ns"])
        slot = grouped.get(start)
        if slot is None:
            slot = {"stop": int(row["t1_ns"]), "sent": 0, "done": 0, "errors": 0,
                    "offered": 0.0, "hist": {}}
            grouped[start] = slot
        slot["stop"] = max(slot["stop"], int(row["t1_ns"]))
        slot["sent"] += int(row.get("sent", 0))
        slot["done"] += int(row.get("done", 0))
        slot["errors"] += int(row.get("errors", 0))
        slot["offered"] += int(row.get("clients", 0)) * max((int(row["t1_ns"]) - start) / 1e9, 1e-6)
        for pair in row.get("hist") or []:
            if len(pair) < 2 or not pair[1]:
                continue
            bound = int(pair[0])
            slot["hist"][bound] = slot["hist"].get(bound, 0) + int(pair[1])
    if not grouped:
        return np.zeros(0, np.int64), np.zeros((0, 6), np.float32)
    times = sorted(grouped)
    out = np.zeros((len(times), 6), np.float32)
    for i, start in enumerate(times):
        slot = grouped[start]
        err = slot["errors"] / slot["sent"] if slot["sent"] else 1.0
        tput = slot["done"] / slot["offered"] if slot["offered"] else 0.0
        p50 = _percentile_ms(slot["hist"], 0.5)
        p95 = _percentile_ms(slot["hist"], 0.95)
        p99 = _percentile_ms(slot["hist"], 0.99)
        correct = err <= ERR_MAX and tput >= TPUT_MIN and p50 > 0 and p95 <= max(TAIL * p50, TAIL_FLOOR_MS)
        out[i] = (err, tput, p50, p95, p99, 1.0 if correct else 0.0)
    return np.array(times, np.int64), out


def lag(cores):
    """The size held on the previous tick. Tick 0 keeps its own size."""
    out = np.array(cores, copy=True)
    if len(out) > 1:
        out[1:] = cores[:-1]
    return out


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "data" / "learn" / "luna-20261102"
    out.mkdir(parents=True, exist_ok=True)
    runs = discover()
    print(f"{len(runs)} runs", flush=True)
    held_out = sorted({(r["app"], r["policy"], r["rep"]) for r in runs if r["test"]})
    print("test:", " ".join(f"{a}/{p}/{rep}" for a, p, rep in held_out), flush=True)

    daemon_chunks, token_chunks, ctx_chunks = [], [], []
    label_chunks, excess_chunks, valid_chunks, outcome_chunks = [], [], [], []
    correct_chunks = []
    cases = []
    offset = 0
    dt_samples = []
    moved = 0
    labelled = 0
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
            print(f"skip {run['app']}/{run['policy']}/{run['rep']}: no daemon rings")
            continue
        outcome_t, outcome = critical_outcome(directory / "loadgen.hist.jsonl", set(CRITICAL[run["app"]]))
        stamps_s, wait_s, busy_s, threads_s = rings.sample_table(directory / "sample.jsonl")
        aligned = {}
        medians = []
        for name, (stamps, clock, pressure, cores) in prepared.items():
            medians.append((float(np.median(clock)), name))
            aligned[name] = rings.daemon_features(
                stamps, clock, pressure, lag(cores),
                rings._column(stamps_s, wait_s.get(name), stamps),
                rings._column(stamps_s, busy_s.get(name), stamps),
                rings._column(stamps_s, threads_s.get(name), stamps),
            )
        order = [name for _, name in sorted(medians, key=lambda item: (-item[0], item[1]))]
        for name, (stamps, _clock, _pressure, cores) in prepared.items():
            daemon, token, _own = aligned[name]
            peers = [peer for peer in order if peer != name][:9]
            context = np.zeros((len(stamps), 9, 5), np.float32)
            mask = np.zeros(9, np.int64)
            for slot, peer in enumerate(peers):
                context[:, slot] = rings.at_times(prepared[peer][0], aligned[peer][2], stamps)
                mask[slot] = 1
            label = np.asarray(cores, np.float32)
            valid = np.ones(len(stamps), np.uint8)
            here = rings.at_times(outcome_t, outcome, stamps)
            correct_chunks.append((here[:, 5] > 0.5).astype(np.uint8))
            if len(stamps) > 1:
                dt_samples.append(float(np.median(np.diff(stamps))) / 1e9)
                moved += int(np.count_nonzero(label[1:] != label[:-1]))
            labelled += len(stamps)
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
        correct_n = int(sum(int(c.sum()) for c in correct_chunks[-len(prepared):]))
        tick_n = sum(len(prepared[name][0]) for name in prepared)
        print(f"featurized {run['app']}/{run['policy']}/{run['rep']}: {len(prepared)} daemons, "
              f"correct {100.0 * correct_n / max(tick_n, 1):.0f}%"
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
        "feature_alloc": "previous_tick",
        "outcome": ["error_rate", "throughput", "p50_ms", "p95_ms", "p99_ms", "correct"],
        "correct_when": {"error_rate_max": ERR_MAX, "throughput_min": TPUT_MIN,
                         "p95_max": "max(4 * p50_ms, 20)"},
        "dt_s": dt,
        "stride": rings.STRIDE,
        "req_max": rings.REQ_MAX,
        "cases": cases,
        "ticks_changed": moved,
        "baseline_own_alloc": baseline,
        "baseline_correct": baseline_correct,
    }
    json.dump(manifest, open(out / "manifest.json", "w"), indent=1)
    train_n = sum(c["n"] for c in cases if not c["test"])
    test_n = sum(c["n"] for c in cases if c["test"])
    print(f"wrote {out}: {sum(not c['test'] for c in cases)} train / {sum(c['test'] for c in cases)} test cases, "
          f"{train_n} / {test_n} ticks, dt {dt * 1e3:.1f} ms, "
          f"changed {moved} of {max(labelled, 1)} ticks ({100.0 * moved / max(labelled, 1):.2f}%)")
    print("baseline, predict the previous tick's cpuset")
    print("  train", json.dumps(baseline["train"].get("all", {})))
    print("  test ", json.dumps(baseline["test"].get("all", {})))
    print("baseline on correct decisions only")
    print("  train", json.dumps(baseline_correct["train"].get("all", {})))
    print("  test ", json.dumps(baseline_correct["test"].get("all", {})))
    if test_n == 0 or train_n == 0:
        sys.exit("no labelled ticks")


if __name__ == "__main__":
    main()
