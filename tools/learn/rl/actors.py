"""Interchangeable decentralized actors for one centralized critic.

Every actor maps the same tensors to a continuous joint action:

    obs        [B, N, F]      local features at the decision
    history    [B, T, N, F]   strictly older local features, index 0 oldest
    topology   [B, N, N]      shift operator, one row per process
    node_mask  [B, N]         True where a process is present
    -> action  [B, N, A]      in [-1, 1]

`actor_type` is `mlp`, `gnn`, or `transformer`. The training step calls
`build_actor` and `forward`. It does not branch on the class.

The GNN actor is an adapter around `LocalGNNDB` in `tools/learn/train_gnn.py`.
`demand` is that module's forward, unchanged. The policy head sits outside it:
the last demand is a feature of a linear layer, then tanh. Nothing in this
file rounds or thresholds the action.
"""

from __future__ import annotations

import os
import sys
from dataclasses import dataclass

import torch
import torch.nn as nn

_LEARN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
if _LEARN not in sys.path:
    sys.path.insert(0, _LEARN)

from train_gnn import LocalGNNDB  # noqa: E402


def check_actor_inputs(obs, history, topology, node_mask):
    """Raise ValueError when the four actor tensors disagree."""
    if obs.dim() != 3:
        raise ValueError(f"obs must be [B, N, F], got {tuple(obs.shape)}")
    batch, nodes, feat = obs.shape
    if history.dim() != 4 or history.shape[0] != batch or history.shape[2] != nodes or history.shape[3] != feat:
        raise ValueError(
            f"history must be [B, T, N, F] = [{batch}, T, {nodes}, {feat}], got {tuple(history.shape)}"
        )
    if topology.shape != (batch, nodes, nodes):
        raise ValueError(f"topology must be [{batch}, {nodes}, {nodes}], got {tuple(topology.shape)}")
    if node_mask.shape != (batch, nodes):
        raise ValueError(f"node_mask must be [{batch}, {nodes}], got {tuple(node_mask.shape)}")
    if node_mask.dtype != torch.bool:
        raise ValueError("node_mask must be bool")
    return batch, nodes, feat


def sequence(obs, history):
    """Stack history under the current observation. Time index 0 is oldest."""
    if history.shape[1] == 0:
        return obs.unsqueeze(1)
    return torch.cat([history, obs.unsqueeze(1)], dim=1)


def apply_node_mask(obs, history, topology, node_mask):
    """Drop features and edges of absent processes. Present rows are unchanged."""
    present = node_mask.to(dtype=obs.dtype)
    obs = obs * present.unsqueeze(-1)
    history = history * present.view(present.shape[0], 1, present.shape[1], 1)
    pair = present.unsqueeze(-1) * present.unsqueeze(-2)
    topology = topology * pair
    return obs, history, topology


@dataclass
class ActorConfig:
    """Selects an actor. Training and evaluation read this and nothing else."""

    actor_type: str
    feat_dim: int
    action_dim: int
    hidden: int = 32
    taps: int = 3
    layers: int = 2
    heads: int = 4
    max_age: int = 64


class MlpActor(nn.Module):
    """Shared memoryless policy. Each process sees only its own current features."""

    def __init__(self, feat_dim: int, action_dim: int, hidden: int = 32):
        super().__init__()
        self.feat_dim = feat_dim
        self.action_dim = action_dim
        self.net = nn.Sequential(
            nn.Linear(feat_dim, hidden),
            nn.ReLU(),
            nn.Linear(hidden, hidden),
            nn.ReLU(),
            nn.Linear(hidden, action_dim),
        )

    def forward(self, obs, history, topology, node_mask):
        check_actor_inputs(obs, history, topology, node_mask)
        if obs.shape[-1] != self.feat_dim:
            raise ValueError(f"obs features {obs.shape[-1]} != {self.feat_dim}")
        present = node_mask.to(dtype=obs.dtype).unsqueeze(-1)
        local = obs * present
        return torch.tanh(self.net(local)) * present


class GnnActor(nn.Module):
    """`LocalGNNDB` with a continuous action head.

    `demand(x, S)` returns `LocalGNNDB.forward(x, S)` for the tensors the
    existing trainer already uses: x [T, N, F], S [T, N, N]. `forward` builds
    those tensors from the shared interface, one batch row at a time, calls
    `demand`, and maps the last tick through a linear layer and tanh.
    """

    def __init__(self, feat_dim: int, action_dim: int, hidden: int = 16, taps: int = 3):
        super().__init__()
        self.feat_dim = feat_dim
        self.action_dim = action_dim
        self.taps = taps
        self.backbone = LocalGNNDB(feat_dim, hidden, taps)
        self.head = nn.Linear(1, action_dim)

    def demand(self, x, shift):
        """Exact `LocalGNNDB` forward. x [T, N, F], shift [T, N, N] -> [T, N]."""
        return self.backbone(x, shift)

    def forward(self, obs, history, topology, node_mask):
        check_actor_inputs(obs, history, topology, node_mask)
        if obs.shape[-1] != self.feat_dim:
            raise ValueError(f"obs features {obs.shape[-1]} != {self.feat_dim}")
        obs, history, topology = apply_node_mask(obs, history, topology, node_mask)
        seq = sequence(obs, history)
        batch, ticks, nodes, _ = seq.shape
        last = []
        for row in range(batch):
            features = seq[row]
            shift = topology[row].unsqueeze(0).expand(ticks, nodes, nodes)
            demand = self.demand(features, shift)
            last.append(demand[-1])
        latest = torch.stack(last, 0).unsqueeze(-1)
        present = node_mask.to(dtype=latest.dtype).unsqueeze(-1)
        return torch.tanh(self.head(latest)) * present


# Larger than any history the age embedding accepts. An unreachable process never becomes visible.
UNREACHABLE = 10**6


def hop_distances(topology: torch.Tensor) -> torch.Tensor:
    """Shortest-path hops on the positive entries of topology.

    Diagonal is distance 0 and is not an edge. Unreachable pairs stay at
    UNREACHABLE. topology [B, N, N] -> long tensor [B, N, N]. The mask is
    structural, so this does not keep a gradient.
    """
    batch, nodes, _ = topology.shape
    with torch.no_grad():
        eye = torch.eye(nodes, dtype=torch.bool, device=topology.device).expand(batch, nodes, nodes)
        adjacent = (topology > 0) & ~eye
        dist = torch.full((batch, nodes, nodes), UNREACHABLE, dtype=torch.long, device=topology.device)
        dist = dist.masked_fill(eye, 0)
        frontier = adjacent.clone()
        for hop in range(1, nodes):
            if not bool(frontier.any()):
                break
            dist = torch.where(frontier & (dist == UNREACHABLE), hop, dist)
            visited = dist < UNREACHABLE
            nxt = (frontier.to(dtype=topology.dtype) @ adjacent.to(dtype=topology.dtype)) > 0
            frontier = nxt & ~visited
    return dist


def attention_mask(topology: torch.Tensor, node_mask: torch.Tensor, history_len: int) -> torch.Tensor:
    """Additive mask [B, 1, L, L] over tokens (process, time).

    Time index τ runs from 0 (oldest history) to history_len (the current
    observation). Age of a token is history_len - τ. A query at τ may read a
    key at τ' only when τ' <= τ and the key's age is at least the hop distance.
    A process therefore sees its own current features and a neighbor's features
    only after they are old enough to have arrived. Absent processes are not
    keys. A query with no legal key falls back to itself so softmax stays defined.
    """
    batch, nodes, _ = topology.shape
    ticks = history_len + 1
    length = nodes * ticks
    dist = hop_distances(topology)
    tau = torch.arange(ticks, device=topology.device)
    query_time = tau.view(1, 1, ticks, 1, 1)
    key_time = tau.view(1, 1, 1, 1, ticks)
    key_age = (history_len - tau).view(1, 1, 1, 1, ticks)
    allow = key_time <= query_time
    allow = allow & (key_age >= dist.view(batch, nodes, 1, nodes, 1))
    allow = allow & node_mask.view(batch, nodes, 1, 1, 1) & node_mask.view(batch, 1, 1, nodes, 1)
    allow = allow.reshape(batch, length, length)
    none = ~allow.any(dim=-1)
    eye = torch.eye(length, dtype=torch.bool, device=topology.device).expand(batch, length, length)
    allow = torch.where(none.unsqueeze(-1), eye, allow)
    mask = torch.zeros(batch, 1, length, length, dtype=topology.dtype, device=topology.device)
    return mask.masked_fill(~allow.unsqueeze(1), float("-inf"))


class TransformerBlock(nn.Module):
    """Attention over the original tokens, not over the previous layer's mixture.

    Keys and values stay the raw local tokens. A deeper stack can transform a
    process's own representation, and it cannot turn a one-hop read into a
    two-hop read. Blocked weights are multiplied by zero so a masked token
    has no gradient.
    """

    def __init__(self, width: int, heads: int):
        super().__init__()
        if width % heads != 0:
            raise ValueError(f"width {width} must divide into {heads} heads")
        self.heads = heads
        self.ln_q = nn.LayerNorm(width)
        self.q = nn.Linear(width, width)
        self.kv = nn.Linear(width, 2 * width)
        self.proj = nn.Linear(width, width)
        self.ln2 = nn.LayerNorm(width)
        self.ff = nn.Sequential(nn.Linear(width, 4 * width), nn.GELU(), nn.Linear(4 * width, width))

    def forward(self, tokens, raw, mask):
        batch, length, width = tokens.shape
        head = width // self.heads
        query = self.q(self.ln_q(tokens)).view(batch, length, self.heads, head).permute(0, 2, 1, 3)
        key, value = self.kv(raw).chunk(2, dim=-1)
        key = key.view(batch, length, self.heads, head).permute(0, 2, 1, 3)
        value = value.view(batch, length, self.heads, head).permute(0, 2, 1, 3)
        logits = torch.matmul(query, key.transpose(-2, -1)) * (head ** -0.5)
        weights = torch.softmax(logits + mask, dim=-1)
        weights = weights.masked_fill(torch.isneginf(mask), 0)
        mixed = torch.matmul(weights, value).transpose(1, 2).reshape(batch, length, width)
        tokens = tokens + self.proj(mixed)
        return tokens + self.ff(self.ln2(tokens))


class TransformerActor(nn.Module):
    """One shared encoder. Tokens are (process, time); age is an embedding.

    No global feature enters this module. The action is a linear head on the
    current token of each process, passed through tanh.
    """

    def __init__(self, feat_dim: int, action_dim: int, hidden: int = 32, layers: int = 2,
                 heads: int = 4, max_age: int = 64):
        super().__init__()
        self.feat_dim = feat_dim
        self.action_dim = action_dim
        self.max_age = max_age
        self.inp = nn.Linear(feat_dim, hidden)
        self.age = nn.Embedding(max_age, hidden)
        self.blocks = nn.ModuleList(TransformerBlock(hidden, heads) for _ in range(layers))
        self.ln = nn.LayerNorm(hidden)
        self.head = nn.Linear(hidden, action_dim)

    def forward(self, obs, history, topology, node_mask):
        check_actor_inputs(obs, history, topology, node_mask)
        if obs.shape[-1] != self.feat_dim:
            raise ValueError(f"obs features {obs.shape[-1]} != {self.feat_dim}")
        history_len = history.shape[1]
        if history_len >= self.max_age:
            raise ValueError(f"history length {history_len} needs max_age > {history_len}")
        obs, history, topology = apply_node_mask(obs, history, topology, node_mask)
        frames = sequence(obs, history)
        batch, ticks, nodes, _ = frames.shape
        # frames time 0 is oldest (age = history_len). Current sits at the last index (age 0).
        ages = torch.arange(history_len, -1, -1, device=obs.device)
        # [B, N, ticks, F] so one process's tokens are contiguous after the flatten.
        local = frames.transpose(1, 2)
        tokens = self.inp(local) + self.age(ages).view(1, 1, ticks, -1)
        length = nodes * ticks
        raw = tokens.reshape(batch, length, -1)
        mask = attention_mask(topology, node_mask, history_len)
        flat = raw
        for block in self.blocks:
            flat = block(flat, raw, mask)
        flat = self.ln(flat)
        current_index = torch.arange(nodes, device=obs.device) * ticks + (ticks - 1)
        current = flat.index_select(1, current_index)
        present = node_mask.to(dtype=current.dtype).unsqueeze(-1)
        return torch.tanh(self.head(current)) * present


def build_actor(config: ActorConfig) -> nn.Module:
    """The only constructor the trainer and the evaluator call."""
    kind = config.actor_type
    if kind == "mlp":
        return MlpActor(config.feat_dim, config.action_dim, config.hidden)
    if kind == "gnn":
        return GnnActor(config.feat_dim, config.action_dim, config.hidden, config.taps)
    if kind == "transformer":
        return TransformerActor(
            config.feat_dim, config.action_dim, config.hidden, config.layers, config.heads, config.max_age
        )
    raise ValueError(f"unknown actor_type {kind!r}; expected mlp, gnn, or transformer")
