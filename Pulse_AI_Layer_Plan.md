# Pulse: AI Agent Layer — Development Plan

This plan turns Pulse from a human-only multiplayer sandbox into one where
neural-network agents and humans share the same world, where the agents are
trained with self-play reinforcement learning, and where their policy runs as
hand-written C inference inside the existing 60 Hz server tick.

It is built to extend the current architecture, not replace it. The design reuses
the single most important idea already in the codebase: the simulation in
`world.c` is deterministic and shared by both sides. The AI layer adds a second
shared artifact alongside it, a deterministic observation builder, and unifies
agents and humans at the input layer so that nothing downstream has to know the
difference.

---

## 1. The core idea

An AI agent is a **server-side controller**. Every tick, for each agent slot, the
server:

1. builds a fixed-size observation vector from authoritative world state,
2. runs the policy-network forward pass to get an action,
3. decodes that action into the existing input struct `{buttons, yaw, pitch}`,
4. applies it through the same `world_apply_input` call a human input goes through.

Because the agent's output is an ordinary input and the agent's position is an
ordinary player state, **the wire protocol does not change at all**. Snapshots
already broadcast "all players" at 20 Hz, and the client already interpolates
"other players." Agents are simply more players. A human connecting to a server
running bots sees them move with zero client changes.

This is the architectural payoff and it is why the work is tractable solo: the
hard, already-solved parts (transport, prediction, reconciliation, interpolation,
snapshotting) are untouched. The new surface area is the agent controller, the
observation builder, the policy forward pass, and an offline training harness.

---

## 2. Architectural principle: a second shared artifact

The netcode works because `(state, input, dt)` produces the same result on the
client and the server. The AI layer introduces the same discipline for perception:

> The observation builder `build_observation(world, agent_id, out[])` is compiled
> into **both** the training environment and the live server, exactly as `world.c`
> is compiled into both client and server.

If the observation seen during training differs in any way from the observation
seen at deployment, the policy silently breaks. So the observation builder is
written once, in C, and the Python training environment calls that same C code.
This mirrors the existing determinism guarantee and removes a whole class of
train/deploy divergence bugs before they happen.

The reward function, by contrast, is only needed during training and never at
deployment, so it can live in Python where it is faster to iterate on.

---

## 3. What changes and what stays the same

**Stays the same:** the 17-byte header and seq/ack/ack_bits transport, the 60 Hz /
20 Hz cadence, the event-on-snapshot mechanism, heartbeats and timeouts, client
prediction, reconciliation, 100 ms interpolation, input redundancy, and the
deterministic `world_apply_input`. None of these are modified.

**Changes, minimally:**

- `server.c` gains a `controller_type` per player slot: `CONTROLLER_NETWORK`
  (input arrives in packets, as today) or `CONTROLLER_AGENT` (input is produced
  locally by a policy). The per-client "apply each input seq exactly once" rule is
  a network-dedup concern, so agent slots bypass it and call the per-tick apply
  directly. `world.c` itself needs no change.
- A `--bots <N>` and `--policy <file>` CLI flag, in the same style as the existing
  `--loss` and `--latency` flags, to spawn N agent slots loading a weights file.

**Is new:**

```
include/agent.h      controller interface, observation layout, action layout
src/agent_obs.c      SHARED observation builder (linked into env .so and server)
src/policy.c         hand-written forward pass + weight loading, no dependencies
src/agent.c          server-side controller: obs -> policy -> action -> input
training/env.py       Gym-style env wrapping the sim+obs shared library via cffi
training/ppo.py       single-file PPO (CleanRL-style) with a multi-discrete policy
training/export.py    PyTorch weights -> policy.bin (or a baked C header)
tests/test_obs.c      observation unit tests + train/deploy obs parity
tests/test_policy.c   C forward pass vs PyTorch forward pass parity
```

This slots directly into the existing component map.

---

## 4. Observation space

A small, egocentric, fixed-size float vector. Egocentric (everything relative to
the agent's own position and facing) so the policy generalizes across the arena
and stays small. Roughly 40 floats:

- **Self (5):** velocity in local frame (vx, vy, vz), on-ground flag, normalized
  pitch. Yaw is omitted because the frame is already yaw-relative.
- **K nearest other players (8 each, K=3 → 24):** range (normalized), bearing as
  (sin, cos), elevation, relative velocity (vx, vy), an is-enemy flag, and a
  line-of-sight flag. Slots beyond the number of present players are zero-padded
  and masked.
- **Environment rays (8):** a fan of normalized distances from short ray casts
  (ray vs arena AABBs) for obstacle and wall awareness. Can start as plain
  distance-to-boundary if the arena has no internal obstacles yet.
- **Task (3):** for navigation, the relative range and bearing (sin, cos) to the
  current target.

Sin/cos encoding for angles avoids the wraparound discontinuity at +/-180 degrees,
the same care the netcode already takes with `seq_greater` across the u16 wrap.

---

## 5. Action space

Multi-discrete, decoded into the existing `{buttons, yaw, pitch}` input struct so
`world_apply_input` is reused unchanged:

| Head        | Classes                 | Maps to                          |
|-------------|-------------------------|----------------------------------|
| move_x      | {-1, 0, +1}             | strafe-left / none / strafe-right |
| move_z      | {-1, 0, +1}             | back / none / forward            |
| jump        | {0, 1}                  | jump button                      |
| yaw_delta   | {-2, -1, 0, +1, +2} deg | added to maintained yaw          |
| pitch_delta | {-1, 0, +1} deg         | added to maintained pitch (clamped) |
| fire        | {0, 1} (combat phase)   | fire button                      |

The controller keeps the agent's current yaw and pitch, applies the chosen deltas,
clamps pitch with the existing logic, and writes absolute yaw and pitch into the
input. Multi-discrete with independent categorical heads gives clean PPO
log-probabilities (the joint log-prob is the sum of per-head log-probs).

Discrete look bins are chosen first because they train more stably than continuous
control. Continuous yaw/pitch (a Gaussian policy head) is a later refinement once
the discrete version works, and is the natural upgrade if aiming precision becomes
the bottleneck.

---

## 6. The policy network

A deliberately tiny MLP:

```
obs (~40) -> Dense(128) -> tanh -> Dense(128) -> tanh -> { policy heads, value head }
```

- Policy heads: one linear head per action dimension producing logits.
- Value head: one linear unit producing the state value, used by PPO during
  training and discarded at deployment.

Parameter count is on the order of tens of thousands. The forward pass is a handful
of matrix-vector products. This matters twice: it makes the project **trainable on
a laptop CPU** (the observation is a small vector, not pixels, so no GPU is
required and the bottleneck is the environment, which is native C), and it makes
**inference microsecond-scale** so it fits the 16 ms tick with enormous headroom.

---

## 7. Training

### 7.1 The environment: the C sim as a fast, headless Gym env

RL needs millions of steps. A real socket round-trip per step would make that
hopeless, so training does not use the network at all. Instead:

- Compile `world.c` and `agent_obs.c` into a shared library.
- `training/env.py` binds that library with cffi and exposes a standard
  `reset()` / `step(action)` interface: `step` decodes the action, calls the C
  per-tick apply, advances the sim, computes the reward in Python, and calls the
  C observation builder for the next state.
- Run with no 60 Hz sleep and no rendering, so it executes far faster than real
  time, and vectorize N parallel environments for batched rollouts.

Reusing the exact C sim and the exact C observation builder is what keeps the
trained policy valid when it moves to the server. The Python side reimplements
nothing about the world.

### 7.2 The algorithm: PPO

Use a single-file, readable PPO implementation (CleanRL-style) modified for the
custom multi-discrete policy, rather than a heavyweight framework. Owning a PPO
loop you understand line by line is more defensible in interview than importing an
opaque one, and it is the same instinct behind hand-writing the C inference.

Sensible starting hyperparameters (tune from here):

| Parameter            | Start value        |
|----------------------|--------------------|
| Discount gamma       | 0.99               |
| GAE lambda           | 0.95               |
| Clip epsilon         | 0.2                |
| Learning rate (Adam) | 3e-4               |
| Rollout length       | 2048 steps per env |
| Parallel envs        | 8 to 16            |
| Update epochs        | 10                 |
| Minibatch size       | 256                |
| Entropy coefficient  | 0.01               |
| Value coefficient    | 0.5                |
| Max grad norm        | 0.5                |
| Frame-skip k         | 2 to 4             |

Frame-skip (hold an action for k ticks) shortens the effective horizon and speeds
learning. Whatever k is used in training **must** be used at deployment, so the
agent decides every k ticks on the live server too. This is another train/deploy
parity constraint to lock down early.

### 7.3 Self-play and curriculum

Self-play is introduced in stages, and the word "self-play" is only earned once
agents are actually competing:

1. **Shared-parameter self-play.** All agents in the arena run one shared policy
   and learn simultaneously. Their opponents are, by construction, the policy
   itself. This is the simplest legitimate form of self-play.
2. **Opponent pool.** Periodically freeze a snapshot of the policy and sample from
   a pool of past snapshots as opponents. This prevents strategy cycling and the
   "chasing a moving target" instability that pure simultaneous self-play can hit.

Curriculum, ordered easy to hard, so there is a demonstrable result early and the
risky parts come last:

1. **Navigation** (single-agent, no opponents): reach a target, with obstacles.
   This validates movement, the observation builder, and the ray sensing. There is
   no opponent here, so this stage is reinforcement learning but not yet self-play.
2. **Pursuit and evasion** (two roles, self-play): a pursuer rewarded for closing
   and tagging, an evader rewarded for surviving. This is where emergent behavior
   appears and where "self-play" becomes genuinely true. This is the headline demo:
   a human can join and try to outrun the bots.
3. **Aim and fire** (combat, stretch): add the fire action, a hitscan, and health,
   with reward for landing hits and penalty for taking them. This is the hard one
   and is explicitly optional.

---

## 8. C inference

### 8.1 Export

A small Python script reads the trained PyTorch weights and writes a flat binary
(`policy.bin`): a short header (layer count, sizes, activation id) followed by each
layer's weight matrix and bias. An alternative is to emit a C header with the
arrays baked in as `static const float[]`, so the weights are compiled into the
binary and there is, in the most literal sense, no runtime dependency at all.

### 8.2 The forward pass

It is just loops. No BLAS, no libtorch, no Python:

```c
// policy.c (dependency-free)
typedef struct { int in, out; const float *W; const float *b; } Layer;

static void dense_tanh(const Layer *L, const float *x, float *y) {
    for (int o = 0; o < L->out; o++) {
        const float *w = L->W + (size_t)o * L->in;   // row o of W (out x in)
        float acc = L->b[o];
        for (int i = 0; i < L->in; i++) acc += w[i] * x[i];
        y[o] = tanhf(acc);
    }
}
// hidden layers use dense_tanh; the final heads are linear (no activation),
// then argmax per head selects the deployed action.
```

Deployment uses **argmax** decoding (or seeded sampling) rather than stochastic
sampling, so tiny floating-point differences between the PyTorch and C forward
passes cannot cause behavioral divergence except exactly at a decision boundary.

### 8.3 Parity test

The analog of the existing loopback convergence test. `tests/test_policy.c` runs a
batch of random observation vectors through the C forward pass and asserts the
logits match a reference set exported from PyTorch within a float tolerance
(for example 1e-4). The reference is generated once by the export script. This is a
differential test in the same spirit as the order-book fuzzer against `std::map`:
it proves the deployed implementation matches the trusted reference.

`tests/test_obs.c` does the same for perception: a known world state is fed through
both the Python training env's observation path and the C builder, asserting they
produce an identical vector.

### 8.4 Integration and performance

Wire `policy.c` into `src/agent.c`, load weights via `--policy`, and have each
agent slot run obs build plus forward pass plus decode on its decision tick.
Measure the added per-agent cost with the same `rdtsc` technique used for the
order book, and report it: expected to be single-digit microseconds per agent,
leaving the 16 ms budget essentially untouched, which directly substantiates
"running inference inside the 16ms tick." Report the maximum number of agents that
fit in one tick with headroom.

---

## 9. Testing and verification

The AI layer should match the rigor already in `tests/`, all headless and
CI-friendly:

- `test_obs.c`: observation unit tests (egocentric transforms, padding and
  masking, value ranges) plus train/deploy parity.
- `test_policy.c`: C-versus-PyTorch forward-pass parity across random inputs.
- Extend `test_loopback.c`: stand up a headless server populated with agents, run
  a real-localhost client, and assert the agents move within arena bounds, the
  snapshot and interpolation path renders them without crashing, and behavior is
  reproducible from a fixed seed.
- A performance microbenchmark for `build_obs + policy_forward + decode`,
  asserting the per-tick cost stays under budget.

A nice end-to-end validation: confirm an agent produces the same action sequence
whether driven in-process (training path) or server-side over a real loopback link
(deployment path). That parity is the proof the offline training transferred
faithfully, and it is a strong thing to be able to state in interview.

---

## 10. Phased milestones

Each phase ships something demonstrable. The architecture is de-risked before any
ML, so a failure to train well never blocks the parts that are guaranteed to work.

**Phase 0 — Controller abstraction and perception (no ML yet).** Add
`controller_type`, route agent slots through a stub controller that emits fixed or
random inputs, add `--bots N`, and confirm a human client sees bots wander via the
unchanged protocol. Implement and unit-test `agent_obs.c`. Specify the observation
and action layouts on paper.
*Outcome:* agents exist in-world; the input/state/snapshot unification is real and
tested. Rough effort: ~1 week.

**Phase 1 — Training environment and first learning signal.** Build the sim+obs
shared library, the cffi env, the arena, spawns, episode termination, and the
navigation reward. Stand up PPO and get an agent reliably reaching the target. Log
returns and save the learning curve.
*Outcome:* a trained navigation policy and a reproducible training run. Rough
effort: ~1 to 2 weeks (getting the first PPO run to learn is the main hurdle).

**Phase 2 — C inference, parity, and deployment.** Write the exporter, `policy.c`,
and the parity tests. Wire the trained net into the server agent controller. Run
the `rdtsc` benchmark and record the numbers. Watch the trained bots navigate with
a human client connected.
*Outcome:* the inference, deployment, and integration are done and measured. Rough
effort: ~1 week (mechanical for someone who shipped the order book).

**Phase 3 — Self-play and a compelling task.** Add the pursuer/evader roles and
reward, shared-parameter self-play, then an opponent pool. Train until pursuit and
evasion behavior emerges. Let humans join and play with or against the bots.
*Outcome:* "self-play" is genuinely earned and the project has a strong live demo.
Rough effort: ~2 to 4 weeks. This is the variable phase; RL convergence time is the
wildcard.

**Phase 4 (stretch) — Combat.** Add fire, hitscan, and health, with hit/survival
rewards. Attempt only after Phases 1 to 3 are solid.
*Outcome:* combat behavior, if it works. Rough effort: +2 to 4 weeks.

**Phase 5 — Portfolio polish.** A short companion design doc in the style of the
netcode document, learning-curve plots, the parity and performance numbers in the
README, and a recorded demo clip of humans and bots sharing the world.
*Outcome:* interview-ready, with concrete talking points. Rough effort: ~1 week.

A defensible, demoable result lives at the end of Phase 3, roughly 5 to 8 weeks of
part-time work, with Phase 3 carrying most of the uncertainty.

---

## 11. Additional Context

This is already put on a resume of a computer systems engineering undergradaute student applying for software development internships. The student has already made the CV bullet points before implementing the project. The points describing the project and showing engineering capabilities are:

Extended the authoritative 60Hz server so agents and humans share one world via the same input and snapshot path.
Trained agents via self-play reinforcement learning (PPO) over a custom observation space, action set and reward.
Hand-wrote the policy-network forward pass in C, running inference inside the 16ms tick with no runtime ML dependency.
Built the UDP layer on raw sockets with seq/ack/ackBits, client prediction, server reconciliation and interpolation.


## 12. When each CV bullet becomes true

Treat these as gates. A bullet goes on the CV only once its gate is cleared, so
every claim can be defended under questioning.

| CV bullet | Becomes honest after |
|-----------|----------------------|
| Extended the authoritative 60Hz server so agents and humans share one world via the same input and snapshot path. | **Phase 0** (true even with a stub controller; the unification is architectural). |
| Hand-wrote the policy-network forward pass in C, running inference inside the 16ms tick with no runtime ML dependency. | **Phase 2** (forward pass written, parity-tested, deployed, benchmarked under budget). |
| Built the UDP layer on raw sockets with seq/ack/ackBits, client prediction, server reconciliation and interpolation. | **Already true today.** |
| Trained agents via self-play reinforcement learning (PPO) over a custom observation space, action set and reward. | The **PPO / observation / action / reward** part is true after **Phase 1**. The word **"self-play"** is only true after **Phase 3**. If you stop at navigation, drop "self-play" and write "reinforcement learning (PPO)". |

That last row is the one to watch. Navigation is reinforcement learning but not
self-play, because there is no opponent. Do not put "self-play" on the CV until
pursuit-evasion is actually training.

