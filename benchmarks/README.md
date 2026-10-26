# Benchmarks

Microservice benchmark workloads, the Mercury load/operator tooling, and perturbation experiments.

| Path | What it is |
|---|---|
| `perturb/` | Perturbation/placement experiments (`perturb.py`, `placer.py`, `counters.py`). See [perturb/README.md](perturb/README.md). Run outputs (`perturb/labels/`) are not tracked. |
| `demo/` | End-to-end demo driver (`run-demo.sh`, plotting/compare scripts). See [demo/DEMO.md](demo/DEMO.md). `bin/`, `lib/`, `opt/`, `runs/`, `state/` are local-only. |
| `deathstar-hotelReservations/` | DeathStarBench hotelReservation snapshot with Mercury load generator (`pc` branch merged here with history). |
| `deathstar-socialNetwork/` | DeathStarBench socialNetwork snapshot with Go operator `goperate` (`pc` branch merged here with history). |
| `train-ticket/` | Train-Ticket services source (node_modules/binaries not tracked). |
| `gomaxprocs/` | Small Go GOMAXPROCS test service/operator (binaries not tracked). |
| `runtime/` | Local service runtimes. Only hand-written wrapper sources (`*-server.c`, `mongod.c`) and jaeger scripts/config/patches are tracked; binaries, DB data and per-port state are local-only. |
| `stress-ng/` | stress-ng sources (build products are not tracked). Submodule branch `stress-ng`, imported from https://github.com/ColinIanKing/stress-ng. |

## Not tracked: upstream clones
- `runtime/jaeger/src/`: https://github.com/jaegertracing/jaeger (local patches in `runtime/jaeger/patches/`)

## Branches
- `main`: this monorepo. `perturb`: the original top-level branch, unchanged.
- `hotel/*` and `social/*`: the other branches of the former nested repos (demo, jaeger, jaeger-dtn01, main, mercury-loadgen / mercury-operator), imported as-is with their original history. Their files are at the repo **root** (the layout of the old standalone repo), not under the subdirectory. To compare with main use e.g. `git diff hotel/jaeger main:deathstar-hotelReservations`.
- The demo repo had only the `demo` branch, which is merged into `main` under `demo/`.
- `stress-ng`: upstream stress-ng `master`, imported with its history. Files are at the repo root. The remote `upstream` in that checkout is ColinIanKing/stress-ng.
