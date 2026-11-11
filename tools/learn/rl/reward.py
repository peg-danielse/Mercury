"""Scalar reward from the RL contract in SPEC.md.

The weights are the starting hypotheses written there. The caller passes
`action_cost` already summed for the joint step. This function does not look
at an actor or turn a continuous proposal into a discrete tool.
"""

from __future__ import annotations


def step_reward(done: float, sent: float, p95_ms: float, p95_slo_ms: float, errors: float,
                busy: float, cores: float, pool_max: float, action_cost: float,
                error_budget: float = 0.01) -> tuple[float, dict]:
    if p95_slo_ms <= 0:
        raise ValueError("p95_slo_ms must be positive")
    sent_floor = max(sent, 1.0)
    goodput = min(done / sent_floor, 1.0)
    violation = max(p95_ms / p95_slo_ms - 1.0, 0.0)
    error_fraction = errors / sent_floor
    bonus = 1.0 if violation == 0.0 and error_fraction <= error_budget else 0.0
    resource = (cores / pool_max) if pool_max else 0.0
    terms = {
        "goodput_fraction": goodput,
        "latency_violation": min(violation, 2.0),
        "error_fraction": error_fraction,
        "utilization": busy * bonus,
        "resource_fraction": resource,
        "action_cost": action_cost,
    }
    value = (
        1.0 * terms["goodput_fraction"]
        - 2.0 * terms["latency_violation"]
        - 3.0 * terms["error_fraction"]
        + 0.2 * terms["utilization"]
        - 0.2 * terms["resource_fraction"]
        - 0.1 * terms["action_cost"]
    )
    return value, terms
