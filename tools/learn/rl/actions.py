"""Action models that post every tool the teacher can post.

The continuous actors emit one number per process, which `decode_action` reads
as `hold` or `scale_daemon`. These models keep a process encoder (mlp, gnn, or
transformer) and add a head for each argument of a Mercury tool: the tool, the
service, the signed CPU count, the L3 domain (and a second domain), an `auto`
domain, and the route. `choose` turns the argmax into the arguments `execute`
already posts. An illegal combination becomes `hold`.

`inspect` is not a tool here. It reads state and does not change the machine.
"""

from __future__ import annotations

import math

import torch
import torch.nn as nn

from actors import TransformerBlock, apply_node_mask, attention_mask, sequence
from control import Decision, core_count, legal_delta
from teacher import FEAT_DIM, GLOBAL_DIM, encode_call, global_features, observe, processes

TOOLS = (
    "hold",
    "scale_daemon",
    "grow_pool",
    "shrink_pool",
    "swap_domain",
    "move_daemon",
    "add_replica",
    "remove_replica",
    "shed_route",
    "restore_route",
)
CPU_VALUES = tuple(i for i in range(-64, 65) if i != 0)
ROUTES = (
    "recommendations",
    "restaurants",
    "museums",
    "cinema",
    "review",
    "follow",
    "read-user-timeline",
)
DOMAIN_FEAT = 9
CONTEXT_DIM = GLOBAL_DIM + 3


def _f(value, default=0.0) -> float:
    if isinstance(value, bool) or value is None:
        return default
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def domain_features(domain: dict, free: set[str]) -> list[float]:
    cpus = domain.get("cpus") or []
    width = max(len(cpus), 1)
    name = str(domain.get("name") or "")
    return [
        1.0 if domain.get("in_pool") else 0.0,
        _f(domain.get("load")),
        len(domain.get("free") or []) / width,
        len(domain.get("daemons") or []) / 8.0,
        _f(domain.get("foreign_cpus")) / width,
        _f(domain.get("ipc")) / 3.0,
        math.log1p(max(_f(domain.get("llc_per_ms")), 0.0)) / 8.0,
        _f(domain.get("cpu_wait")),
        1.0 if name in free else 0.0,
    ]


def context_features(state: dict) -> list[float]:
    """The whole machine: `global_features`, then pool start/64, budget left/64, and log(p95 / healthy p95)/4."""
    pool = state.get("pool") or {}
    latency = state.get("latency") or {}
    p95 = _f((latency.get("now") or {}).get("p95_ms"))
    reference = _f(latency.get("reference_p95_ms"))
    ratio = max(min(math.log(p95 / reference) / 4.0, 2.0), -2.0) if p95 > 0 and reference > 0 else 0.0
    left = max(_f(pool.get("max")) - _f(pool.get("cores")), 0.0) if pool.get("max") else 0.0
    return global_features(state) + [_f(pool.get("start")) / 64.0, left / 64.0, ratio]


class Scene:
    """One machine, in the tensors an action model reads."""

    def __init__(self, state: dict, previous=None):
        ids, obs, history, topology, mask, global_state = observe(state, previous)
        self.ids = ids
        self.obs = obs
        self.history = history
        self.topology = topology
        self.mask = mask
        self.global_state = global_state
        self.context = torch.tensor(context_features(state), dtype=torch.float32)
        self.daemons = processes(state)
        domains = sorted(state.get("l3") or [], key=lambda row: str(row.get("name") or ""))
        free = {str(name) for name in ((state.get("pool") or {}).get("free_domains") or [])}
        self.domain_ids = [str(row.get("name") or "") for row in domains]
        if domains:
            self.domain_feat = torch.tensor([domain_features(row, free) for row in domains], dtype=torch.float32)
        else:
            self.domain_feat = torch.zeros(0, DOMAIN_FEAT)
        self.domain_in_pool = torch.tensor([bool(row.get("in_pool")) for row in domains], dtype=torch.bool)
        self.domain_free = torch.tensor([self.domain_ids[i] in free for i in range(len(domains))], dtype=torch.bool)
        degrade = state.get("degrade") or {}
        self.sheddable = [str(route) for route in (degrade.get("sheddable") or [])]
        self.shed = [str(route) for route in (degrade.get("shed") or [])]
        self.routes = list(ROUTES)
        pool = state.get("pool") or {}
        self.has_pool = bool(pool)
        self.pool_cores = int(_f(pool.get("cores")))
        self.pool_start = int(_f(pool.get("start")))
        self.pool_max = int(_f(pool.get("max")))
        self.domain_size = [len(row.get("cpus") or []) for row in domains]
        self.have = [core_count(daemon) or 0 for daemon in self.daemons]
        self.replica = [bool(daemon.get("replica")) for daemon in self.daemons]
        self.domain_growable = torch.tensor(
            [self.domain_free[i].item() and (not self.pool_max or self.pool_cores + self.domain_size[i] <= self.pool_max)
             for i in range(len(domains))], dtype=torch.bool)
        self.domain_shrinkable = torch.tensor(
            [self.domain_in_pool[i].item() and self.pool_cores - self.domain_size[i] >= self.pool_start
             for i in range(len(domains))], dtype=torch.bool)
        self.tool_legal = legal_tools(self)
        self.cpu_legal = legal_cpus(self)

    def headroom(self, index: int) -> int:
        """CPUs `add_cores` can still hand this process: the pool CPUs it does not hold."""
        limit = self.pool_cores if self.has_pool else 64
        return max(limit - self.have[index], 0)


DATA_STORES = ("mongo", "redis", "memcached", "rabbitmq", "jaeger", "loadgen")


def replicable(daemon_id: str) -> bool:
    """Mercury's `replica_denied` reads the exec path, which `/llm/state` does not carry; the id names the same
    data stores (hotel's memcached instances are named by port, `hotel-11211`)."""
    lowered = daemon_id.lower()
    if any(bit in lowered for bit in DATA_STORES):
        return False
    return not lowered.rsplit("-", 1)[-1].isdigit()


def move_targets(scene: Scene, index: int) -> list[int]:
    """Domains `move_daemon` may send this process to: in the pool (when there is one) and not already its own."""
    own = _daemon_domains(scene.daemons[index])
    return [slot for slot, ident in enumerate(scene.domain_ids)
            if ident not in own and (not scene.has_pool or bool(scene.domain_in_pool[slot]))]


def legal_tools(scene: Scene) -> torch.Tensor:
    """Tools Mercury would accept for at least one argument in this state."""
    count = len(scene.ids)
    scalable = any(scene.headroom(i) > 0 or scene.have[i] > 1 for i in range(count))
    movable = any(_daemon_domains(scene.daemons[i]) and move_targets(scene, i) for i in range(count))
    legal = {
        "hold": True,
        "scale_daemon": scalable,
        "grow_pool": bool(scene.domain_growable.any()) if scene.has_pool else False,
        "shrink_pool": bool(scene.domain_shrinkable.any()) if scene.has_pool else False,
        "swap_domain": bool(scene.domain_in_pool.any()) and bool(scene.domain_free.any()),
        "move_daemon": movable,
        "add_replica": any(not flag and replicable(ident) for flag, ident in zip(scene.replica, scene.ids)),
        "remove_replica": any(scene.replica),
        "shed_route": any(route in ROUTES and route not in scene.shed for route in scene.sheddable),
        "restore_route": any(route in ROUTES for route in scene.shed),
    }
    return torch.tensor([legal[name] for name in TOOLS], dtype=torch.bool)


def legal_cpus(scene: Scene) -> torch.Tensor:
    """[N, len(CPU_VALUES)]: a step up within the free pool CPUs, a step down that leaves one CPU."""
    values = torch.tensor(CPU_VALUES)
    rows = []
    for i in range(len(scene.ids)):
        up = scene.headroom(i)
        down = max(scene.have[i] - 1, 0)
        rows.append(((values > 0) & (values <= up)) | ((values < 0) & (-values <= down)))
    if not rows:
        return torch.zeros(0, len(CPU_VALUES), dtype=torch.bool)
    return torch.stack(rows, 0)


def would_refuse(scene: Scene, name: str, arguments: dict) -> bool:
    """True when Mercury would refuse this call in this state (`run_tool` and `/act` in `src/daemons.cpp`)."""
    if name == "hold":
        return False
    daemon_id = str(arguments.get("daemon_id") or "")
    index = scene.ids.index(daemon_id) if daemon_id in scene.ids else None
    if name == "scale_daemon":
        if index is None:
            return True
        cpus = int(arguments.get("cpus") or 0)
        return cpus == 0 or (cpus > 0 and scene.headroom(index) < 1) or (cpus < 0 and scene.have[index] <= 1)
    if name in ("add_replica", "remove_replica"):
        if name == "add_replica" and not replicable(daemon_id):
            return True
        return index is None or scene.replica[index] != (name == "remove_replica")
    l3 = arguments.get("l3")
    if name == "grow_pool":
        if l3 in (None, "", "auto"):
            return not bool(scene.domain_growable.any())
        return l3 not in scene.domain_ids or not bool(scene.domain_growable[scene.domain_ids.index(l3)])
    if name == "shrink_pool":
        if l3 in (None, "", "auto"):
            return not bool(scene.domain_shrinkable.any())
        return l3 not in scene.domain_ids or not bool(scene.domain_shrinkable[scene.domain_ids.index(l3)])
    if name == "swap_domain":
        source, dest = arguments.get("from_l3"), arguments.get("to_l3")
        if source not in scene.domain_ids or dest not in scene.domain_ids:
            return True
        return not (bool(scene.domain_in_pool[scene.domain_ids.index(source)])
                    and bool(scene.domain_free[scene.domain_ids.index(dest)]))
    if name == "move_daemon":
        if index is None:
            return True
        own = _daemon_domains(scene.daemons[index])
        dest = arguments.get("to_l3")
        return arguments.get("from_l3") not in own or dest not in scene.domain_ids \
            or scene.domain_ids.index(dest) not in move_targets(scene, index)
    if name == "shed_route":
        return arguments.get("route") not in scene.sheddable or arguments.get("route") in scene.shed
    if name == "restore_route":
        return arguments.get("route") not in scene.shed
    return True


def legal_logits(logits: dict, scene: Scene, keep: dict | None = None) -> dict:
    """Tool and CPU scores with Mercury's refusals at -inf. `keep` is a target that stays allowed."""
    out = dict(logits)
    tool_ok = scene.tool_legal.clone()
    cpu_ok = scene.cpu_legal.clone()
    if keep is not None:
        tool_ok[keep["tool"]] = True
        if keep.get("cpu", -1) >= 0 and keep.get("service", -1) >= 0 and cpu_ok.numel():
            cpu_ok[keep["service"], keep["cpu"]] = True
    out["tool"] = logits["tool"].masked_fill(~tool_ok.view(1, -1), float("-inf"))
    if cpu_ok.numel() and logits["cpu"].shape[1] == cpu_ok.shape[0]:
        out["cpu"] = logits["cpu"].masked_fill(~cpu_ok.unsqueeze(0), float("-inf"))
    return out


def _pick(scores: torch.Tensor) -> int | None:
    if scores.numel() == 0 or not bool(torch.isfinite(scores).any()):
        return None
    return int(scores.argmax())


def _mask_service(service: torch.Tensor, scene: Scene, tool: str) -> torch.Tensor:
    scores = service.clone()
    if scene.mask.numel() == scores.numel():
        scores = scores.masked_fill(~scene.mask.reshape(-1), float("-inf"))
    replica = torch.tensor([bool(daemon.get("replica")) for daemon in scene.daemons], dtype=torch.bool)
    if replica.numel() == scores.numel():
        if tool == "add_replica":
            denied = torch.tensor([not replicable(ident) for ident in scene.ids], dtype=torch.bool)
            scores = scores.masked_fill(replica | denied, float("-inf"))
        elif tool == "remove_replica":
            scores = scores.masked_fill(~replica, float("-inf"))
    if tool == "scale_daemon" and scene.cpu_legal.shape[0] == scores.numel():
        scores = scores.masked_fill(~scene.cpu_legal.any(dim=1), float("-inf"))
    if tool == "move_daemon" and len(scene.daemons) == scores.numel():
        stuck = torch.tensor([not (_daemon_domains(scene.daemons[i]) and move_targets(scene, i))
                              for i in range(len(scene.daemons))], dtype=torch.bool)
        scores = scores.masked_fill(stuck, float("-inf"))
    return scores


def _daemon_domains(daemon: dict) -> set[str]:
    return {str(name) for name in (daemon.get("l3") or [])}


def decode_choice(tool: torch.Tensor, service: torch.Tensor, cpu: torch.Tensor, domain: torch.Tensor,
                  domain_auto: torch.Tensor, domain_to: torch.Tensor, route: torch.Tensor, scene: Scene):
    """Argmax of one sample, as `(name, arguments, daemon_id, cpus)`."""
    name = TOOLS[int(tool.reshape(-1).argmax())]
    reason = {"reason": "action"}
    if name == "hold":
        return "hold", reason, None, 0
    if name in ("scale_daemon", "add_replica", "remove_replica", "move_daemon"):
        index = _pick(_mask_service(service.reshape(-1), scene, name))
        if index is None:
            return "hold", reason, None, 0
        daemon_id = scene.ids[index]
        daemon = scene.daemons[index]
        if name == "scale_daemon":
            count = CPU_VALUES[int(cpu.reshape(-1, len(CPU_VALUES))[index].argmax())]
            count = legal_delta(daemon, count)
            if count == 0:
                return "hold", reason, None, 0
            return "scale_daemon", {"daemon_id": daemon_id, "cpus": count, **reason}, daemon_id, count
        if name in ("add_replica", "remove_replica"):
            return name, {"daemon_id": daemon_id, **reason}, daemon_id, 0
        origin = _daemon_domains(daemon)
        targets_ok = set(move_targets(scene, index))
        source = domain.reshape(-1).clone()
        dest = domain_to.reshape(-1).clone()
        for slot, ident in enumerate(scene.domain_ids):
            if ident not in origin:
                source[slot] = float("-inf")
            if slot not in targets_ok:
                dest[slot] = float("-inf")
        from_i, to_i = _pick(source), _pick(dest)
        if from_i is None or to_i is None:
            return "hold", reason, None, 0
        return "move_daemon", {
            "daemon_id": daemon_id, "from_l3": scene.domain_ids[from_i], "to_l3": scene.domain_ids[to_i], **reason,
        }, daemon_id, 0
    if name in ("grow_pool", "shrink_pool"):
        named = domain.reshape(-1).clone()
        allow = scene.domain_growable if name == "grow_pool" else scene.domain_shrinkable
        if allow.numel() == named.numel():
            named = named.masked_fill(~allow, float("-inf"))
        auto = float(domain_auto.reshape(-1)[0])
        best = _pick(named)
        if best is None or (not math_gt(named[best], auto)):
            return name, {"l3": "auto", **reason}, None, 0
        return name, {"l3": scene.domain_ids[best], **reason}, None, 0
    if name == "swap_domain":
        source = domain.reshape(-1).clone()
        dest = domain_to.reshape(-1).clone()
        if scene.domain_in_pool.numel() == source.numel():
            source = source.masked_fill(~scene.domain_in_pool, float("-inf"))
            dest = dest.masked_fill(~scene.domain_free, float("-inf"))
        from_i, to_i = _pick(source), _pick(dest)
        if from_i is None or to_i is None or from_i == to_i:
            return "hold", reason, None, 0
        return "swap_domain", {"from_l3": scene.domain_ids[from_i], "to_l3": scene.domain_ids[to_i], **reason}, None, 0
    if name in ("shed_route", "restore_route"):
        scores = route.reshape(-1).clone()
        allowed = scene.sheddable if name == "shed_route" else scene.shed
        for slot, ident in enumerate(scene.routes):
            if ident not in allowed or (name == "shed_route" and ident in scene.shed):
                scores[slot] = float("-inf")
        index = _pick(scores)
        if index is None:
            return "hold", reason, None, 0
        return name, {"route": scene.routes[index], **reason}, None, 0
    return "hold", reason, None, 0


def math_gt(left, right) -> bool:
    return float(left) > float(right)


def targets(scene: Scene, call: dict | None) -> dict | None:
    """Class indices for one accepted call. `None` means the call cannot be a label.

    `domain` equal to the number of domains is the `auto` slot. A head that this
    tool does not use stays -1.
    """
    if not scene.ids:
        return None
    if call is None or call.get("name") in (None, "", "none", "hold"):
        return {"tool": TOOLS.index("hold"), "service": -1, "cpu": -1, "domain": -1, "domain_to": -1, "route": -1}
    if not call.get("ok", True):
        return None
    name = str(call.get("name") or "")
    if name not in TOOLS:
        return None
    raw = dict(call.get("input") or {})
    out = {"tool": TOOLS.index(name), "service": -1, "cpu": -1, "domain": -1, "domain_to": -1, "route": -1}

    def service_index(ident: str, replica: bool | None = None) -> int | None:
        if ident not in scene.ids:
            return None
        index = scene.ids.index(ident)
        is_replica = bool(scene.daemons[index].get("replica"))
        if replica is True and not is_replica:
            return None
        if replica is False and is_replica:
            return None
        return index

    if name == "scale_daemon":
        try:
            cpus = int(raw.get("cpus"))
        except (TypeError, ValueError):
            return None
        index = service_index(str(raw.get("daemon_id") or ""))
        if index is None or cpus not in CPU_VALUES:
            return None
        out["service"] = index
        out["cpu"] = CPU_VALUES.index(cpus)
        return out
    if name == "add_replica":
        index = service_index(str(raw.get("daemon_id") or ""), replica=False)
        if index is None:
            return None
        out["service"] = index
        return out
    if name == "remove_replica":
        index = service_index(str(raw.get("daemon_id") or ""), replica=True)
        if index is None:
            return None
        out["service"] = index
        return out
    if name in ("grow_pool", "shrink_pool"):
        l3 = raw.get("l3")
        if l3 in (None, "", "auto"):
            out["domain"] = len(scene.domain_ids)
            return out
        if str(l3) not in scene.domain_ids:
            return None
        out["domain"] = scene.domain_ids.index(str(l3))
        return out
    if name == "swap_domain":
        source, dest = str(raw.get("from_l3") or ""), str(raw.get("to_l3") or "")
        if source not in scene.domain_ids or dest not in scene.domain_ids or source == dest:
            return None
        out["domain"] = scene.domain_ids.index(source)
        out["domain_to"] = scene.domain_ids.index(dest)
        return out
    if name == "move_daemon":
        index = service_index(str(raw.get("daemon_id") or ""))
        source, dest = str(raw.get("from_l3") or ""), str(raw.get("to_l3") or "")
        if index is None or source not in scene.domain_ids or dest not in scene.domain_ids:
            return None
        out["service"] = index
        out["domain"] = scene.domain_ids.index(source)
        out["domain_to"] = scene.domain_ids.index(dest)
        return out
    if name in ("shed_route", "restore_route"):
        route = str(raw.get("route") or "")
        if route not in scene.routes:
            return None
        out["route"] = scene.routes.index(route)
        return out
    return None


def action_loss(logits: dict, target: dict, scene: Scene | None = None) -> torch.Tensor:
    """Cross-entropy on the tool and on each argument the tool uses.

    With `scene`, the tools and CPU steps Mercury would refuse are masked, except
    the target itself. `target["weight"]` scales the row. `target["avoid"]` is a
    call to move away from (refused, or followed by a worse outcome): the loss is
    `-log(1 - p(call))`, where `p(call)` is the joint probability of the tool and
    every argument it used.
    """
    avoid = bool(target.get("avoid"))
    if scene is not None:
        logits = legal_logits(logits, scene, None if avoid else target)
    weight = float(target.get("weight", 1.0))
    log_p = call_log_prob(logits, target)
    if avoid:
        if not bool(torch.isfinite(log_p)):
            return logits["tool"].new_zeros(())
        return -torch.log((1.0 - log_p.exp()).clamp(min=1e-6)) * weight
    return -log_p * weight


def call_log_prob(logits: dict, target: dict) -> torch.Tensor:
    """log p(tool) plus log p of each argument the tool uses."""

    def pick(row: torch.Tensor, index: int) -> torch.Tensor:
        return torch.log_softmax(row, dim=-1)[index]

    log_p = pick(logits["tool"][0], target["tool"])
    if target["service"] >= 0:
        log_p = log_p + pick(logits["service"][0], target["service"])
    if target["cpu"] >= 0:
        log_p = log_p + pick(logits["cpu"][0, target["service"]], target["cpu"])
    if target["domain"] >= 0:
        log_p = log_p + pick(torch.cat([logits["domain"][0], logits["domain_auto"][0].reshape(1)]), target["domain"])
    if target["domain_to"] >= 0:
        log_p = log_p + pick(logits["domain_to"][0], target["domain_to"])
    if target["route"] >= 0:
        log_p = log_p + pick(logits["route"][0], target["route"])
    return log_p


class ActionHeads(nn.Module):
    """One score for the tool and one score for each of its arguments."""

    def __init__(self, hidden: int):
        super().__init__()
        self.tool = nn.Linear(hidden, len(TOOLS))
        self.service = nn.Linear(hidden, 1)
        self.cpus = nn.Linear(hidden, len(CPU_VALUES))
        self.domain = nn.Linear(hidden, 1)
        self.domain_to = nn.Linear(hidden, 1)
        self.auto = nn.Linear(hidden, 1)
        self.route = nn.Linear(hidden, len(ROUTES))

    def forward(self, process, domain, process_mask, summary=None):
        """`summary` [B, hidden] feeds the tool, auto, and route heads; without it, the mean process row."""
        if summary is None:
            present = process_mask.to(dtype=process.dtype).unsqueeze(-1)
            summary = (process * present).sum(1) / present.sum(1).clamp(min=1.0)
        pooled = summary
        service = self.service(process).squeeze(-1)
        service = service.masked_fill(~process_mask, float("-inf"))
        return {
            "tool": self.tool(pooled),
            "service": service,
            "cpu": self.cpus(process),
            "domain": self.domain(domain).squeeze(-1),
            "domain_to": self.domain_to(domain).squeeze(-1),
            "domain_auto": self.auto(pooled),
            "route": self.route(pooled),
        }


class ActionModel(nn.Module):
    """Process encoder plus `ActionHeads`. `actor_type` is mlp, gnn, or transformer.

    With `context`, the tool, auto, and route heads read the mean process row,
    the mean domain row, and `context_features` (the pool and latency) together.
    """

    def __init__(self, actor_type: str, feat_dim: int = FEAT_DIM, hidden: int = 32, taps: int = 2,
                 layers: int = 2, heads: int = 4, max_age: int = 64, context: bool = True):
        super().__init__()
        if actor_type not in ("mlp", "gnn", "transformer"):
            raise ValueError(f"actor_type must be mlp, gnn, or transformer, got {actor_type!r}")
        self.actor_type = actor_type
        self.feat_dim = feat_dim
        self.hidden = hidden
        self.taps = taps
        self.layers = layers
        self.heads = heads
        self.max_age = max_age
        self.action_dim = 1
        if actor_type == "mlp":
            self.net = nn.Sequential(nn.Linear(feat_dim, hidden), nn.ReLU(), nn.Linear(hidden, hidden), nn.ReLU())
        elif actor_type == "gnn":
            from train_gnn import DelayedGraphFilter
            self.g1 = DelayedGraphFilter(feat_dim, hidden, taps)
            self.g2 = DelayedGraphFilter(hidden, hidden, taps)
        else:
            if hidden % heads != 0:
                raise ValueError(f"width {hidden} must divide into {heads} heads")
            self.inp = nn.Linear(feat_dim, hidden)
            self.age = nn.Embedding(max_age, hidden)
            self.blocks = nn.ModuleList(TransformerBlock(hidden, heads) for _ in range(layers))
            self.ln = nn.LayerNorm(hidden)
        self.domain_enc = nn.Sequential(nn.Linear(DOMAIN_FEAT, hidden), nn.ReLU(), nn.Linear(hidden, hidden))
        self.heads_out = ActionHeads(hidden)
        self.context = context
        if context:
            self.context_enc = nn.Sequential(nn.Linear(CONTEXT_DIM, hidden), nn.ReLU())
            self.summary = nn.Sequential(nn.Linear(3 * hidden, hidden), nn.ReLU())

    def encode_process(self, obs, history, topology, node_mask):
        if self.actor_type == "mlp":
            return self.net(obs)
        if self.actor_type == "gnn":
            obs, history, topology = apply_node_mask(obs, history, topology, node_mask)
            seq = sequence(obs, history)
            batch, ticks, nodes, _ = seq.shape
            rows = []
            for row in range(batch):
                shift = topology[row].unsqueeze(0).expand(ticks, nodes, nodes)
                hidden = torch.relu(self.g1(seq[row], shift))
                hidden = torch.relu(self.g2(hidden, shift))
                rows.append(hidden[-1])
            return torch.stack(rows, 0)
        obs, history, topology = apply_node_mask(obs, history, topology, node_mask)
        frames = sequence(obs, history)
        batch, ticks, nodes, _ = frames.shape
        ages = torch.arange(ticks - 1, -1, -1, device=obs.device)
        local = frames.transpose(1, 2)
        tokens = self.inp(local) + self.age(ages).view(1, 1, ticks, -1)
        raw = tokens.reshape(batch, nodes * ticks, -1)
        mask = attention_mask(topology, node_mask, ticks - 1)
        flat = raw
        for block in self.blocks:
            flat = block(flat, raw, mask)
        flat = self.ln(flat)
        current_index = torch.arange(nodes, device=obs.device) * ticks + (ticks - 1)
        return flat.index_select(1, current_index)

    def forward(self, obs, history, topology, node_mask, domain_feat, context=None):
        if obs.dim() == 2:
            obs = obs.unsqueeze(0)
            history = history.unsqueeze(0)
            topology = topology.unsqueeze(0)
            node_mask = node_mask.unsqueeze(0)
            domain_feat = domain_feat.unsqueeze(0)
            if context is not None:
                context = context.unsqueeze(0)
        process = self.encode_process(obs, history, topology, node_mask)
        domain = self.domain_enc(domain_feat)
        if not self.context:
            return self.heads_out(process, domain, node_mask)
        if context is None:
            raise ValueError("this action model reads the machine context: pass Scene.context (scene_logits)")
        present = node_mask.to(dtype=process.dtype).unsqueeze(-1)
        pooled = (process * present).sum(1) / present.sum(1).clamp(min=1.0)
        domains = domain.mean(1) if domain.shape[1] else domain.new_zeros(domain.shape[0], domain.shape[-1])
        summary = self.summary(torch.cat([pooled, domains, self.context_enc(context)], dim=-1))
        return self.heads_out(process, domain, node_mask, summary)


def scene_logits(model: ActionModel, scene: Scene) -> dict:
    return model(scene.obs, scene.history, scene.topology, scene.mask, scene.domain_feat, scene.context)


def build_action_model(actor_type: str, hidden: int = 32, taps: int = 2, layers: int = 2, heads: int = 4,
                       context: bool = True) -> ActionModel:
    return ActionModel(actor_type, hidden=hidden, taps=taps, layers=layers, heads=heads, context=context)


POOL_TOOLS = ("grow_pool", "shrink_pool", "swap_domain")


def refusal_rate(model: ActionModel, scenes: list[Scene], masked: bool = True) -> dict:
    """Decode on each logged scene and count the calls Mercury would refuse, by tool."""
    tools: dict[str, list[int]] = {}
    was = model.training
    model.eval()
    with torch.no_grad():
        for scene in scenes:
            if not scene.ids:
                continue
            logits = scene_logits(model, scene)
            if masked:
                logits = legal_logits(logits, scene)
            name, arguments, _daemon, _cpus = decode_choice(
                logits["tool"][0], logits["service"][0], logits["cpu"][0], logits["domain"][0],
                logits["domain_auto"][0], logits["domain_to"][0], logits["route"][0], scene,
            )
            bucket = tools.setdefault(name, [0, 0])
            bucket[0] += 1
            bucket[1] += int(would_refuse(scene, name, arguments))
    model.train(was)
    total = sum(item[0] for item in tools.values())
    refused = sum(item[1] for item in tools.values())
    return {"n": total, "refused": refused, "rate": refused / max(total, 1),
            "by_tool": {name: {"n": item[0], "refused": item[1]} for name, item in sorted(tools.items())}}


def choose(model: ActionModel, state: dict, previous=None, used: dict | None = None):
    """One period. The returned call is one `execute` posts, or `hold`.

    Calls Mercury would refuse are masked. `used` holds the `services` and pool
    `tools` already acted on this episode; those are masked too.
    """
    scene = Scene(state, previous)
    carry = (scene.ids, scene.obs)
    empty = torch.zeros(len(scene.ids), 1)
    if not scene.ids:
        decision = Decision(
            "hold", {"reason": "action"}, empty, empty, [], scene.obs, scene.history, scene.topology,
            scene.mask, scene.global_state, None, 0,
        )
        return decision, carry
    was = model.training
    model.eval()
    try:
        with torch.no_grad():
            logits = scene_logits(model, scene)
    finally:
        model.train(was)
    logits = legal_logits(logits, scene)
    if used:
        tool = logits["tool"].clone()
        for name in used.get("tools", ()):
            if name in POOL_TOOLS:
                tool[0, TOOLS.index(name)] = float("-inf")
        service = logits["service"].clone()
        for ident in used.get("services", ()):
            if ident in scene.ids:
                service[0, scene.ids.index(ident)] = float("-inf")
        logits = {**logits, "tool": tool, "service": service}
    name, arguments, daemon_id, cpus = decode_choice(
        logits["tool"][0], logits["service"][0], logits["cpu"][0], logits["domain"][0],
        logits["domain_auto"][0], logits["domain_to"][0], logits["route"][0], scene,
    )
    if name == "scale_daemon":
        executed = encode_call(scene.ids, {"name": name, "input": arguments, "ok": True})["continuous"]
    else:
        executed = empty
    decision = Decision(
        name, arguments, logits["tool"][0].detach(), executed, scene.ids, scene.obs, scene.history,
        scene.topology, scene.mask, scene.global_state, daemon_id, cpus,
    )
    return decision, carry


def save_action_model(model: ActionModel, path: str, setting: str | None = None) -> None:
    torch.save({
        "kind": "action",
        "setting": setting,
        "actor_type": model.actor_type,
        "model": model.state_dict(),
        "hidden": model.hidden,
        "taps": model.taps,
        "layers": model.layers,
        "heads": model.heads,
        "feat_dim": model.feat_dim,
        "context": model.context,
    }, path)


def load_action_model(path: str, blob: dict | None = None) -> ActionModel:
    if blob is None:
        blob = torch.load(path, map_location="cpu", weights_only=False)
    if blob.get("kind") != "action":
        raise ValueError(f"{path} is not an action-model checkpoint")
    model = build_action_model(
        blob["actor_type"], int(blob["hidden"]), int(blob["taps"]), int(blob["layers"]), int(blob["heads"]),
        bool(blob.get("context", False)),
    )
    model.load_state_dict(blob["model"])
    model.eval()
    return model
