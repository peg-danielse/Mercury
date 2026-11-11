"""Centralized twin critic.

Each Q network encodes the concatenation of a process's own features and its
action, then pools the present rows with a masked mean and a masked max.
Absent rows are zeroed before that encoder, so a pad cannot change the value
and receives no action gradient. The module does not know which actor produced
the action. The action tensor stays in the autograd graph.
"""

from __future__ import annotations

import torch
import torch.nn as nn


class _Q(nn.Module):
    def __init__(self, global_dim: int, action_dim: int, feat_dim: int, hidden: int):
        super().__init__()
        self.global_dim = global_dim
        self.action_dim = action_dim
        self.feat_dim = feat_dim
        self.pair = nn.Sequential(
            nn.Linear(feat_dim + action_dim, hidden),
            nn.ReLU(),
            nn.Linear(hidden, hidden),
            nn.ReLU(),
        )
        self.head = nn.Sequential(
            nn.Linear(global_dim + 2 * hidden + 1, hidden),
            nn.ReLU(),
            nn.Linear(hidden, 1),
        )

    def forward(self, global_state, actions, node_mask, node_feat):
        if global_state.dim() != 2 or global_state.shape[-1] != self.global_dim:
            raise ValueError(f"global_state must be [B, {self.global_dim}], got {tuple(global_state.shape)}")
        if actions.dim() != 3 or actions.shape[-1] != self.action_dim:
            raise ValueError(f"actions must be [B, N, {self.action_dim}], got {tuple(actions.shape)}")
        if node_feat.shape[:2] != actions.shape[:2] or node_feat.shape[-1] != self.feat_dim:
            raise ValueError("node_feat must be [B, N, F] aligned with actions")
        if node_mask.shape != actions.shape[:2] or node_mask.dtype != torch.bool:
            raise ValueError("node_mask must be [B, N] bool aligned with actions")
        present = node_mask.to(dtype=actions.dtype).unsqueeze(-1)
        count = node_mask.sum(dim=1, keepdim=True).to(dtype=actions.dtype)
        paired = torch.cat([node_feat * present, actions * present], dim=-1)
        encoded = self.pair(paired) * present
        mean = encoded.sum(dim=1) / count.clamp(min=1.0)
        filled = encoded.masked_fill(~node_mask.unsqueeze(-1), torch.finfo(encoded.dtype).min)
        maximum = filled.max(dim=1).values
        empty = count == 0
        mean = torch.where(empty, torch.zeros_like(mean), mean)
        maximum = torch.where(empty, torch.zeros_like(maximum), maximum)
        return self.head(torch.cat([global_state, mean, maximum, count], dim=-1)).squeeze(-1)


class TwinCritic(nn.Module):
    def __init__(self, global_dim: int, action_dim: int, feat_dim: int, hidden: int = 128):
        super().__init__()
        self.q1 = _Q(global_dim, action_dim, feat_dim, hidden)
        self.q2 = _Q(global_dim, action_dim, feat_dim, hidden)

    def forward(self, global_state, actions, node_mask, node_feat):
        return self.q1(global_state, actions, node_mask, node_feat), self.q2(
            global_state, actions, node_mask, node_feat
        )
