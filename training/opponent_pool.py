"""
Opponent pool for self-play (Phase 3, stage 2).

Pure simultaneous self-play (every agent always the current policy) can chase a
moving target and cycle strategies. The standard fix is to periodically freeze a
snapshot of the policy into a pool and, on some episodes, play the learner
against a sampled PAST snapshot instead of its live self. Those opponent agents
run inference-only and are masked out of the PPO loss (no gradient flows through
them), so the learner faces a diverse, stable distribution of opponents.

This module is deliberately small: a ring buffer of frozen, eval-mode copies of
the actor-critic. ppo.py owns when to freeze and when to assign a pool opponent.
"""
import copy

import torch


class OpponentPool:
    def __init__(self, make_model, capacity=20):
        """make_model() -> a fresh MultiDiscreteActorCritic with the right dims."""
        self._make = make_model
        self.capacity = capacity
        self._snaps = []   # list of state_dicts (CPU)

    def __len__(self):
        return len(self._snaps)

    def freeze(self, agent):
        """Snapshot the current policy weights into the pool (ring buffer)."""
        sd = copy.deepcopy({k: v.detach().cpu().clone() for k, v in agent.state_dict().items()})
        self._snaps.append(sd)
        if len(self._snaps) > self.capacity:
            self._snaps.pop(0)

    def sample(self, rng):
        """Return a frozen, eval-mode model drawn uniformly from the pool, or
        None if the pool is empty (caller then uses the live policy)."""
        if not self._snaps:
            return None
        sd = self._snaps[int(rng.integers(len(self._snaps)))]
        m = self._make()
        m.load_state_dict(sd)
        m.eval()
        for p in m.parameters():
            p.requires_grad_(False)
        return m


@torch.no_grad()
def opponent_action(model, obs_row):
    """Deterministic (argmax) action for a single observation, as a (n_heads,)
    long tensor -- matches the deployed argmax decode."""
    x = torch.as_tensor(obs_row, dtype=torch.float32).unsqueeze(0)
    h = model.trunk(x)
    return torch.stack([head(h).argmax(dim=-1).squeeze(0) for head in model.heads])
