"""TD3 update that is the same for every actor.

The critic backup uses `executed`, the action the constraints applied. The
actor loss feeds the live actor's proposal straight into the critic, so the
gradient is not taken through the clip. There is no rounding on either path.
"""

from __future__ import annotations

import copy

import torch
import torch.nn.functional as F


def soft_update(target: torch.nn.Module, source: torch.nn.Module, tau: float) -> None:
    with torch.no_grad():
        for dst, src in zip(target.parameters(), source.parameters()):
            dst.mul_(1.0 - tau).add_(src, alpha=tau)


class TD3Trainer:
    def __init__(self, actor, critic, lr: float = 1e-3, gamma: float = 0.99, tau: float = 0.005,
                 policy_noise: float = 0.2, noise_clip: float = 0.5, policy_delay: int = 2):
        self.actor = actor
        self.actor_target = copy.deepcopy(actor)
        self.critic = critic
        self.critic_target = copy.deepcopy(critic)
        self.actor_target.eval()
        self.critic_target.eval()
        for module in (self.actor_target, self.critic_target):
            for param in module.parameters():
                param.requires_grad_(False)
        self.actor_opt = torch.optim.Adam(actor.parameters(), lr=lr)
        self.critic_opt = torch.optim.Adam(critic.parameters(), lr=lr)
        self.gamma = gamma
        self.tau = tau
        self.policy_noise = policy_noise
        self.noise_clip = noise_clip
        self.policy_delay = policy_delay
        self.updates = 0

    def update(self, batch: dict) -> dict:
        with torch.no_grad():
            nxt = self.actor_target(
                batch["next_obs"], batch["next_history"], batch["next_topology"], batch["next_mask"]
            )
            noise = (torch.randn_like(nxt) * self.policy_noise).clamp(-self.noise_clip, self.noise_clip)
            nxt = (nxt + noise).clamp(-1.0, 1.0) * batch["next_mask"].unsqueeze(-1).to(dtype=nxt.dtype)
            q1_t, q2_t = self.critic_target(batch["next_global"], nxt, batch["next_mask"], batch["next_obs"])
            target = batch["reward"] + (1.0 - batch["done"]) * self.gamma * torch.minimum(q1_t, q2_t)
        q1, q2 = self.critic(batch["global_state"], batch["executed"], batch["node_mask"], batch["obs"])
        critic_loss = F.mse_loss(q1, target) + F.mse_loss(q2, target)
        self.critic_opt.zero_grad(set_to_none=True)
        critic_loss.backward()
        self.critic_opt.step()

        actor_loss = None
        self.updates += 1
        if self.updates % self.policy_delay == 0:
            proposal = self.actor(batch["obs"], batch["history"], batch["topology"], batch["node_mask"])
            q1_pi, _ = self.critic(batch["global_state"], proposal, batch["node_mask"], batch["obs"])
            actor_loss_t = -q1_pi.mean()
            self.actor_opt.zero_grad(set_to_none=True)
            actor_loss_t.backward()
            self.actor_opt.step()
            soft_update(self.actor_target, self.actor, self.tau)
            soft_update(self.critic_target, self.critic, self.tau)
            actor_loss = float(actor_loss_t.detach())
        return {"critic_loss": float(critic_loss.detach()), "actor_loss": actor_loss}
