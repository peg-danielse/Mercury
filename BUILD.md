# Building Mercury

Mercury is a C++ worker plus the DeathStarBench services it drives. `cmake` builds all of it.

Tested on Debian 13 (trixie), x86_64. Ubuntu is not tested.

## Quick start

```
tools/setup/install-deps.sh          # apt packages (sudo), then libmongoc 2 and AMQP-CPP into ~/opt
source ~/opt/env.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j6
```

`install-deps.sh` is safe to rerun: it skips anything already installed. `DRY_RUN=1` prints the steps without running them. The prefix can be changed with `PREFIX=/path`.

## What the build needs

### From apt (sudo)

| Package | Why |
|---|---|
| `build-essential`, `cmake`, `make`, `pkg-config`, `git`, `curl` | toolchain |
| `libboost-all-dev` | Boost log, log_setup, program_options, chrono, thread, filesystem (Boost 1.83 tested) |
| `libssl-dev` | OpenSSL |
| `nlohmann-json3-dev` | nlohmann_json |
| `libyaml-cpp-dev` | yaml-cpp |
| `libmemcached-dev` | libmemcached (pkg-config `libmemcached`) |
| `libevent-dev` | `event` |
| `libthrift-dev` | Thrift 0.19 (pkg-config `thrift`) |
| `golang-go` | Go 1.24 or later for `benchmarks/deathstar-socialNetwork/goperate` (its `go.mod` says 1.24) |

### Built from source by `install-deps.sh` (no sudo, into `~/opt`)

| Library | Version | Why not apt |
|---|---|---|
| mongo-c-driver (libmongoc 2, pkg-config `mongoc2`) | 2.5.6 | Debian ships only libmongoc 1.30. The services use the libmongoc 2 API, with compat headers in `cmake/benchmarks.cmake`. |
| AMQP-CPP (`amqpcpp`) | v4.3.27 | not packaged for Debian 13 |

`~/opt/env.sh` sets `PKG_CONFIG_PATH`, `CMAKE_PREFIX_PATH`, `CPATH` and `LD_LIBRARY_PATH` so cmake and the compiler find these. Source it before every cmake configure.

### Fetched by cmake at pinned tags

These need network access at configure time. The tags are in `cmake/benchmarks.cmake`.

| Dependency | Tag |
|---|---|
| opentracing-cpp | v1.6.0 |
| hiredis | v1.2.0 |
| redis-plus-plus | 1.3.13 |
| rabbitmq-c | v0.15.0 |
| jaeger-client-cpp | v0.9.0 |
| cpp-jwt | v1.5.1 |
| SimpleAmqpClient | v2.5.1 |
| cpp_redis | 4.3.1 |
| tacopie | 3.2.0 |

### Benchmark sources (git submodule)

The apps, stressors, and runtimes are one submodule, `benchmarks`, checked out at the commit the superproject records:

```
git submodule update --init benchmarks
```

`benchmarks/README.md` describes the layout. A shallow (`--depth 1`) update does not land on that commit.

### Thrift and the media services

The media services' generated code is Thrift 0.12, and it includes `<thrift/stdcxx.h>`, which the system Thrift 0.19 does not provide. `cmake/benchmarks.cmake` writes that header as a small shim in `build/media-compat/` and puts it first on the media include path. No Thrift change is needed.

## Verifying the build

```
cmake --build build -j6
```

`-j6` on a 32 GB machine. stress-ng compiles with 4 jobs of its own inside this build, and `-j$(nproc)` exhausts that much RAM.

Expect no errors. Targets that matter for the experiments are `mercury`, `hotel`, the social services, `goperate`, and `media_*`.

## What the experiments need beyond the build

These are not needed to compile, but `src/test/dsb_ab.sh` and `compare_systems.sh` require them:

- `sudo` without a password, for the cgroup setup (`/sys/fs/cgroup/mercury`).
- A host CPU layout file, `benchmarks/perturb/perturb/hosts/<hostname>.env`. dtn-01 is covered; other hosts need their own file.
- The built `benchmarks/` tree (`DSB_ROOT`, the default). Social and media also need `/usr/bin/redis-server` and `/usr/lib/rabbitmq/bin/rabbitmq-server`. `README.md` is the short path through a `compare_systems.sh` run.
- Python 3: the tools use the standard library. Plotting needs `matplotlib`, and `tools/learn` needs `torch`. Neither is installed by the script.

## Not verified here

- Java and the train-ticket benchmark. Nothing in this build needs them, and I did not check that path.
- Ubuntu and other distributions.
- The Docker deployment in `benchmarks/deathstar-mediaMicroservices/README.md`.
