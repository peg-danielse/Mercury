"""One replay schema for every actor.

A transition is a joint step: every present process's proposal, the action
that was actually applied, the scalar reward, and the next mask and topology.
Process count may change between the two states. Sampling pads the shorter
rows; the pad is a False mask and zeros, never a fabricated process.
"""

from __future__ import annotations

import torch

KEYS = (
    "obs",
    "history",
    "topology",
    "node_mask",
    "action",
    "executed",
    "reward",
    "done",
    "next_obs",
    "next_history",
    "next_topology",
    "next_mask",
    "global_state",
    "next_global",
)


def _as_tensor(value, dtype):
    if isinstance(value, torch.Tensor):
        return value.detach().to(dtype=dtype).clone()
    return torch.tensor(value, dtype=dtype)


def _check(fields: dict) -> tuple[int, int, int, int]:
    missing = [key for key in KEYS if key not in fields]
    if missing:
        raise KeyError(f"transition is missing {missing}")
    obs = fields["obs"]
    if obs.dim() != 2:
        raise ValueError("obs must be [N, F]")
    nodes, feat = obs.shape
    history = fields["history"]
    if history.dim() != 3 or history.shape[1] != nodes or history.shape[2] != feat:
        raise ValueError("history must be [T, N, F]")
    ticks = history.shape[0]
    if fields["topology"].shape != (nodes, nodes):
        raise ValueError("topology must be [N, N]")
    if fields["node_mask"].shape != (nodes,) or fields["node_mask"].dtype != torch.bool:
        raise ValueError("node_mask must be [N] bool")
    action = fields["action"]
    if action.dim() != 2 or action.shape[0] != nodes:
        raise ValueError("action must be [N, A]")
    action_dim = action.shape[1]
    if fields["executed"].shape != (nodes, action_dim):
        raise ValueError("executed must be [N, A]")
    nxt = fields["next_obs"]
    if nxt.dim() != 2 or nxt.shape[1] != feat:
        raise ValueError("next_obs must be [N', F]")
    next_nodes = nxt.shape[0]
    if fields["next_history"].shape != (ticks, next_nodes, feat):
        raise ValueError("next_history must be [T, N', F] with the same T and F")
    if fields["next_topology"].shape != (next_nodes, next_nodes):
        raise ValueError("next_topology must be [N', N']")
    if fields["next_mask"].shape != (next_nodes,) or fields["next_mask"].dtype != torch.bool:
        raise ValueError("next_mask must be [N'] bool")
    if fields["global_state"].dim() != 1 or fields["next_global"].shape != fields["global_state"].shape:
        raise ValueError("global_state and next_global must be the same 1-d shape")
    return ticks, feat, action_dim, fields["global_state"].shape[0]


def _pad_nodes(tensor, nodes, node_dim):
    if tensor.shape[node_dim] == nodes:
        return tensor
    extra = nodes - tensor.shape[node_dim]
    if extra < 0:
        raise ValueError("pad target is smaller than the tensor")
    pad_shape = list(tensor.shape)
    pad_shape[node_dim] = extra
    filler = tensor.new_zeros(pad_shape)
    return torch.cat([tensor, filler], dim=node_dim)


def _pad_square(tensor, nodes):
    if tensor.shape == (nodes, nodes):
        return tensor
    out = tensor.new_zeros(nodes, nodes)
    n = tensor.shape[0]
    out[:n, :n] = tensor
    return out


class Replay:
    def __init__(self, capacity: int):
        if capacity < 1:
            raise ValueError("capacity must be positive")
        self.capacity = capacity
        self.items: list[dict] = []
        self.pos = 0
        self.spec = None

    def __len__(self) -> int:
        return len(self.items)

    def add(self, **fields):
        ticks, feat, action_dim, global_dim = _check(fields)
        spec = (ticks, feat, action_dim, global_dim)
        if self.spec is None:
            self.spec = spec
        elif spec != self.spec:
            raise ValueError(f"transition spec {spec} does not match replay {self.spec}")
        stored = {
            "obs": fields["obs"].detach().clone(),
            "history": fields["history"].detach().clone(),
            "topology": fields["topology"].detach().clone(),
            "node_mask": fields["node_mask"].detach().clone(),
            "action": fields["action"].detach().clone(),
            "executed": fields["executed"].detach().clone(),
            "reward": _as_tensor(fields["reward"], torch.float32).reshape(()),
            "done": _as_tensor(bool(fields["done"]), torch.float32).reshape(()),
            "next_obs": fields["next_obs"].detach().clone(),
            "next_history": fields["next_history"].detach().clone(),
            "next_topology": fields["next_topology"].detach().clone(),
            "next_mask": fields["next_mask"].detach().clone(),
            "global_state": fields["global_state"].detach().clone(),
            "next_global": fields["next_global"].detach().clone(),
        }
        if len(self.items) < self.capacity:
            self.items.append(stored)
        else:
            self.items[self.pos] = stored
        self.pos = (self.pos + 1) % self.capacity

    def sample(self, batch_size: int, generator: torch.Generator | None = None) -> dict:
        if batch_size < 1 or batch_size > len(self.items):
            raise ValueError(f"batch size {batch_size} is outside 1..{len(self.items)}")
        if generator is None:
            order = torch.randperm(len(self.items))
        else:
            order = torch.randperm(len(self.items), generator=generator)
        chosen = [self.items[int(i)] for i in order[:batch_size]]
        nodes = max(item["obs"].shape[0] for item in chosen)
        next_nodes = max(item["next_obs"].shape[0] for item in chosen)

        def stack_nodes(key, width, node_dim):
            return torch.stack([_pad_nodes(item[key], width, node_dim) for item in chosen])

        def stack_mask(key, width):
            return torch.stack([
                torch.nn.functional.pad(item[key], (0, width - item[key].shape[0]))
                for item in chosen
            ])

        return {
            "obs": stack_nodes("obs", nodes, 0),
            "history": stack_nodes("history", nodes, 1),
            "topology": torch.stack([_pad_square(item["topology"], nodes) for item in chosen]),
            "node_mask": stack_mask("node_mask", nodes),
            "action": stack_nodes("action", nodes, 0),
            "executed": stack_nodes("executed", nodes, 0),
            "reward": torch.stack([item["reward"] for item in chosen]),
            "done": torch.stack([item["done"] for item in chosen]),
            "next_obs": stack_nodes("next_obs", next_nodes, 0),
            "next_history": stack_nodes("next_history", next_nodes, 1),
            "next_topology": torch.stack([_pad_square(item["next_topology"], next_nodes) for item in chosen]),
            "next_mask": stack_mask("next_mask", next_nodes),
            "global_state": torch.stack([item["global_state"] for item in chosen]),
            "next_global": torch.stack([item["next_global"] for item in chosen]),
        }
