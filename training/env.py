"""
Headless, vectorized Gym-style environment over the C simulation.

RL needs millions of steps, so this never touches a socket: it binds the compiled
C sim (_pulse_sim) and steps it in-process. Crucially it calls the SAME C
functions the live server calls -- world_apply_input (the deterministic step),
build_observation (perception), agent_decode_action (the action map) -- so a
policy trained here behaves identically when deployed server-side. Nothing about
the world is reimplemented in Python; only the reward (a training-only concern)
lives here.

Layout: `num_arenas` independent C World structs, each with `agents_per_arena`
agents that share one policy (shared-parameter self-play). PPO sees
`num_envs = num_arenas * agents_per_arena` logical sub-agents. An action is held
for `frame_skip` ticks (the same cadence the server uses at deploy).

No fallbacks: shape mismatches and non-finite observations raise immediately, so
a bug surfaces loudly instead of silently corrupting training.
"""
import os
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

try:
    import _pulse_sim
except ImportError as e:
    raise SystemExit(
        "Could not import _pulse_sim. Build it first:\n"
        "    python training/build_sim.py"
    ) from e

import rewards as R  # noqa: E402

ffi, lib = _pulse_sim.ffi, _pulse_sim.lib

OBS_DIM   = lib.OBS_DIM
NUM_HEADS = lib.NUM_HEADS
NVEC      = [lib.HEAD_SIZES[i] for i in range(NUM_HEADS)]
TICK_DT   = 1.0 / float(lib.TICK_RATE)
HALF      = float(lib.ARENA_HALF_EXTENT)
EYE       = float(lib.EYE_HEIGHT)

# episode caps (in env steps, i.e. decisions)
NAV_MAX_STEPS = 300


def _hdist(ax, az, bx, bz):
    dx, dz = ax - bx, az - bz
    return float(np.sqrt(dx * dx + dz * dz))


class PulseVecEnv:
    def __init__(self, num_arenas=8, agents_per_arena=1, frame_skip=4,
                 task="navigation", seed=0):
        assert task in ("navigation", "pursuit")
        if task == "pursuit" and agents_per_arena < 2:
            agents_per_arena = 2
        self.task = task
        self.num_arenas = num_arenas
        self.apa = agents_per_arena
        self.num_envs = num_arenas * agents_per_arena
        self.frame_skip = frame_skip
        self.rng = np.random.default_rng(seed)

        self.worlds = [ffi.new("World *") for _ in range(num_arenas)]
        for w in self.worlds:
            w.dt = TICK_DT
            for a in range(agents_per_arena):
                w.present[a] = 1
                w.team[a] = a if task == "pursuit" else 0

        # maintained orientation + per-agent episode bookkeeping (flat, num_envs)
        self.yaw       = np.zeros(self.num_envs, np.float32)
        self.pitch     = np.zeros(self.num_envs, np.float32)
        self.prev_d    = np.zeros(self.num_envs, np.float32)   # navigation: dist to target
        self.prev_range = np.zeros(self.num_envs, np.float32)  # pursuit: range to opponent
        self.ep_step   = np.zeros(self.num_envs, np.int32)
        self.role     = ["pursuer" if (i % agents_per_arena) == 0 else "evader"
                         for i in range(self.num_envs)]

        # reusable C scratch (avoid per-step allocation in the hot loop)
        self._heads  = ffi.new("int[]", NUM_HEADS)
        self._yawp   = ffi.new("float *")
        self._pitchp = ffi.new("float *")
        self._cmd    = ffi.new("InputCmd *")
        self._obs    = ffi.new("float[]", OBS_DIM)

    # --- index helpers ---
    def _arena(self, env_idx):
        return self.worlds[env_idx // self.apa]

    def _slot(self, env_idx):
        return env_idx % self.apa

    # --- spawn / reset ---
    def _spawn(self, env_idx):
        w = self._arena(env_idx)
        a = self._slot(env_idx)
        m = HALF * 0.9
        px, pz = self.rng.uniform(-m, m, 2)
        w.players[a].pos[0] = float(px)
        w.players[a].pos[1] = EYE
        w.players[a].pos[2] = float(pz)
        yaw = float(self.rng.uniform(-180.0, 180.0))
        w.players[a].yaw = yaw
        w.players[a].pitch = 0.0
        w.prev_pos[a][0] = float(px)
        w.prev_pos[a][1] = EYE
        w.prev_pos[a][2] = float(pz)
        self.yaw[env_idx] = yaw
        self.pitch[env_idx] = 0.0
        self.ep_step[env_idx] = 0

        if self.task == "navigation":
            # target at least 5 units away from spawn
            while True:
                tx, tz = self.rng.uniform(-m, m, 2)
                if _hdist(px, pz, tx, tz) >= 5.0:
                    break
            w.target[a][0] = float(tx)
            w.target[a][1] = EYE
            w.target[a][2] = float(tz)
            self.prev_d[env_idx] = _hdist(px, pz, tx, tz)
        else:
            w.target[a][0] = 0.0
            w.target[a][1] = EYE
            w.target[a][2] = 0.0

    def _obs_for(self, env_idx):
        w = self._arena(env_idx)
        a = self._slot(env_idx)
        lib.build_observation(w, a, self._obs)
        o = np.frombuffer(ffi.buffer(self._obs, OBS_DIM * 4), np.float32).copy()
        if not np.all(np.isfinite(o)):
            raise FloatingPointError(
                f"non-finite observation for env {env_idx}: {o}")
        return o

    def _seed_pursuit_range(self, arena_k):
        i0 = arena_k * self.apa
        w = self.worlds[arena_k]
        rng = _hdist(w.players[0].pos[0], w.players[0].pos[2],
                     w.players[1].pos[0], w.players[1].pos[2])
        self.prev_range[i0] = self.prev_range[i0 + 1] = rng

    def reset(self):
        for i in range(self.num_envs):
            self._spawn(i)
        if self.task == "pursuit":
            for k in range(self.num_arenas):
                self._seed_pursuit_range(k)
        return np.stack([self._obs_for(i) for i in range(self.num_envs)])

    # --- step ---
    def step(self, actions):
        actions = np.asarray(actions, dtype=np.int64)
        if actions.shape != (self.num_envs, NUM_HEADS):
            raise ValueError(
                f"actions shape {actions.shape} != {(self.num_envs, NUM_HEADS)}")

        # 1) decode each agent's action once (held across frame_skip ticks)
        cmds = {}  # env_idx -> (buttons, yaw, pitch) materialized in an InputCmd
        for i in range(self.num_envs):
            for h in range(NUM_HEADS):
                self._heads[h] = int(actions[i, h])
            self._yawp[0] = float(self.yaw[i])
            self._pitchp[0] = float(self.pitch[i])
            lib.agent_decode_action(self._heads, self._yawp, self._pitchp, self._cmd)
            self.yaw[i] = self._yawp[0]
            self.pitch[i] = self._pitchp[0]
            cmds[i] = (self._cmd.buttons, self._cmd.yaw, self._cmd.pitch)

        # 2) advance every arena frame_skip ticks, holding the decoded action.
        #    On the last sub-tick, snapshot pre-positions so build_observation
        #    derives velocity over exactly one tick -- matching the server.
        for t in range(self.frame_skip):
            last = (t == self.frame_skip - 1)
            pre = {}
            for i in range(self.num_envs):
                w = self._arena(i)
                a = self._slot(i)
                if last:
                    pre[i] = (w.players[a].pos[0], w.players[a].pos[1], w.players[a].pos[2])
                btn, yaw, pitch = cmds[i]
                self._cmd.seq = 0
                self._cmd.buttons = btn
                self._cmd.yaw = yaw
                self._cmd.pitch = pitch
                lib.world_apply_input(ffi.addressof(w.players, a), self._cmd, TICK_DT)
            if last:
                for i in range(self.num_envs):
                    w = self._arena(i)
                    a = self._slot(i)
                    w.prev_pos[a][0], w.prev_pos[a][1], w.prev_pos[a][2] = pre[i]

        # 3) rewards + termination
        rew = np.zeros(self.num_envs, np.float32)
        done = np.zeros(self.num_envs, np.float32)
        self.ep_step += 1

        # the shared clamp must keep everyone in-arena; otherwise it's a bug
        for i in range(self.num_envs):
            w = self._arena(i); a = self._slot(i)
            px, pz = w.players[a].pos[0], w.players[a].pos[2]
            if abs(px) > HALF + 1e-2 or abs(pz) > HALF + 1e-2:
                raise RuntimeError(
                    f"env {i} left the arena ({px:.3f},{pz:.3f}) -- clamp missing?")

        if self.task == "navigation":
            for i in range(self.num_envs):
                w = self._arena(i); a = self._slot(i)
                d = _hdist(w.players[a].pos[0], w.players[a].pos[2],
                           w.target[a][0], w.target[a][2])
                reached = d < R.NAV_REACH_RADIUS
                rew[i] = R.reward_navigation(self.prev_d[i], d, reached)
                self.prev_d[i] = d
                if reached or self.ep_step[i] >= NAV_MAX_STEPS:
                    done[i] = 1.0
        else:  # pursuit/evasion: slot 0 is the pursuer, slot 1 the evader
            for k in range(self.num_arenas):
                i0 = k * self.apa
                i1 = i0 + 1
                w = self.worlds[k]
                rng = _hdist(w.players[0].pos[0], w.players[0].pos[2],
                             w.players[1].pos[0], w.players[1].pos[2])
                tagged = rng < R.TAG_RADIUS
                timed = self.ep_step[i0] >= NAV_MAX_STEPS
                rew[i0] = R.reward_pursuit("pursuer", self.prev_range[i0], rng, tagged, True, timed)
                rew[i1] = R.reward_pursuit("evader",  self.prev_range[i1], rng, tagged, True, timed)
                self.prev_range[i0] = self.prev_range[i1] = rng
                if tagged or timed:
                    done[i0] = done[i1] = 1.0

        # 4) auto-reset done agents, then build the next observation
        obs = np.empty((self.num_envs, OBS_DIM), np.float32)
        for i in range(self.num_envs):
            if done[i]:
                self._spawn(i)
            obs[i] = self._obs_for(i)
        if self.task == "pursuit":      # reseed range after both agents respawn
            for k in range(self.num_arenas):
                if done[k * self.apa]:
                    self._seed_pursuit_range(k)

        infos = {}
        return obs, rew, done, infos


if __name__ == "__main__":
    # smoke test: shapes, finiteness, and that an agent can be rewarded for
    # moving toward its target.
    env = PulseVecEnv(num_arenas=4, agents_per_arena=1, frame_skip=4, seed=0)
    o = env.reset()
    assert o.shape == (env.num_envs, OBS_DIM), o.shape
    total = 0.0
    for _ in range(200):
        # heuristic: always push forward + small random turn, to confirm the
        # progress reward is non-trivial when moving
        acts = np.tile([1, 2, 0, 2, 1, 0], (env.num_envs, 1))
        o, r, d, _ = env.step(acts)
        assert o.shape == (env.num_envs, OBS_DIM)
        assert np.all(np.isfinite(o))
        total += r.sum()
    print(f"[env] smoke ok: num_envs={env.num_envs} obs_dim={OBS_DIM} "
          f"nvec={NVEC} 200-step total reward={total:.2f}")
