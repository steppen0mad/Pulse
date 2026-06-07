# Pulse — AI Agent Layer

This document explains how Pulse runs neural-network agents inside the same
authoritative world humans play in, and *why* it is built this way. It is the
companion to [`ARCHITECTURE.md`](../ARCHITECTURE.md), which covers the netcode.

## The core idea

An AI agent is a **server-side controller** occupying an ordinary player slot.
Every tick, for each agent slot, the server:

1. builds a fixed-size observation vector from authoritative world state,
2. runs the policy-network forward pass to get an action,
3. decodes that action into the existing input struct `{buttons, yaw, pitch}`,
4. applies it through the same `world_apply_input` a human input goes through.

Because the agent's output is an ordinary input and its position an ordinary
player state, **the wire protocol does not change at all**. Snapshots already
broadcast every player at 20 Hz and the client already interpolates other
players, so agents are simply more players. A human connecting to a server
running bots sees them with zero client changes — confirmed by
`tests/test_loopback_agents.c`, where a real localhost client decodes agents out
of ordinary snapshots.

This is the architectural payoff: the hard, already-solved parts (transport,
prediction, reconciliation, interpolation, snapshotting) are untouched. The new
surface area is the controller, the observation builder, the forward pass, and an
offline training harness.

## The discipline: a second shared artifact

The netcode works because `(state, input, dt)` produces the same result on client
and server — `world.c` is compiled into both. The AI layer applies the same
discipline to perception:

> `build_observation(world, agent_id, out[])` and `agent_decode_action(...)` are
> compiled into **both** the training environment and the live server.

If the observation seen during training differed in any way from the one seen at
deployment, the policy would silently break. So they are written once, in C
(`src/agent_obs.c`), and the Python training environment calls that same compiled
code through cffi (`training/env.py`). The reward, by contrast, is only needed
during training, so it lives in Python (`training/rewards.py`) where it is fast to
iterate on.

Two more parity guards close the loop: the cffi build uses `-ffp-contract=off`,
matching the Makefile so the shared library and the server round identically; and
`policy.bin` carries the frame-skip and an observation-layout version, both
checked at load so a mismatched cadence or stale layout aborts rather than
quietly misbehaving.

## What changes, and what stays the same

**Unchanged:** the 17-byte header and seq/ack/ack_bits transport, the 60/20 Hz
cadence, events-on-snapshot, heartbeats/timeouts, prediction, reconciliation,
100 ms interpolation, input redundancy, and `world_apply_input` itself.

**Minimal changes:** `server.c` gains a `controller_type` per slot —
`CONTROLLER_NETWORK` (input arrives in packets, as today) or `CONTROLLER_AGENT`
(input produced locally). Agent slots bypass the per-seq dedup (a transport
concern) and are skipped by the timeout/heartbeat logic (they have no peer). A
`--bots N` / `--policy FILE` flag spawns them. `world.c` gains a shared bounded
arena clamp so client, server, and training bound positions identically.

**New:** `include/agent.h` (the contract), `src/agent_obs.c` (shared perception +
decode), `src/policy.c` (the forward pass), `src/agent.c` (the controller), and
the `training/` harness.

## Observation space (40 floats, egocentric)

Everything is relative to the agent's own position and facing, so the policy
generalizes across the arena and stays small.

- **Self (5):** local-frame velocity (right, vertical, forward), an on-ground
  flag, and normalized pitch. Yaw is omitted because the frame is yaw-relative.
- **K=3 nearest other players (8 each):** range, bearing as `(sin, cos)`,
  elevation, relative velocity (right, forward), an is-enemy flag, and a
  line-of-sight flag. Missing players are zero-padded.
- **Environment rays (8):** a fan of normalized distances to the arena walls.
- **Task (3):** range and bearing `(sin, cos)` to the current target.

Angles use `sin/cos` to avoid the wraparound discontinuity at ±180°, the same
care the netcode takes with `seq_greater` across the u16 wrap. Velocity is
derived inside the builder from `World.prev_pos` (one formula, one place), so the
server and the training env compute it identically.

## Action space (multi-discrete)

Decoded into `{buttons, yaw, pitch}` so `world_apply_input` is reused unchanged:

| Head | Classes | Maps to |
|------|---------|---------|
| move_x | {−1,0,+1} | strafe left / none / right |
| move_z | {−1,0,+1} | back / none / forward |
| jump | {0,1} | up |
| yaw_delta | {−2,−1,0,+1,+2}° | added to maintained yaw |
| pitch_delta | {−1,0,+1}° | added to maintained pitch (clamped ±89°) |
| fire | {0,1} | reserved for the combat phase |

Independent categorical heads give clean PPO log-probabilities: the joint
log-prob is the sum of per-head log-probs, and the entropy is the sum of per-head
entropies. Deployment uses **argmax** (first-max tie-break) so tiny FP
differences between PyTorch and C cannot cause divergence except exactly at a
decision boundary.

## The policy network

A deliberately tiny MLP:

```
obs(40) -> Dense(128) -> tanh -> Dense(128) -> tanh -> { 6 policy heads, value head }
```

Only `Linear` and `Tanh` — anything else would have no hand-written C analog, and
`export.py` refuses to export a model containing other layer types. The value
head is used by PPO and discarded at deployment. Inference is a handful of
matrix-vector products: microsecond-scale, so it fits the 16 ms tick with
enormous headroom (~900–1000 agents per tick measured).

## Training

**The environment** (`training/env.py`) binds the C sim+perception as a cffi
extension and exposes a vectorized `reset()/step(action)`: `step` decodes the
action with the shared C decoder, advances the C sim `frame_skip` ticks while
holding the action, computes the reward in Python, and rebuilds the observation
with the shared C builder. No sockets, no sleep, no rendering — it runs far
faster than real time, and observations are asserted finite so a builder bug
surfaces loudly.

**The algorithm** (`training/ppo.py`) is a single-file, CleanRL-style PPO modified
for the multi-discrete policy: GAE(λ), a clipped surrogate, value and entropy
terms, and a learner mask so frozen-opponent rows contribute zero gradient.
Owning the loop line-by-line is the same instinct as hand-writing the C inference.

**Self-play and curriculum.** Navigation comes first (single agent, reach a
target) — reinforcement learning, but not yet self-play; it validates movement,
perception, and reward. Then pursuit/evasion: a pursuer rewarded for closing and
tagging, an evader for surviving — shared-parameter self-play, then an opponent
pool of frozen past snapshots (`training/opponent_pool.py`) to prevent strategy
cycling. This is where "self-play" is genuinely earned and where a human can join
and try to outrun the bots.

## C inference and parity

`training/export.py` writes a flat little-endian `policy.bin` — a short header
(dims, head sizes, frame-skip, obs version) followed by each layer's weight matrix
(row-major `out×in`, **no transpose**: PyTorch's `Linear.weight` already matches
the C `dense()` loop) and bias. `src/policy.c` loads it with `fread` and runs the
forward pass as plain loops, no BLAS or libtorch. A bad file aborts.

`tests/test_policy.c` is the differential test: it runs a batch of random
observations through the C forward pass and asserts the logits match a PyTorch
reference within 1e-4 (measured 3.8e-6) and that the per-head argmax agrees. This
is the analog of the netcode's loopback convergence test — it proves the deployed
implementation matches the trusted reference.

## Testing

All headless and CI-friendly (`make test`):

- `test_obs.c` — egocentric transforms, padding/masking, value ranges, decode.
- `test_policy.c` — C-vs-PyTorch forward-pass parity + a no-fallback abort check.
- `test_loopback_agents.c` — agents stay in-arena, are reproducible from a seed,
  and a real localhost client decodes them without crashing.
- `bench_agent.c` — the per-agent decision cost and how many fit one tick.

The parity fixtures under `tests/data/` are checked in, so `make test` needs no
Python — the C side is a self-contained proof.
