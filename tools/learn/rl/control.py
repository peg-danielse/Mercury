"""One control step from a Mercury `/llm/state` to one process action.

`decide` is what a controller calls. It runs the decentralized actor and returns
the call `execute` in `tools/llmctl.py` already posts: `hold`, or `scale_daemon`
with a `daemon_id` and a signed `cpus`. The actor's continuous vector stays the
proposal. `executed` is that call mapped back through `encode_call`, after the
step has been made legal for Mercury.

`remove_cores` never takes a daemon below one CPU (`replica_act` in
`src/daemons.cpp`). A negative step is shortened to leave one, and a step that
would remove the last CPU becomes `hold`. `scale_cpus` in `llmctl.py` accepts
only a non-zero integer whose absolute value is at most 64. This module emits
only those integers. It does not open a socket and does not call `execute`.
"""

from __future__ import annotations

from dataclasses import dataclass

import torch

from project import constrain_actions
from student import student_act
from teacher import CPU_SCALE, decode_action, encode_call, observe, processes


def core_count(daemon: dict) -> int | None:
    """CPUs the daemon holds now, from the `cpus` list `/llm/state` returns."""
    cpus = daemon.get("cpus")
    if isinstance(cpus, list):
        return len(cpus)
    cores = daemon.get("cores")
    if isinstance(cores, bool) or not isinstance(cores, int):
        return None
    return cores


def legal_delta(daemon: dict, cpus: int) -> int:
    """The signed step `run_tool` can post for this daemon. 0 means hold."""
    if not isinstance(cpus, int) or isinstance(cpus, bool) or cpus == 0 or abs(cpus) > 64:
        return 0
    if cpus < 0:
        have = core_count(daemon)
        if have is None:
            return 0
        cpus = max(cpus, -(have - 1))
    return cpus if cpus != 0 else 0


def project_call(proposal: torch.Tensor, ids: list[str], daemons: list[dict], mask: torch.Tensor):
    """Pick one process and return `(name, arguments, executed, daemon_id, cpus)`."""
    executed = torch.zeros(len(ids), 1)
    name, daemon_id, cpus = decode_action(proposal, ids, mask)
    if name != "scale_daemon" or daemon_id is None:
        return "hold", {"reason": "ctde"}, executed, None, 0
    by_id = {str(daemon.get("daemon_id")): daemon for daemon in daemons}
    stepped = legal_delta(by_id.get(daemon_id, {}), cpus)
    if stepped == 0:
        return "hold", {"reason": "ctde"}, executed, None, 0
    mapped = encode_call(ids, {"name": "scale_daemon", "input": {"daemon_id": daemon_id, "cpus": stepped}, "ok": True})
    return "scale_daemon", {"daemon_id": daemon_id, "cpus": stepped, "reason": "ctde"}, mapped["continuous"], daemon_id, stepped


@dataclass
class Decision:
    """The proposal, the call to post, and the tensors a replay row needs."""

    name: str
    arguments: dict
    proposal: torch.Tensor
    executed: torch.Tensor
    ids: list
    obs: torch.Tensor
    history: torch.Tensor
    topology: torch.Tensor
    mask: torch.Tensor
    global_state: torch.Tensor
    daemon_id: str | None
    cpus: int


def decide(actor, state: dict, previous: tuple[list[str], torch.Tensor] | None = None) -> tuple[Decision, tuple[list[str], torch.Tensor]]:
    """One period. `previous` is the `(ids, obs)` this function returned last time.

    The second return is the carry for the next call. Pass it back as `previous`.
    """
    ids, obs, history, topology, mask, global_state = observe(state, previous)
    if not ids:
        empty = torch.zeros(0, 1)
        decision = Decision(
            "hold", {"reason": "ctde"}, empty, empty, [], obs, history, topology, mask, global_state, None, 0,
        )
        return decision, (ids, obs)
    raw = student_act(
        actor, obs.unsqueeze(0), history.unsqueeze(0), topology.unsqueeze(0), mask.unsqueeze(0),
    )[0]
    proposal = constrain_actions(raw, limit=1.0, node_mask=mask)
    name, arguments, executed, daemon_id, cpus = project_call(proposal, ids, processes(state), mask)
    decision = Decision(
        name, arguments, proposal, executed, ids, obs, history, topology, mask, global_state, daemon_id, cpus,
    )
    return decision, (ids, obs)
