"""
Single-file PPO (CleanRL-style) for the custom multi-discrete Pulse policy.

The network is deliberately tiny and contains ONLY Linear + Tanh layers, because
the deployed forward pass is hand-written in C (src/policy.c): anything fancier
(LayerNorm, residuals, biasless layers) would have no C analog. Trunk:
    obs(40) -> Linear(128) -> tanh -> Linear(128) -> tanh
Heads: one Linear per action dimension (logits) + one Linear value head (used by
PPO, discarded at deployment).

Owning the PPO loop line-by-line -- rather than importing a framework -- is the
same instinct as hand-writing the C inference: it is fully inspectable and the
multi-discrete log-prob (sum of per-head categorical log-probs) is explicit.

Usage:
    python training/ppo.py                      # train navigation, save checkpoint
    python training/ppo.py --updates 5          # short smoke run
"""
import argparse
import time
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.distributions import Categorical

import env as ENV

HERE = Path(__file__).resolve().parent
CKPT_DIR = HERE / "checkpoints"


def layer_init(layer, std=np.sqrt(2.0), bias=0.0):
    nn.init.orthogonal_(layer.weight, std)
    nn.init.constant_(layer.bias, bias)
    return layer


class MultiDiscreteActorCritic(nn.Module):
    def __init__(self, obs_dim, nvec, hidden=128):
        super().__init__()
        self.nvec = list(nvec)
        self.trunk = nn.Sequential(
            layer_init(nn.Linear(obs_dim, hidden)), nn.Tanh(),
            layer_init(nn.Linear(hidden, hidden)), nn.Tanh(),
        )
        # small init on policy heads (stable start), unit init on value
        self.heads = nn.ModuleList(
            [layer_init(nn.Linear(hidden, n), std=0.01) for n in self.nvec])
        self.value = layer_init(nn.Linear(hidden, 1), std=1.0)

    def get_value(self, x):
        return self.value(self.trunk(x)).squeeze(-1)

    def get_action_and_value(self, x, action=None):
        h = self.trunk(x)
        logits = [head(h) for head in self.heads]
        dists = [Categorical(logits=l) for l in logits]
        if action is None:
            action = torch.stack([d.sample() for d in dists], dim=-1)
        logprob = torch.stack(
            [d.log_prob(action[:, i]) for i, d in enumerate(dists)], dim=-1).sum(-1)
        entropy = torch.stack([d.entropy() for d in dists], dim=-1).sum(-1)
        return action, logprob, entropy, self.value(h).squeeze(-1)


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--task", default="navigation", choices=["navigation", "pursuit"])
    p.add_argument("--arenas", type=int, default=8)
    p.add_argument("--agents-per-arena", type=int, default=1)
    p.add_argument("--frame-skip", type=int, default=4)
    p.add_argument("--rollout", type=int, default=2048, help="steps per env per update")
    p.add_argument("--updates", type=int, default=60)
    p.add_argument("--epochs", type=int, default=10)
    p.add_argument("--minibatch", type=int, default=256)
    p.add_argument("--gamma", type=float, default=0.99)
    p.add_argument("--gae-lambda", type=float, default=0.95)
    p.add_argument("--clip", type=float, default=0.2)
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--ent-coef", type=float, default=0.01)
    p.add_argument("--vf-coef", type=float, default=0.5)
    p.add_argument("--max-grad-norm", type=float, default=0.5)
    p.add_argument("--hidden", type=int, default=128)
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--out", default=None, help="checkpoint path (default checkpoints/<task>.pt)")
    # self-play opponent pool (pursuit only)
    p.add_argument("--opponent-pool", action="store_true",
                   help="pursuit: play the evader against frozen past snapshots")
    p.add_argument("--pool-prob", type=float, default=0.5,
                   help="probability an arena's evader is a frozen opponent")
    p.add_argument("--freeze-every", type=int, default=10, help="updates between snapshots")
    p.add_argument("--pool-cap", type=int, default=20)
    return p.parse_args()


def main():
    args = parse_args()
    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    device = torch.device("cpu")

    env = ENV.PulseVecEnv(num_arenas=args.arenas,
                          agents_per_arena=args.agents_per_arena,
                          frame_skip=args.frame_skip, task=args.task, seed=args.seed)
    N = env.num_envs
    T = args.rollout
    obs_dim = ENV.OBS_DIM
    nvec = ENV.NVEC
    n_heads = len(nvec)

    agent = MultiDiscreteActorCritic(obs_dim, nvec, args.hidden).to(device)
    opt = torch.optim.Adam(agent.parameters(), lr=args.lr, eps=1e-5)

    # opponent pool (Phase 3 stage 2). Only meaningful for pursuit, where the
    # evader (odd env indices) can be driven by a frozen past snapshot.
    use_pool = args.opponent_pool and args.task == "pursuit"
    pool = None
    opp_models = [None] * args.arenas      # per-arena frozen evader, or None (live)
    if use_pool:
        from opponent_pool import OpponentPool, opponent_action
        pool = OpponentPool(lambda: MultiDiscreteActorCritic(obs_dim, nvec, args.hidden),
                            capacity=args.pool_cap)
        pool_rng = np.random.default_rng(args.seed + 7)

        def assign_opponent(k):
            opp_models[k] = (pool.sample(pool_rng)
                             if (len(pool) > 0 and pool_rng.random() < args.pool_prob)
                             else None)

    # rollout storage
    obs_buf = torch.zeros((T, N, obs_dim))
    act_buf = torch.zeros((T, N, n_heads), dtype=torch.long)
    logp_buf = torch.zeros((T, N))
    rew_buf = torch.zeros((T, N))
    done_buf = torch.zeros((T, N))
    val_buf = torch.zeros((T, N))
    mask_buf = torch.ones((T, N))          # 1 = learner row, 0 = frozen opponent

    next_obs = torch.tensor(env.reset(), dtype=torch.float32)
    next_done = torch.zeros(N)

    # episodic-return tracking (env auto-resets, so we accumulate here)
    ep_ret = np.zeros(N, np.float32)
    recent_returns = []

    global_step = 0
    start = time.time()
    print(f"[ppo] task={args.task} envs={N} rollout={T} (batch {T*N}) "
          f"updates={args.updates}")

    for update in range(1, args.updates + 1):
        for t in range(T):
            global_step += N
            obs_buf[t] = next_obs
            done_buf[t] = next_done
            with torch.no_grad():
                action, logp, _, value = agent.get_action_and_value(next_obs)
            val_buf[t] = value

            mask_row = torch.ones(N)
            if use_pool:
                # override each frozen-opponent evader's action; mask it out of loss
                for k in range(args.arenas):
                    if opp_models[k] is not None:
                        ev = k * env.apa + 1
                        action[ev] = opponent_action(opp_models[k], next_obs[ev].numpy())
                        mask_row[ev] = 0.0
            mask_buf[t] = mask_row
            act_buf[t] = action
            logp_buf[t] = logp

            o, r, d, _ = env.step(action.numpy())
            rew_buf[t] = torch.tensor(r)
            next_obs = torch.tensor(o, dtype=torch.float32)
            next_done = torch.tensor(d, dtype=torch.float32)

            ep_ret += r
            for i in range(N):
                if d[i]:
                    if mask_row[i] > 0:           # only log learner episodes
                        recent_returns.append(float(ep_ret[i]))
                    ep_ret[i] = 0.0
            if use_pool:                          # reassign opponents on arena reset
                for k in range(args.arenas):
                    if d[k * env.apa]:
                        assign_opponent(k)

        # GAE
        with torch.no_grad():
            next_value = agent.get_value(next_obs)
            adv = torch.zeros((T, N))
            lastgaelam = torch.zeros(N)
            for t in reversed(range(T)):
                if t == T - 1:
                    nextnonterminal = 1.0 - next_done
                    nextvalues = next_value
                else:
                    nextnonterminal = 1.0 - done_buf[t + 1]
                    nextvalues = val_buf[t + 1]
                delta = rew_buf[t] + args.gamma * nextvalues * nextnonterminal - val_buf[t]
                lastgaelam = delta + args.gamma * args.gae_lambda * nextnonterminal * lastgaelam
                adv[t] = lastgaelam
            ret = adv + val_buf

        # flatten
        b_obs = obs_buf.reshape(-1, obs_dim)
        b_act = act_buf.reshape(-1, n_heads)
        b_logp = logp_buf.reshape(-1)
        b_adv = adv.reshape(-1)
        b_ret = ret.reshape(-1)
        b_val = val_buf.reshape(-1)
        b_mask = mask_buf.reshape(-1)

        batch = T * N
        idx = np.arange(batch)
        clipfracs = []
        for _epoch in range(args.epochs):
            np.random.shuffle(idx)
            for s in range(0, batch, args.minibatch):
                mb = idx[s:s + args.minibatch]
                mb_mask = b_mask[mb]
                denom = mb_mask.sum().clamp(min=1.0)

                _, newlogp, entropy, newval = agent.get_action_and_value(
                    b_obs[mb], b_act[mb])
                ratio = (newlogp - b_logp[mb]).exp()

                # normalize advantages over learner rows only
                mb_adv = b_adv[mb]
                learner = mb_adv[mb_mask > 0]
                if learner.numel() > 1:
                    mb_adv = (mb_adv - learner.mean()) / (learner.std() + 1e-8)

                pg1 = -mb_adv * ratio
                pg2 = -mb_adv * torch.clamp(ratio, 1 - args.clip, 1 + args.clip)
                pg = torch.max(pg1, pg2)

                v = 0.5 * ((newval - b_ret[mb]) ** 2)
                # masked means: frozen-opponent rows contribute zero gradient
                pg_loss = (pg * mb_mask).sum() / denom
                v_loss = (v * mb_mask).sum() / denom
                ent_loss = (entropy * mb_mask).sum() / denom
                loss = pg_loss - args.ent_coef * ent_loss + args.vf_coef * v_loss

                opt.zero_grad()
                loss.backward()
                nn.utils.clip_grad_norm_(agent.parameters(), args.max_grad_norm)
                opt.step()
                with torch.no_grad():
                    clipfracs.append(((ratio - 1.0).abs() > args.clip).float().mean().item())

        window = recent_returns[-100:]
        mean_ret = float(np.mean(window)) if window else float("nan")
        sps = int(global_step / (time.time() - start))
        print(f"  update {update:3d}/{args.updates} | step {global_step:>8d} | "
              f"ep_ret(mean100) {mean_ret:7.2f} | n_ep {len(recent_returns):5d} | "
              f"v_loss {v_loss.item():.3f} | clipfrac {np.mean(clipfracs):.3f} | {sps} sps")

    # save checkpoint with everything deploy parity needs
    CKPT_DIR.mkdir(exist_ok=True)
    out = Path(args.out) if args.out else CKPT_DIR / f"{args.task}.pt"
    torch.save({
        "state_dict": agent.state_dict(),
        "nvec": nvec,
        "obs_dim": obs_dim,
        "hidden": args.hidden,
        "frame_skip": args.frame_skip,
        "obs_version": int(ENV.lib.OBS_VERSION),
    }, out)
    print(f"[ppo] saved checkpoint -> {out}")


if __name__ == "__main__":
    main()
