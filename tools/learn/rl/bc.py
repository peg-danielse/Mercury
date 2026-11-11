"""Behavior cloning for a decentralized actor.

The loss sees the actor inputs only: local observations, history, topology,
and the mask. The global vector stored on a demonstration is not an actor
input. Continuous targets are the mapped teacher vector. Discrete agreement
is the decoded tool, used for evaluation, not as a second head inside the actor.
"""

from __future__ import annotations

import torch

from teacher import CPU_SCALE, Demonstration, decode_action


def collate(demos: list[Demonstration]) -> dict:
    if not demos:
        raise ValueError("no demonstrations")
    nodes = max(demo.obs.shape[0] for demo in demos)
    feat = demos[0].obs.shape[-1]
    ticks = demos[0].history.shape[0]
    action_dim = demos[0].continuous.shape[-1]
    batch = len(demos)

    def zeros(shape):
        return torch.zeros(shape)

    obs = zeros((batch, nodes, feat))
    history = zeros((batch, ticks, nodes, feat))
    topology = zeros((batch, nodes, nodes))
    mask = torch.zeros(batch, nodes, dtype=torch.bool)
    target = zeros((batch, nodes, action_dim))
    valid = torch.zeros(batch, dtype=torch.bool)
    for index, demo in enumerate(demos):
        n = demo.obs.shape[0]
        obs[index, :n] = demo.obs
        history[index, :, :n] = demo.history
        topology[index, :n, :n] = demo.topology
        mask[index, :n] = demo.mask
        target[index, :n] = demo.continuous
        valid[index] = demo.student_valid
    return {
        "obs": obs,
        "history": history,
        "topology": topology,
        "mask": mask,
        "target": target,
        "valid": valid,
    }


def imitation_loss(prediction: torch.Tensor, batch: dict) -> torch.Tensor:
    """Masked squared error on demonstrations the student action can represent."""
    present = batch["mask"].unsqueeze(-1).to(dtype=prediction.dtype)
    usable = batch["valid"].to(dtype=prediction.dtype).view(-1, 1, 1)
    weight = present * usable
    denom = weight.sum().clamp(min=1.0)
    return ((prediction - batch["target"]) ** 2 * weight).sum() / denom


def clone_step(actor, optimizer, demos: list[Demonstration], coefficient: float = 1.0) -> float:
    batch = collate(demos)
    if not bool(batch["valid"].any()) or coefficient == 0.0:
        return 0.0
    prediction = actor(batch["obs"], batch["history"], batch["topology"], batch["mask"])
    loss = imitation_loss(prediction, batch) * coefficient
    optimizer.zero_grad(set_to_none=True)
    loss.backward()
    optimizer.step()
    return float(loss.detach())


def clone(actor, demos: list[Demonstration], steps: int, lr: float = 1e-3, batch_size: int = 16,
          seed: int = 0) -> list[float]:
    """Pretrain `actor` on teacher demonstrations. The optimizer is local to cloning."""
    usable = [demo for demo in demos if demo.student_valid]
    if not usable:
        raise ValueError("no student-valid demonstrations")
    optimizer = torch.optim.Adam(actor.parameters(), lr=lr)
    generator = torch.Generator().manual_seed(seed)
    losses = []
    for _ in range(steps):
        choice = torch.randint(len(usable), (min(batch_size, len(usable)),), generator=generator)
        losses.append(clone_step(actor, optimizer, [usable[int(i)] for i in choice]))
    return losses


def agreement(actor, demos: list[Demonstration]) -> float:
    """Fraction of student-valid demonstrations whose decoded action matches the teacher tool."""
    usable = [demo for demo in demos if demo.student_valid]
    if not usable:
        return 0.0
    matched = 0
    was = actor.training
    actor.eval()
    with torch.no_grad():
        for demo in usable:
            prediction = actor(
                demo.obs.unsqueeze(0), demo.history.unsqueeze(0),
                demo.topology.unsqueeze(0), demo.mask.unsqueeze(0),
            )[0]
            name, daemon_id, cpus = decode_action(prediction, demo.ids, demo.mask)
            same = name == demo.tool_name
            if name == "scale_daemon":
                limit = int(CPU_SCALE)
                expected = max(-limit, min(limit, int(demo.cpus or 0)))
                same = same and daemon_id == demo.daemon_id and cpus == expected
            matched += int(same)
    actor.train(was)
    return matched / len(usable)


def save_actor(actor, path: str, actor_type: str) -> None:
    torch.save({
        "model": actor.state_dict(),
        "actor_type": actor_type,
        "feat_dim": actor.feat_dim,
        "action_dim": actor.action_dim,
    }, path)


def load_actor(actor, path: str) -> dict:
    blob = torch.load(path, map_location="cpu", weights_only=False)
    actor.load_state_dict(blob["model"])
    return blob
