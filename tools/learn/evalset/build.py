#!/usr/bin/env python3
"""Build one dataset from the 2026-11-01 hotel, social and media evaluation runs.

Those runs recorded cgroup rings and latency histograms, not the per-thread traces
train_seq.py reads. Each case is one daemon of one run, on a 50 ms grid (every 5th
ring sample). Repeat 2 is held out. MLP, GRU and the memory transformer train on
this same split.

The label of a tick is C*, the smallest median cpuset among the policies of that
repeat which kept the critical-route p95 within 20% of the best policy, for the
same offered load and the same antagonist (kind, level, victim). The gaps with no
antagonist are a separate scenario. A scenario with fewer than two such policies,
or fewer than 30 completed critical requests, is left unlabelled.

  python build.py [OUT_DIR]

OUT_DIR defaults to <repo>/data/learn/eval-20261101. Numpy only; no torch.
"""

import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import (  # noqa: E402
    APPS, ARMS, CRITICAL, MARGIN, MIN_POLICIES, MIN_REQUESTS, REQ_MAX, STRIDE, metrics,
)

REPO = Path(__file__).resolve().parents[3]
COMPARE = REPO / "data" / "dsb" / "compare"


def read_mring(path):
    """Ring archive: timestamps and counter columns. Same layout as train_seq.read_mring."""
    data = open(path, "rb").read()
    _, count, _, record = struct.unpack_from("<4I", data, 8)
    committed, offset = struct.unpack_from("<2Q", data, 24)
    names, at = [], 40
    for _ in range(count):
        (size,) = struct.unpack_from("<I", data, at)
        names.append(data[at + 4:at + 4 + size].decode())
        at += 4 + size
    committed = min(committed, (len(data) - offset) // record)
    rows = np.frombuffer(data, dtype=np.uint64, count=committed * record // 8, offset=offset)
    rows = rows.reshape(committed, record // 8)
    return rows[:, 0].astype(np.int64), {n: rows[:, 4 + i].astype(np.float64) for i, n in enumerate(names)}


def discover():
    """Every finished run directory that has rings, a histogram and an antagonist log."""
    found = []
    for app in APPS:
        for suffix, arm in ARMS:
            base = COMPARE / f"{app}-eval-20261101{suffix}"
            if not base.is_dir():
                continue
            for policy in sorted(p.name for p in base.iterdir() if p.is_dir() and p.name != "figs"):
                for rep in ("r1", "r2"):
                    root = base / policy / rep
                    if not root.is_dir():
                        continue
                    samples = sorted(root.rglob("sample.jsonl"))
                    runs = [p.parent for p in samples if (p.parent / "rings").is_dir()
                            and (p.parent / "loadgen.hist.jsonl").is_file()
                            and (p.parent / "antagonists.jsonl").is_file()]
                    if not runs:
                        print(f"skip {app}/{arm}/{policy}/{rep}: no complete run directory")
                        continue
                    if len(runs) > 1:
                        print(f"note {app}/{arm}/{policy}/{rep}: {len(runs)} run directories, using {runs[-1].name}")
                    found.append({
                        "app": app, "arm": arm, "policy": policy, "rep": rep,
                        "test": rep == "r2", "dir": runs[-1],
                    })
    return found


def load_windows(path):
    rows = []
    for line in open(path):
        row = json.loads(line)
        rows.append((int(row["t0_ns"]), int(row["t1_ns"]), int(row["clients"]), row["route"], row.get("hist") or []))
    rows.sort()
    return rows


def load_phases(path):
    """(t_on, t_off, kind, level, victim) for each antagonist-on interval."""
    events = [json.loads(line) for line in open(path)]
    offs = [e for e in events if e.get("event") == "off"]
    phases = []
    for event in events:
        if event.get("event") != "on":
            continue
        start = int(event["t_ns"])
        stop = next((int(e["t_ns"]) for e in offs if e.get("phase") == event.get("phase") and int(e["t_ns"]) >= start), start)
        phases.append((start, stop, str(event.get("kind")), str(event.get("level")), str(event.get("victim") or "")))
    return phases


def segments(t_min, t_max, phases, windows):
    """Split the run at antagonist edges and load changes. Each piece has one scenario key."""
    cuts = {t_min, t_max}
    for start, stop, *_ in phases:
        cuts.add(min(t_max, max(t_min, start)))
        cuts.add(min(t_max, max(t_min, stop)))
    previous = None
    for start, _, clients, _, _ in windows:
        if previous is None or clients != previous:
            cuts.add(min(t_max, max(t_min, start)))
            previous = clients
    cuts = sorted(cuts)
    pieces = []
    for left, right in zip(cuts, cuts[1:]):
        if right - left < 1_000_000_000:
            continue
        mid = (left + right) // 2
        phase = next(((k, lv, v) for s, e, k, lv, v in phases if s <= mid < e), None)
        overlap = defaultdict(int)
        for start, stop, clients, _, _ in windows:
            span = min(right, stop) - max(left, start)
            if span > 0:
                overlap[clients] += span
        if not overlap:
            continue
        load = max(overlap, key=overlap.get)
        if phase is None:
            key = ("off", str(load), "", "")
        else:
            key = (phase[0], str(load), phase[1], phase[2])
        pieces.append((left, right, key))
    return pieces


def pool_histogram(windows, pieces, routes):
    """Sum critical-route histogram buckets whose window midpoint falls in a piece of this key."""
    spans = [(a, b) for a, b, _ in pieces]
    counts = defaultdict(int)
    done = 0
    for start, stop, _, route, hist in windows:
        if route not in routes:
            continue
        mid = (start + stop) // 2
        if not any(a <= mid < b for a, b in spans):
            continue
        for pair in hist:
            if len(pair) < 2 or not pair[1]:
                continue
            counts[int(pair[0])] += int(pair[1])
            done += int(pair[1])
    return counts, done


def percentile(counts, q):
    if not counts:
        return None
    items = sorted(counts.items())
    total = sum(c for _, c in items)
    if total <= 0:
        return None
    need = q * total
    have = 0
    for bound, count in items:
        have += count
        if have >= need:
            return float(bound)
    return float(items[-1][0])


def ring_series(path):
    """Per-gap core rates on the ring's own clock, dropping the opening sample."""
    stamps, cols = read_mring(path)
    if len(stamps) < STRIDE + 2 or "task-clock" not in cols or "cores" not in cols:
        return None
    gap = np.diff(stamps).astype(np.float64)
    gap[gap <= 0] = np.nan
    clock = np.nan_to_num(cols["task-clock"][1:] / gap, nan=0.0, posinf=0.0, neginf=0.0)
    if "pressure" in cols:
        pressure = np.clip(np.nan_to_num(cols["pressure"][1:] / gap, nan=0.0, posinf=0.0, neginf=0.0), 0, 5)
    else:
        pressure = np.zeros_like(clock)
    cores = cols["cores"][1:]
    keep = np.arange(0, len(clock), STRIDE)
    return stamps[1:][keep], clock[keep], pressure[keep], cores[keep]


def sample_table(path):
    """Per daemon, aligned to sample.jsonl rows: run-queue wait in cores, busy threads, thread count."""
    rows = [json.loads(line) for line in open(path)]
    stamps = np.array([int(r["ts_ns"]) for r in rows], dtype=np.int64)
    wait, busy, threads = {}, {}, {}
    for i, row in enumerate(rows):
        gap = row.get("iv_ns") or 1
        for name, daemon in row.get("daemons", {}).items():
            wait.setdefault(name, np.zeros(len(rows)))[i] = daemon.get("wait_ns", 0) / gap
            busy.setdefault(name, np.zeros(len(rows)))[i] = daemon.get("busy", 0)
            threads.setdefault(name, np.zeros(len(rows)))[i] = daemon.get("threads", 0)
    return stamps, wait, busy, threads


def at_times(src_t, src, dst_t):
    """Last source row at or before each destination time. Zeros before the source starts."""
    if src_t is None or len(src_t) == 0 or len(src) == 0:
        return np.zeros((len(dst_t),) + src.shape[1:], np.float32)
    index = np.searchsorted(src_t, dst_t, side="right") - 1
    out = np.zeros((len(dst_t),) + src.shape[1:], np.float32)
    ok = index >= 0
    out[ok] = src[np.clip(index, 0, len(src) - 1)][ok]
    return out


def daemon_features(stamps, clock, pressure, cores, wait, busy, threads):
    """The 25-wide vector seq_models.featurize packs, filled from a cgroup ring.

    There is no per-thread trace, so the thread sums and the maxima are the one
    cgroup rate, context switches, migrations and the NUMA shares stay zero, and
    run is the task-clock rate. Slot order matches featurize so the three models
    share it. It is not the vector GruFeatures builds, and a checkpoint trained
    here is not a weights file for the C++ autoscaler.
    """
    n = len(stamps)
    delta = np.zeros(n, np.float32)
    delta[1:] = cores[1:] - cores[:-1]
    since = np.full(n, 10_000.0)
    last = None
    recent = np.zeros(n, np.float32)
    changed = np.zeros(n, np.float32)
    changed[1:] = cores[1:] != cores[:-1]
    cover = 0.0
    left = 0
    for i in range(n):
        if changed[i]:
            last = stamps[i]
        if last is not None:
            since[i] = (stamps[i] - last) / 1e7
        cover += changed[i]
        while stamps[i] - stamps[left] > 1_000_000_000:
            cover -= changed[left]
            left += 1
        recent[i] = cover
    occupancy = np.clip(clock / np.maximum(cores, 1.0), 0, 4)
    held = np.clip(pressure, 0, 1)
    stacked = np.stack([
        clock, clock, wait, clock, wait,
        np.zeros(n), np.zeros(n),
        busy / 16.0, threads / 100.0,
        occupancy, held,
        clock, wait,
        cores / 16.0, delta / 4.0, np.log1p(since) / 10.0, recent / 10.0,
    ], 1)
    daemon = np.concatenate([stacked, np.zeros((n, 8), np.float32)], 1).astype(np.float32)
    token = np.stack([
        clock, clock, wait, np.zeros(n), np.zeros(n), occupancy, held,
        np.zeros(n), np.zeros(n),
        np.zeros(n), np.zeros(n), np.zeros(n), np.zeros(n),
        np.zeros(n), np.zeros(n), np.zeros(n), np.zeros(n),
    ], 1).astype(np.float32)
    step = np.zeros(n, np.float32)
    step[1:] = cores[1:] - cores[:-1]
    context = np.stack([
        clock / 16.0, held, cores / 16.0, step / 4.0, clock / np.maximum(cores, 1.0),
    ], 1).astype(np.float32)
    return daemon, token, context


def cores_over(stamps, cores, pieces):
    parts = []
    for left, right, _ in pieces:
        i0 = int(np.searchsorted(stamps, left, side="left"))
        i1 = int(np.searchsorted(stamps, right, side="left"))
        if i1 > i0:
            parts.append(cores[i0:i1])
    if not parts:
        return None
    return float(np.median(np.concatenate(parts)))


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO / "data" / "learn" / "eval-20261101"
    out.mkdir(parents=True, exist_ok=True)
    runs = discover()
    print(f"{len(runs)} runs", flush=True)

    # Pass 1: one p95 and one median cpuset per daemon, per scenario, per run.
    described = []
    for run in runs:
        directory = run["dir"]
        windows = load_windows(directory / "loadgen.hist.jsonl")
        phases = load_phases(directory / "antagonists.jsonl")
        rings = {}
        for path in sorted((directory / "rings").glob("*.mring")):
            name = path.name[:-len(".mring")]
            if name.startswith("cpu-") or "loadgen" in name or "jaeger" in name:
                continue
            series = ring_series(path)
            if series is not None:
                rings[name] = series
        if not rings or not windows:
            print(f"skip {run['app']}/{run['arm']}/{run['policy']}/{run['rep']}: empty rings or histogram")
            continue
        t_min = min(int(s[0][0]) for s in rings.values())
        t_max = max(int(s[0][-1]) for s in rings.values())
        pieces = segments(t_min, t_max, phases, windows)
        by_key = defaultdict(list)
        for piece in pieces:
            by_key[piece[2]].append(piece)
        scenarios = {}
        for key, group in by_key.items():
            counts, done = pool_histogram(windows, group, set(CRITICAL[run["app"]]))
            held = {}
            for daemon, series in rings.items():
                median = cores_over(series[0], series[3], group)
                if median is not None and median >= 0.5:
                    held[daemon] = median
            scenarios[key] = {"p95": percentile(counts, 0.95), "n": done, "cores": held}
        run = dict(run)
        run["rings"] = rings
        run["pieces"] = pieces
        run["scenarios"] = scenarios
        described.append(run)
        print(f"read {run['app']}/{run['arm']}/{run['policy']}/{run['rep']}: "
              f"{len(rings)} daemons, {len(scenarios)} scenarios", flush=True)

    # The label is shared by every policy of one repeat that faced the same scenario.
    grouped = defaultdict(list)
    for run in described:
        for key in run["scenarios"]:
            grouped[(run["app"], run["rep"], key)].append(run)

    labelled = skipped = 0
    cstar_values = defaultdict(list)
    for (app, rep, key), members in grouped.items():
        scored = [m for m in members if m["scenarios"][key]["p95"] is not None
                  and m["scenarios"][key]["n"] >= MIN_REQUESTS]
        if len(scored) < MIN_POLICIES:
            for member in members:
                member["scenarios"][key]["label"] = None
            skipped += 1
            continue
        best = min(m["scenarios"][key]["p95"] for m in scored)
        good = [m for m in scored if m["scenarios"][key]["p95"] <= MARGIN * best]
        names = set()
        for member in good:
            names.update(member["scenarios"][key]["cores"])
        cstar = {}
        for name in names:
            votes = [m["scenarios"][key]["cores"][name] for m in good if name in m["scenarios"][key]["cores"]]
            if votes:
                cstar[name] = float(min(votes))
        for member in members:
            own = member["scenarios"][key]["p95"]
            if own is None or member["scenarios"][key]["n"] < MIN_REQUESTS or not cstar:
                member["scenarios"][key]["label"] = None
                continue
            member["scenarios"][key]["label"] = (cstar, max(0.0, own / best - 1.0))
        labelled += 1
        for value in cstar.values():
            cstar_values[app].append(value)
    print(f"scenarios labelled {labelled}, skipped {skipped}", flush=True)

    # Pass 2: features, stamped with that label.
    daemon_chunks, token_chunks, ctx_chunks = [], [], []
    label_chunks, excess_chunks, valid_chunks = [], [], []
    cases = []
    offset = 0
    dt_samples = []
    for run in described:
        directory = run["dir"]
        stamps_s, wait_s, busy_s, threads_s = sample_table(directory / "sample.jsonl")
        prepared = {name: series for name, series in run["rings"].items()}
        aligned = {}
        medians = []
        for name, (stamps, clock, pressure, cores) in prepared.items():
            medians.append((float(np.median(clock)), name))
            aligned[name] = daemon_features(
                stamps, clock, pressure, cores,
                _column(stamps_s, wait_s.get(name), stamps),
                _column(stamps_s, busy_s.get(name), stamps),
                _column(stamps_s, threads_s.get(name), stamps),
            )
        order = [name for _, name in sorted(medians, key=lambda item: (-item[0], item[1]))]
        for name, (stamps, clock, pressure, cores) in prepared.items():
            daemon, token, _own = aligned[name]
            peers = [p for p in order if p != name][:9]
            context = np.zeros((len(stamps), 9, 5), np.float32)
            mask = np.zeros(9, np.int64)
            for slot, peer in enumerate(peers):
                context[:, slot] = at_times(prepared[peer][0], aligned[peer][2], stamps)
                mask[slot] = 1
            label = np.zeros(len(stamps), np.float32)
            excess = np.zeros(len(stamps), np.float32)
            valid = np.zeros(len(stamps), np.uint8)
            for left, right, key in run["pieces"]:
                stamped = run["scenarios"][key].get("label")
                if not stamped or name not in stamped[0]:
                    continue
                cstar, extra = stamped
                chosen = (stamps >= left) & (stamps < right)
                label[chosen] = cstar[name]
                excess[chosen] = extra
                valid[chosen] = 1
            if len(stamps) > 1:
                dt_samples.append(float(np.median(np.diff(stamps))) / 1e9)
            n = len(stamps)
            cases.append({
                "name": f"{run['app']}/{run['arm']}/{run['policy']}/{run['rep']}/{name}",
                "group": run["app"], "test": run["test"], "start": offset, "length": n,
                "n": int(valid.sum()), "ctx_mask": mask.tolist(),
            })
            daemon_chunks.append(daemon.astype(np.float16))
            token_chunks.append(token.astype(np.float16))
            ctx_chunks.append(context.astype(np.float16))
            label_chunks.append(label.astype(np.float16))
            excess_chunks.append(excess.astype(np.float16))
            valid_chunks.append(valid)
            offset += n
        run["rings"] = None
        print(f"featurized {run['app']}/{run['arm']}/{run['policy']}/{run['rep']}", flush=True)

    dt = float(np.median(dt_samples)) if dt_samples else 0.05
    _write(out / "daemon.f16", daemon_chunks, np.float16)
    _write(out / "token.f16", token_chunks, np.float16)
    _write(out / "ctx.f16", ctx_chunks, np.float16)
    _write(out / "label.f16", label_chunks, np.float16)
    _write(out / "excess.f16", excess_chunks, np.float16)
    _write(out / "valid.u8", valid_chunks, np.uint8)

    baseline = {"train": _baseline(cases, daemon_chunks, label_chunks, valid_chunks, dt, False),
                "test": _baseline(cases, daemon_chunks, label_chunks, valid_chunks, dt, True)}
    manifest = {
        "dt_s": dt, "stride": STRIDE, "margin": MARGIN, "req_max": REQ_MAX,
        "critical": CRITICAL, "cases": cases,
        "scenarios_labelled": labelled, "scenarios_skipped": skipped,
        "cstar": {app: _dist(values) for app, values in cstar_values.items()},
        "baseline_own_alloc": baseline,
    }
    json.dump(manifest, open(out / "manifest.json", "w"), indent=1)
    train_n = sum(c["n"] for c in cases if not c["test"])
    test_n = sum(c["n"] for c in cases if c["test"])
    print(f"wrote {out}: {sum(not c['test'] for c in cases)} train / {sum(c['test'] for c in cases)} test cases, "
          f"{train_n} / {test_n} labelled ticks, dt {dt * 1e3:.1f} ms")
    print("C* cores", json.dumps(manifest["cstar"]))
    print("baseline, predict the cores the run already held")
    print("  train", json.dumps(baseline["train"].get("all", {})))
    print("  test ", json.dumps(baseline["test"].get("all", {})))
    if test_n == 0 or train_n == 0:
        sys.exit("no labelled ticks")


def _column(stamps_s, values, dst):
    if values is None:
        return np.zeros(len(dst), np.float32)
    return at_times(stamps_s, np.asarray(values, np.float32), dst)


def _write(path, chunks, dtype):
    total = sum(len(c) for c in chunks)
    rest = chunks[0].shape[1:]
    mapped = np.lib.format.open_memmap(path, mode="w+", dtype=dtype, shape=(total, *rest))
    at = 0
    for chunk in chunks:
        mapped[at:at + len(chunk)] = chunk
        at += len(chunk)
    mapped.flush()
    del mapped


def _dist(values):
    if not values:
        return {}
    arr = np.array(values, np.float64)
    return {"n": int(len(arr)), "p10": float(np.percentile(arr, 10)), "p50": float(np.percentile(arr, 50)),
            "p90": float(np.percentile(arr, 90)), "mean": float(arr.mean())}


def _baseline(cases, daemon_chunks, label_chunks, valid_chunks, dt, test):
    """Request the cores the run already held. core-seconds use the dataset tick."""
    by = defaultdict(list)
    pairs = []
    for case, daemon, label, valid in zip(cases, daemon_chunks, label_chunks, valid_chunks):
        if bool(case["test"]) != test or case["n"] == 0:
            continue
        held = np.clip(np.round(daemon[:, 13].astype(np.float64) * 16), 1, REQ_MAX)
        pair = (held, label.astype(np.float64), valid.astype(bool))
        pairs.append(pair)
        by[case["group"]].append(pair)
    out = {"all": metrics(pairs, dt)}
    for group, group_pairs in sorted(by.items()):
        out[group] = metrics(group_pairs, dt)
    return out


if __name__ == "__main__":
    main()
