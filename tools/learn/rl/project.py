"""Constraints applied after the actor, outside its autograd graph.

`constrain_actions` clips a proposal. It does not round. A caller that posts
the result through Mercury detaches it first; the trainer's actor loss reads
the proposal, and the replay stores both tensors.
"""

from __future__ import annotations

import torch


def constrain_actions(proposal: torch.Tensor, limit: float = 1.0, node_mask: torch.Tensor | None = None) -> torch.Tensor:
    if limit < 0:
        raise ValueError("limit must be non-negative")
    if proposal.dim() not in (2, 3):
        raise ValueError("proposal must be [N, A] or [B, N, A]")
    executed = proposal.clamp(-limit, limit)
    if node_mask is not None:
        expected = proposal.shape[:-1]
        if node_mask.shape != expected or node_mask.dtype != torch.bool:
            raise ValueError(f"node_mask must be {tuple(expected)} bool")
        executed = executed * node_mask.to(dtype=executed.dtype).unsqueeze(-1)
    return executed
