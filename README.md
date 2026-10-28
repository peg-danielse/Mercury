# Mercury

C++ worker and the DeathStarBench apps it drives. Tested on Debian 13, x86_64.

## Install

```
git clone --recurse-submodules git@github.com:peg-danielse/Mercury.git
cd Mercury
git submodule update --init benchmarks   # the apps, stressors, and runtimes
tools/setup/install-deps.sh              # apt packages, then libmongoc 2 and AMQP-CPP into ~/opt
source ~/opt/env.sh
cmake -S . -B build
cmake --build build -j6
```

`install-deps.sh` is safe to rerun. `BUILD.md` lists every package. Source `~/opt/env.sh` before each cmake configure. Use `-j6` on a 32 GB machine: stress-ng compiles with 4 jobs of its own inside this build, and a full `-j$(nproc)` exhausts that much RAM.

`cmake --build build` produces `build/mercury`, the hotel, social, and media services and operators, gomaxprocs, stress-ng, the runtime wrappers, and the `mongod` and `memcached` binaries those wrappers exec. Train Ticket is built only when both `java` and `mvn` are on `PATH`.

Social and media also exec system servers:

- `/usr/bin/redis-server` (package `redis-server`)
- `/usr/lib/rabbitmq/bin/rabbitmq-server` (package `rabbitmq-server`)

## Before a compare_systems run

Passwordless sudo. The scripts call `sudo -n` for cgroups, starting Mercury, and moving database directories. `sudo -n true` must succeed.

A CPU layout for this machine, `benchmarks/perturb/perturb/hosts/$(hostname -s).env`. `Apollo.env` (32 CPUs) and `dtn-01.env` (64 CPUs) are the checked-in examples. With no file, the pool is CPUs 1–60. Copy the closest example to `$(hostname -s).env` and set `POOL`, `GEN_CPU`, `OP_CPU`, `SAMPLE_CPU`, and `TRACE_CPUS` to CPUs this machine has. `GEN_CPU` stays inside `POOL`. The others stay outside it. Mercury itself uses CPU 0.

## Starting a run

Hotel starts from an empty database, so it needs no seed:

```
SYSTEMS="saturate l3 shift l3shift degrade" \
  src/test/compare_systems.sh "$HOME/mercury-runs/hotel" hotel
```

That is one run of each system, interleaved, at 1600 requests/s. Each run is a 30 s warmup and 12 phases of 20 s off and 30 s on, on a pool of 8 CPUs: about 12 minutes of load, plus startup. Five systems is about an hour. Progress is appended to `$HOME/mercury-runs/hotel/progress.log`. A finished run is `OUT/<system>/r1/DONE`. Running the same command again skips those. At the end, `compare_report.py` writes `report.txt`.

The default `SYSTEMS` also includes `llm`, which reads `GEMINI_API_KEY` from the environment, or from stdin when it is unset. The key is not written to disk.

```
src/test/compare_systems.sh OUT hotel|social|media
```

`REPEATS=3` runs every system three times (`OUT/<system>/r1` … `r3`). `STOP_AT="13:00 tomorrow"` starts no run that would finish after that time. `NOPOOL=1` runs without a CPU pool. The header of `src/test/compare_systems.sh` lists the other knobs (`RATE`, `PHASES`, `POOL_START`, `ROUTE`, `LLM_MODELS`).

Social restores a seed snapshot (the graph and posts). Build it once, record the path, then start the comparison. Redis must be installed.

```
src/test/dsb_seed.sh social "$HOME/mercury-runs/seed-social"
echo "$HOME/mercury-runs/seed-social" > ~/.seed_social
src/test/compare_systems.sh "$HOME/mercury-runs/social" social
```

Media restores `~/.seed_media` (or `MEDIA_SEED`), a snapshot of MongoDB on port 27018 and `dump.rdb`. With the stack stopped, `benchmarks/deathstar-mediaMicroservices/scripts/snapshot_seed.sh OUT` writes one. Media has no frontend, so leave `degrade` out of `SYSTEMS`.
