# Example zeroctl prompt (Mercury)

Rendered from `example-state.json` with `python3 scripts/zeroctl/zeroctl.py --print-prompt --state scripts/zeroctl/example-state.json`. node.0 is saturated and holds both services; node.1 is nearly idle. An earlier move failed and is in History.

```text
You operate a Linux server that runs one application as native daemons under Mercury,
a daemon keeper. Each daemon runs in a cgroup whose cpuset decides which Linux CPUs it
may use. The application is degraded end to end: latency and errors at its entry points
are above the healthy reference. The cause may be contention on a CPU group or a load
the daemon's current CPU share cannot carry.

You are one step in a control loop. Emit one action now; it is applied, and you are
asked again with the updated server and the History below, which lists your earlier
steps. A plan may take several steps. Emit stop only when the application needs no
further change.

## Situation
application: mercury daemons svc0, svc1
now: no latency measurement supplied

## CPU groups
Groups nest socket > node > l3 > core. Each line: group, level, load over the last five
1 s intervals (oldest first, busy share of the group's CPUs), allocatable CPUs, and the
daemons whose cpuset touches it.
socket.0  socket  load 50% 50% 50% 50% 50%  allocatable 7  cpus 0-7  daemons: svc0, svc1
node.0  node  load 90% 95% 97% 99% 100%  allocatable 3  cpus 0-3  daemons: svc0, svc1
node.1  node  load 10% 10% -- 10% 12%  allocatable 4  cpus 4-7  daemons: -

## Daemons
Each line: daemon, Linux CPUs in its cpuset, the node groups it sits on, its CPU
scaling limits, and its optional components with their cost and state.
svc0  cpus 1 (1)  on node.0  scale off  extra on cost 0.3
svc1  cpus 2 (1)  on node.0  scale off  no optional components

## Recent policy events
tick 4: move svc1 from node.1 to node.0

## History
t+20s  move svc1 node.0  FAILED: svc1 already runs on node.0

## Rules
- Daemons you may change: svc0, svc1.
- Optional components: extra. Everything else in a program is critical.
- Never stop or restart a daemon; Mercury keeps the pid.
- A move targets a node group the daemon is not on, whose latest load is under 100%
  and whose allocatable CPUs cover the daemon's current CPU count.
- Never use CPU 0; it is Mercury's own.
- scale_out sets the daemon's CPU count between 1 and 7.
- degrade names an enabled optional component; restore names a disabled one.
- Do not repeat an action marked FAILED in History.
- none and stop both mean the application needs no further change; a busy server is not that.

## Actions
move rewrites the daemon's cpuset onto another CPU group, away from what it shares a saturated group with. Same CPU count, same pid.
scale_out sets the daemon's CPU count (min = max = cpus). Extra CPUs come from the least-loaded cores.
degrade disables one optional component of a daemon; the program sheds that work while it is off.
restore re-enables a disabled optional component once there is headroom.
none when the server needs no change now. A busy group is not a reason to stop.
stop the application needs no further change.

## Output
One YAML document, the action for this step, and nothing else.
action: move | scale_out | degrade | restore | none | stop
workload: <daemon id>               # every action except none and stop
node: <node group>          # move
cpus: <1..7>               # scale_out
component: <name>                   # degrade, restore
reason: <one sentence>

```
