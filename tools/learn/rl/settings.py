"""The seven evaluation controllers, as one name.

`llm` is the existing tool-using teacher (`tools/llmctl.py`, any `--llm` other
than `rl`). The other six are one live path: `load_controller`, `decide`,
`execute`. They differ by the actor and by the procedure `train.py` runs.

    gnn-imitation       local graph observations, cloned from the teacher
    tf-imitation        the same cloning, with temporal attention
    gnn-rl              graph actor, TD3, no teacher
    tf-rl               temporal actor, TD3, no teacher
    gnn-imitation-rl    clone the teacher, then TD3 with the teacher term
    tf-imitation-rl     the same, with temporal attention

A checkpoint stores the name and the widths. Evaluation passes the file to
`--llm rl` and does not pass the name again.
"""

from __future__ import annotations

from dataclasses import dataclass

import torch

from actors import build_actor
from student import TrainConfig


@dataclass(frozen=True)
class Setting:
    name: str
    establishes: str
    actor_type: str | None = None
    training_mode: str | None = None
    teacher_type: str | None = None
    imitation_weight: float = 0.0
    imitation_decay: float = 1.0
    family: str = "scale"

    @property
    def learned(self) -> bool:
        return self.actor_type is not None

    def train_config(self) -> TrainConfig:
        if self.family == "action":
            raise ValueError(f"{self.name} is an action model; it has no scale TrainConfig")
        if not self.learned:
            raise ValueError(f"{self.name} is the teacher; it has no actor")
        return TrainConfig(
            self.actor_type,
            self.training_mode,
            self.teacher_type,
            imitation_weight=self.imitation_weight,
            imitation_decay=self.imitation_decay,
        )


SETTINGS: tuple[Setting, ...] = (
    Setting("llm", "Existing high-quality control baseline"),
    Setting(
        "gnn-imitation",
        "Whether local graph observations reproduce LLM decisions",
        "gnn", "bc", "llm",
    ),
    Setting(
        "tf-imitation",
        "Whether temporal attention improves imitation",
        "transformer", "bc", "llm",
    ),
    Setting(
        "gnn-rl",
        "Learning without teacher guidance",
        "gnn", "rl", "none",
    ),
    Setting(
        "tf-rl",
        "Learning without teacher guidance",
        "transformer", "rl", "none",
    ),
    Setting(
        "gnn-imitation-rl",
        "Benefit of teacher-guided graph learning",
        "gnn", "bc_then_rl", "llm", imitation_weight=1.0,
    ),
    Setting(
        "tf-imitation-rl",
        "Benefit of teacher-guided temporal learning",
        "transformer", "bc_then_rl", "llm", imitation_weight=1.0,
    ),
    Setting(
        "mlp-action",
        "Whether a full tool head can clone every teacher action",
        "mlp", "bc", "llm", family="action",
    ),
    Setting(
        "gnn-action",
        "Whether a graph encoder can clone every teacher action",
        "gnn", "bc", "llm", family="action",
    ),
    Setting(
        "tf-action",
        "Whether temporal attention can clone every teacher action",
        "transformer", "bc", "llm", family="action",
    ),
)

BY_NAME = {item.name: item for item in SETTINGS}


def get(name: str) -> Setting:
    try:
        return BY_NAME[name]
    except KeyError:
        known = ", ".join(item.name for item in SETTINGS)
        raise ValueError(f"unknown setting {name!r}; choose {known}") from None


def save_controller(actor, path: str, setting: Setting, config: TrainConfig | None = None) -> None:
    """Write the actor and the setting that trained it. Widths come from `config`."""
    if not setting.learned:
        raise ValueError(f"{setting.name} is the teacher; it has no checkpoint")
    cfg = config if config is not None else setting.train_config()
    if (cfg.actor_type, cfg.training_mode, cfg.teacher_type) != (
        setting.actor_type, setting.training_mode, setting.teacher_type,
    ):
        raise ValueError(f"config does not match setting {setting.name}")
    if actor.feat_dim != cfg.feat_dim or actor.action_dim != cfg.action_dim:
        raise ValueError("actor width does not match the training config")
    torch.save({
        "model": actor.state_dict(),
        "setting": setting.name,
        "actor_type": cfg.actor_type,
        "training_mode": cfg.training_mode,
        "teacher_type": cfg.teacher_type,
        "imitation_weight": cfg.imitation_weight,
        "imitation_decay": cfg.imitation_decay,
        "feat_dim": cfg.feat_dim,
        "action_dim": cfg.action_dim,
        "hidden": cfg.hidden,
        "taps": cfg.taps,
        "layers": cfg.layers,
        "heads": cfg.heads,
    }, path)


def load_controller(path: str):
    """Rebuild the actor from the checkpoint. Returns `(actor, setting)` in eval mode."""
    blob = torch.load(path, map_location="cpu", weights_only=False)
    setting = get(blob["setting"])
    if not setting.learned:
        raise ValueError(f"{setting.name} is the teacher; it has no checkpoint")
    if blob["actor_type"] != setting.actor_type:
        raise ValueError(f"checkpoint actor {blob['actor_type']} does not match setting {setting.name}")
    config = TrainConfig(
        blob["actor_type"],
        blob["training_mode"],
        blob["teacher_type"],
        imitation_weight=float(blob["imitation_weight"]),
        imitation_decay=float(blob["imitation_decay"]),
        feat_dim=int(blob["feat_dim"]),
        action_dim=int(blob["action_dim"]),
        hidden=int(blob["hidden"]),
        taps=int(blob["taps"]),
        layers=int(blob["layers"]),
        heads=int(blob["heads"]),
    )
    actor = build_actor(config.actor_config())
    actor.load_state_dict(blob["model"])
    actor.eval()
    return actor, setting
