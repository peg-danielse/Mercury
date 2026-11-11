"""Teacher-student training on top of the existing TD3 update.

`training_mode` is `bc`, `rl`, or `bc_then_rl`. `teacher_type` is `none` or
`llm`. Imitation uses the demonstration list. The online `Replay` is a
different buffer, and the critic update is `TD3Trainer.update`, which reads
`executed`. Inference is `student_act`: a local forward pass, with no teacher
and no call into `llmctl.py`.

`CorrectiveQueue.offer` returns immediately. A label is applied only when a
later `label` call delivers the teacher's tool. Nothing here waits on a model.
"""

from __future__ import annotations

from dataclasses import dataclass

import torch

from actors import ActorConfig, build_actor
from bc import clone_step, load_actor, save_actor
from replay import Replay
from teacher import Demonstration, demonstration_from_episode
from td3 import TD3Trainer


@dataclass
class TrainConfig:
    actor_type: str
    training_mode: str
    teacher_type: str
    imitation_weight: float = 0.0
    imitation_decay: float = 1.0
    feat_dim: int = 10
    action_dim: int = 1
    hidden: int = 32
    taps: int = 2
    layers: int = 2
    heads: int = 4

    def __post_init__(self):
        if self.actor_type not in ("mlp", "gnn", "transformer"):
            raise ValueError(f"actor_type must be mlp, gnn, or transformer, got {self.actor_type!r}")
        if self.training_mode not in ("bc", "rl", "bc_then_rl"):
            raise ValueError(f"training_mode must be bc, rl, or bc_then_rl, got {self.training_mode!r}")
        if self.teacher_type not in ("none", "llm"):
            raise ValueError(f"teacher_type must be none or llm, got {self.teacher_type!r}")
        if self.imitation_weight < 0:
            raise ValueError("imitation_weight must be non-negative")
        if not 0.0 <= self.imitation_decay <= 1.0:
            raise ValueError("imitation_decay must be in [0, 1]")

    def actor_config(self) -> ActorConfig:
        return ActorConfig(
            self.actor_type, self.feat_dim, self.action_dim, self.hidden, self.taps, self.layers, self.heads,
        )


class CorrectiveQueue:
    """States from the student, labeled later by whatever asks the LLM."""

    def __init__(self):
        self.pending: dict[str, dict] = {}
        self.ready: list[Demonstration] = []

    def offer(self, key: str, state: dict, run: str = "", workload: str | None = None, t_s: float = 0.0) -> None:
        self.pending[key] = {"state": state, "run": run, "workload": workload, "t_s": t_s}

    def label(self, key: str, call: dict) -> Demonstration:
        item = self.pending.pop(key)
        episode = {
            "run": item["run"],
            "t_s": item["t_s"],
            "state": item["state"],
            "calls": [call],
        }
        demo, _ = demonstration_from_episode(episode, workload=item["workload"])
        self.ready.append(demo)
        return demo

    def drain(self) -> list[Demonstration]:
        labeled = self.ready
        self.ready = []
        return labeled


def student_act(actor, obs, history, topology, mask) -> torch.Tensor:
    """One decentralized forward pass. No teacher, no tool call."""
    training = actor.training
    actor.eval()
    try:
        with torch.no_grad():
            return actor(obs, history, topology, mask)
    finally:
        actor.train(training)


class StudentTrainer:
    def __init__(self, config: TrainConfig, actor=None, critic=None, online_capacity: int = 10_000):
        self.config = config
        self.actor = actor if actor is not None else build_actor(config.actor_config())
        if critic is None:
            from critic import TwinCritic
            critic = TwinCritic(8, config.action_dim, config.feat_dim, hidden=config.hidden)
        self.critic = critic
        self.td3 = TD3Trainer(self.actor, self.critic)
        self.online = Replay(online_capacity)
        self.demos: list[Demonstration] = []
        self.weight = float(config.imitation_weight) if config.teacher_type == "llm" else 0.0
        self.queue = CorrectiveQueue()

    def add_demonstrations(self, demos: list[Demonstration]) -> None:
        self.demos.extend(demos)

    def pretrain(self, steps: int, batch_size: int = 16, seed: int = 0) -> list[float]:
        if self.config.teacher_type != "llm":
            raise RuntimeError("behavior cloning requires teacher_type llm")
        if self.config.training_mode not in ("bc", "bc_then_rl"):
            raise RuntimeError("pretrain is for training_mode bc or bc_then_rl")
        usable = [demo for demo in self.demos if demo.student_valid]
        if not usable:
            raise RuntimeError("no student-valid demonstrations")
        generator = torch.Generator().manual_seed(seed)
        losses = []
        for _ in range(steps):
            choice = torch.randint(len(usable), (min(batch_size, len(usable)),), generator=generator)
            losses.append(clone_step(self.actor, self.td3.actor_opt, [usable[int(i)] for i in choice]))
        return losses

    def _imitation(self, generator: torch.Generator) -> float | None:
        usable = [demo for demo in self.demos if demo.student_valid]
        if self.config.teacher_type != "llm" or self.weight == 0.0 or not usable:
            return None
        choice = torch.randint(len(usable), (min(16, len(usable)),), generator=generator)
        loss = clone_step(
            self.actor, self.td3.actor_opt, [usable[int(i)] for i in choice], coefficient=self.weight,
        )
        self.weight *= self.config.imitation_decay
        return loss

    def update(self, online_batch: dict | None = None, seed: int = 0) -> dict:
        """One step. `bc` does not touch the critic. `rl` with no teacher does not read demonstrations."""
        generator = torch.Generator().manual_seed(seed)
        if self.config.training_mode == "bc":
            return {
                "critic_loss": None,
                "actor_loss": None,
                "imitation_loss": self._imitation(generator),
                "imitation_weight": self.weight,
            }
        if online_batch is None:
            raise ValueError("rl and bc_then_rl require an online batch")
        stats = self.td3.update(online_batch)
        imitation = None
        if self.config.training_mode == "bc_then_rl" or self.config.teacher_type == "llm":
            imitation = self._imitation(generator)
        stats["imitation_loss"] = imitation
        stats["imitation_weight"] = self.weight
        return stats

    def save(self, path: str) -> None:
        save_actor(self.actor, path, self.config.actor_type)

    def load(self, path: str) -> dict:
        return load_actor(self.actor, path)
