"""Antagonist driver for interference runs (INTERFERENCE_DESIGN.md step 2): a dsb_ab.sh SIDECAR that, once the
services are up, alternates OFF and ON phases. Each ON phase picks a victim daemon (weighted by its CPU use over the
2 s before onset) and one CPU c of its cpuset (weighted by that CPU's busy share), an
antagonist kind and level, starts stress-ng pinned relative to c, and REACT seconds later applies one action:

  none             leave everything as is
  move_victim      swap c out of the victim's cpuset for the cleanest free pool CPU, same NUMA node first (Mercury /cpus); the original
                   cpuset is restored when the phase ends, so every phase starts from the same placement
  move_antagonist  restart the antagonist on CPUs of another NUMA node (fewest daemons), same kind and level

Kinds (relative to c):  same   CPU hog on c itself (level 1: 50% load, 2: 100%)
                        smt    CPU hog on c's SMT sibling (50%, 100%)
                        l3     L3 thrash (stress-ng --cache) on the other core of c's L3 (1 or 2 of its threads)
                        membw  memory bandwidth (stress-ng --stream) on CPUs of c's NUMA node outside its L3 (2 or 4)
                        load   application load: no victim CPU and no stress-ng; the load generator's rate is
                               multiplied by --load-scale (level 1: 1.5, 2: 2.0) for the phase, by writing the factor
                               to RUN/loadgen.scale, which the load generator reads every window (Benchmarks-zeroctl
                               operate/autoscale.go loadScale). Not in the default --kinds.
Actions rotate strictly (none, move_victim, move_antagonist), kinds and levels come from a shuffled cycle, so every
(kind, level, action) occurs about equally often over a long run. Every step is one line in RUN/antagonists.jsonl with t_ns on
CLOCK_MONOTONIC (the clock of the rings and placement.jsonl). SIGTERM stops the antagonists, restores the victim and
exits.

  dsb_ab.sh ... with SIDECAR="python3 slo/interfere.py"   (victims auto; works for hotel and social)

Copied on 2026-10-21 from gnn-example/mercury/slo/interfere.py for compare_systems.sh, with two options:
  --actions none           the antagonists only switch on and off; only the system under test moves daemons
  --victims-cycle a,b,c    identical scenarios across runs: phase i targets the service in position i mod n,
                           on its busiest CPU at onset (lowest CPU on a tie), with no random draws; kinds and
                           levels still come from the --seed cycle. A victim that is not running (or uses
                           under --min-use cores) skips its phase but keeps the timing.
  --baseline SEC           after --warmup, stay off for SEC and log event=baseline state=on then state=off.
                           The load during that window is the clean measurement; the later off gaps are not.
  --sequential             walk --kinds in the order given and --levels low to high, without shuffling.
                           Phase i is pair i of that list, and the list repeats when --phases is longer.
  --pin-busiest            once, at the end of the baseline (or before the first phase), lock the victim to the
                           daemon using the most CPU and keep it for every phase. Its busiest CPU is still
                           chosen at each onset. --victims-cycle is ignored while a victim is pinned.
"""
import argparse, itertools, json, os, random, signal, subprocess, sys, time, urllib.request

CG = "/sys/fs/cgroup/mercury"


def stress_ng():
    """stress-ng built by cmake at benchmarks/stress-ng/stress-ng. STRESS_NG or DSB_ROOT selects another one."""
    env = os.environ.get("STRESS_NG")
    if env:
        return env
    root = os.environ.get("DSB_ROOT")
    if root:
        candidate = os.path.join(root, "stress-ng", "stress-ng")
        if os.access(candidate, os.X_OK):
            return candidate
    here = os.path.dirname(os.path.realpath(__file__))
    return os.path.abspath(os.path.join(here, "..", "..", "benchmarks", "stress-ng", "stress-ng"))


def cpu_list(text):
    out = []
    for part in text.strip().split(","):
        if part:
            a, _, b = part.partition("-")
            out += range(int(a), int(b or a) + 1)
    return out


def read(path):
    try:
        return open(path).read()
    except OSError:
        return ""


class Topology:
    def __init__(self):
        self.online = cpu_list(read("/sys/devices/system/cpu/online"))
        self.sibling = {c: cpu_list(read(f"/sys/devices/system/cpu/cpu{c}/topology/thread_siblings_list")) for c in self.online}
        self.l3 = {c: cpu_list(read(f"/sys/devices/system/cpu/cpu{c}/cache/index3/shared_cpu_list")) for c in self.online}
        self.node = {}
        for n in os.listdir("/sys/devices/system/node"):
            if n.startswith("node") and n[4:].isdigit():
                for c in cpu_list(read(f"/sys/devices/system/node/{n}/cpulist")):
                    self.node[c] = int(n[4:])

    def node_cpus(self, n):
        return [c for c in self.online if self.node.get(c) == n]


def cpusets():
    out = {}
    for d in os.listdir(CG) if os.path.isdir(CG) else []:
        cpus = cpu_list(read(f"{CG}/{d}/cpuset.cpus"))
        if cpus:
            out[d] = cpus
    return out


def usage(prefix, secs):
    """CPU use over secs: per daemon (cgroup cpu.stat, cores) and per CPU (/proc/stat busy share)."""
    def snap():
        d = {}
        for name in os.listdir(CG) if os.path.isdir(CG) else []:
            if name.startswith(prefix):
                for line in read(f"{CG}/{name}/cpu.stat").splitlines():
                    if line.startswith("usage_usec"):
                        d[name] = int(line.split()[1])
        c = {}
        for line in read("/proc/stat").splitlines():
            if line.startswith("cpu") and line[3:4].isdigit():
                f = line.split(); v = list(map(int, f[1:8]))
                c[int(f[0][3:])] = (v[0] + v[1] + v[2] + v[5] + v[6], sum(v))
        return d, c
    d0, c0 = snap(); time.sleep(secs); d1, c1 = snap()
    daemons = {k: (d1[k] - d0[k]) / 1e6 / secs for k in d1 if k in d0}
    cpus = {k: (c1[k][0] - c0[k][0]) / max(1, c1[k][1] - c0[k][1]) for k in c1 if k in c0}
    return daemons, cpus


def post(port, route, body):
    req = urllib.request.Request(f"http://127.0.0.1:{port}/{route}", data=json.dumps(body).encode(), method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read().decode()
    except Exception as e:  # a slow or failed call is logged, the run goes on
        return repr(e), ""


class Driver:
    def __init__(self, a):
        self.a = a
        self.topo = Topology()
        self.pool = cpu_list(a.pool)
        self.rng = random.Random(a.seed)
        self.log = open(os.path.join(a.run_dir, "antagonists.jsonl"), "a")
        self.procs = []
        self.restore = None  # (daemon, cpus) to put back at the end of the phase
        self.kinds = [k for k in a.kinds.split(",") if k]
        self.levels = tuple(a.levels)
        # Actions rotate strictly. The default schedule shuffles kind and level. --sequential keeps the
        # listed order so a sensitivity run can compare one kind at a time against an earlier baseline.
        self.actions = itertools.cycle(tuple(a.actions.split(",")))
        pairs = [(k, lv) for k in self.kinds for lv in self.levels]
        self.hits = itertools.cycle(pairs) if a.sequential else self._cycle(pairs)
        self.pinned = None

    def _cycle(self, items):
        while True:
            batch = list(items); self.rng.shuffle(batch)
            yield from batch

    def emit(self, **kw):
        kw = {"t_ns": time.monotonic_ns(), **kw}
        self.log.write(json.dumps(kw) + "\n"); self.log.flush()
        print(json.dumps(kw), flush=True)

    def daemons_on(self, sets):
        on = {}
        for cpus in sets.values():
            for c in cpus:
                on[c] = on.get(c, 0) + 1
        return on

    def targets(self, kind, level, c):
        t = self.topo
        if kind == "same":
            return [c]
        if kind == "smt":
            return [x for x in t.sibling[c] if x != c][:1]
        if kind == "l3":
            other = [x for x in t.l3[c] if x not in t.sibling[c]]
            return sorted(other)[:level]
        if kind == "membw":
            near = [x for x in t.node_cpus(t.node[c]) if x not in t.l3[c] and x in self.pool]
            return sorted(near)[:2 * level]
        raise ValueError(kind)

    def start(self, kind, level, cpus):
        for x in cpus:
            if kind in ("same", "smt"):
                args = ["--cpu", "1", "--cpu-load", "50" if level == 1 else "100"]
            elif kind == "l3":
                args = ["--cache", "1", "--cache-level", "3", "--cache-no-affinity"]
            else:
                args = ["--stream", "1"]
            binary = stress_ng()
            if not os.access(binary, os.X_OK):
                raise FileNotFoundError(f"stress-ng is not executable: {binary}")
            self.procs.append(subprocess.Popen(["taskset", "-c", str(x), binary, *args, "-t", "0", "-q"],
                                               start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))

    def stop(self):
        if os.path.exists(os.path.join(self.a.run_dir, "loadgen.scale")):
            self.scale(1.0)
        for p in self.procs:
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for p in self.procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
        self.procs = []

    def put_back(self):
        if self.restore:
            d, cpus = self.restore
            st, _ = post(self.a.port, "cpus", {"daemon_id": d, "cpus": ",".join(map(str, cpus))})
            self.emit(event="restore", daemon=d, cpus=cpus, status=st)
            self.restore = None

    def clean_cpu(self, avoid, sets, node):
        """Free pool CPU with the fewest daemons, in the victim's NUMA node when one is free there (leaving the node
        widens cpuset.mems; staying on it keeps the write to cpuset.cpus)."""
        on = self.daemons_on(sets)
        cand = [x for x in self.pool if x not in avoid]
        return min(cand, key=lambda x: (self.topo.node.get(x) != node, on.get(x, 0), x)) if cand else None

    def pin_busiest(self):
        """Lock every later phase to the daemon using the most CPU right now."""
        sets = cpusets()
        use, _ = usage(self.a.prefix, 2.0)
        names = [d for d in sets if d.startswith(self.a.prefix) and not any(x in d for x in ("loadgen", "jaeger"))]
        victims = [d for d in names if use.get(d, 0) >= self.a.min_use]
        if not victims:
            self.emit(event="pin", victim=None, reason=f"no daemon using >= {self.a.min_use} cores")
            return
        victim = max(victims, key=lambda d: (use[d], d))
        self.pinned = victim
        self.emit(event="pin", victim=victim, victim_use=round(use[victim], 3),
                  candidates={d: round(use[d], 3) for d in victims})

    def scale(self, factor):
        path = os.path.join(self.a.run_dir, "loadgen.scale")
        with open(path + ".tmp", "w") as f:
            f.write(f"{factor}\n")
        os.replace(path + ".tmp", path)

    def load_phase(self, i, level, action):
        factor = self.a.load_scale[level - 1]
        self.scale(factor)
        self.emit(event="on", phase=i, kind="load", level=level, victim=self.a.prefix + "loadgen", scale=factor,
                  planned_action=action)
        self.emit(event="action", phase=i, action="none", reason="load phase")
        time.sleep(self.a.on)
        self.scale(1.0)
        self.emit(event="off", phase=i)

    def phase(self, i):
        (kind, level), action = next(self.hits), next(self.actions)
        if kind == "load":
            return self.load_phase(i, level, action)
        sets = cpusets()
        use, busy = usage(self.a.prefix, 2.0)
        if self.pinned:
            victim = self.pinned
            if victim not in sets:
                self.emit(event="skip", phase=i, victim=victim, reason="pinned victim not running")
                time.sleep(self.a.on)
                return
            c = max(sets[victim], key=lambda x: (busy.get(x, 0), -x))
        elif self.a.victims_cycle:
            # Identical scenarios: a fixed victim per phase, on its busiest CPU, no random draws.
            cycle = self.a.victims_cycle.split(",")
            victim = self.a.prefix + cycle[i % len(cycle)]
            if victim not in sets or use.get(victim, 0) < self.a.min_use:
                self.emit(event="skip", phase=i, victim=victim, reason=f"not running or under {self.a.min_use} cores")
                time.sleep(self.a.on); return
            c = max(sets[victim], key=lambda x: (busy.get(x, 0), -x))
        else:
            if self.a.victims == "auto":
                names = [d for d in sets if d.startswith(self.a.prefix) and not any(x in d for x in ("loadgen", "jaeger"))]
            else:
                names = [self.a.prefix + v for v in self.a.victims.split(",")]
            victims = [d for d in names if d in sets and use.get(d, 0) >= self.a.min_use]
            if not victims:
                self.emit(event="skip", phase=i, reason=f"no victim using >= {self.a.min_use} cores"); time.sleep(self.a.on); return
            # Victim weighted by its CPU use, then one of its CPUs weighted by how busy that CPU is: the antagonist
            # lands where the victim actually runs.
            victim = self.rng.choices(victims, weights=[use[d] for d in victims])[0]
            c = self.rng.choices(sets[victim], weights=[busy.get(x, 0) + 0.01 for x in sets[victim]])[0]
        ant = self.targets(kind, level, c)
        if not ant:
            self.emit(event="skip", phase=i, reason=f"no {kind} target for cpu {c}"); return
        self.start(kind, level, ant)
        on = self.daemons_on(sets)
        self.emit(event="on", phase=i, kind=kind, level=level, victim=victim, victim_cpu=c, victim_cpus=sets[victim],
                  victim_use=round(use.get(victim, 0), 3), victim_cpu_busy=round(busy.get(c, 0), 3),
                  antagonist_cpus=ant, daemons_on_antagonist={str(x): on.get(x, 0) for x in ant}, planned_action=action)
        time.sleep(self.a.react)
        if action == "move_victim":
            hood = set(self.topo.l3[c]) | set(ant) | set(sets[victim])
            new = self.clean_cpu(hood, sets, self.topo.node[c])
            if new is None:
                self.emit(event="action", phase=i, action="none", reason="no clean cpu")
            else:
                cpus = sorted([x for x in sets[victim] if x != c] + [new])
                st, _ = post(self.a.port, "cpus", {"daemon_id": victim, "cpus": ",".join(map(str, cpus))})
                self.restore = (victim, sets[victim])  # also after a timeout: Mercury may still apply the change
                self.emit(event="action", phase=i, action=action, victim=victim, from_cpu=c, to_cpu=new, cpus=cpus, status=st)
        elif action == "move_antagonist":
            here = self.topo.node[c]
            nodes = sorted({self.topo.node[x] for x in self.pool if self.topo.node[x] != here},
                           key=lambda n: (sum(self.daemons_on(sets).get(x, 0) for x in self.topo.node_cpus(n)), n))
            far = [x for x in self.topo.node_cpus(nodes[0]) if x in self.pool][:len(ant)] if nodes else []
            self.stop()
            self.start(kind, level, far)
            self.emit(event="action", phase=i, action=action, from_cpus=ant, to_cpus=far)
        else:
            self.emit(event="action", phase=i, action="none")
        time.sleep(max(0.0, self.a.on - self.a.react))
        self.stop()
        self.emit(event="off", phase=i)
        self.put_back()

    def run(self):
        self.emit(event="start", args=vars(self.a), pool=self.pool, stress_ng=stress_ng())
        time.sleep(self.a.warmup)
        if self.a.baseline > 0:
            # The clean window. Off gaps between later phases are recovery, not this baseline.
            self.emit(event="baseline", state="on")
            time.sleep(self.a.baseline)
            if self.a.pin_busiest:
                self.pin_busiest()
            self.emit(event="baseline", state="off")
        elif self.a.pin_busiest:
            self.pin_busiest()
        for i in itertools.count() if self.a.phases == 0 else range(self.a.phases):
            time.sleep(self.a.off)
            try:
                self.phase(i)
            except Exception as e:  # one broken phase must not end the run
                self.stop(); self.put_back(); self.emit(event="error", phase=i, error=repr(e))
        self.emit(event="done")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--run-dir", required=True); ap.add_argument("--prefix", required=True)
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--victims", default="auto",
                    help="auto: every daemon of the app (not loadgen/jaeger) using >= --min-use cores, weighted by use; "
                         "or program names, comma separated: frontend,user,mongod")
    ap.add_argument("--min-use", type=float, default=0.05, help="cores a daemon must use to be a victim")
    ap.add_argument("--kinds", default="same,smt,l3,membw", help="add load for application-load phases")
    ap.add_argument("--load-scale", type=lambda v: [float(x) for x in v.split(",")], default=[1.5, 2.0],
                    help="load kind: the load generator's rate factor at level 1 and level 2")
    ap.add_argument("--levels", type=lambda v: [int(x) for x in v.split(",") if x], default=[1, 2],
                    help="intensities of each kind; the default schedule shuffles them, --sequential does not")
    ap.add_argument("--baseline", type=float, default=0,
                    help="seconds with no antagonist after warmup, logged as event=baseline")
    ap.add_argument("--sequential", action="store_true",
                    help="one phase per kind and level, in --kinds order, levels low to high, no shuffle")
    ap.add_argument("--pin-busiest", action="store_true",
                    help="after the baseline, pin every phase to the daemon that was using the most CPU")
    ap.add_argument("--pool", default="1-60")
    ap.add_argument("--warmup", type=float, default=30); ap.add_argument("--off", type=float, default=20)
    ap.add_argument("--on", type=float, default=30); ap.add_argument("--react", type=float, default=5)
    ap.add_argument("--phases", type=int, default=0, help="0: until SIGTERM")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--actions", default="none,move_victim,move_antagonist",
                    help="actions to rotate through after REACT seconds; none keeps the antagonists only")
    ap.add_argument("--victims-cycle", default="",
                    help="program names: phase i targets the one at i mod n (identical scenarios across runs)")
    a = ap.parse_args()
    d = Driver(a)

    def quit_(*_):
        d.stop(); d.put_back(); d.emit(event="stopped"); sys.exit(0)

    signal.signal(signal.SIGTERM, quit_)
    try:
        d.run()
    finally:
        d.stop(); d.put_back()


if __name__ == "__main__":
    main()
