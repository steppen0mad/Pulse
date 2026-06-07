# Pulse

An **AI behaviour simulation that humans share**. Pulse is a first-person
multiplayer sandbox built on a hand-rolled UDP netcode layer, extended so that
neural-network agents and real humans inhabit the *same* authoritative world.
The agents are trained offline with self-play reinforcement learning (PPO), and
their policy runs as hand-written C inference inside the 60 Hz server tick — no
runtime ML dependency.

The whole design turns on one idea: **an agent is just another player whose input
is produced locally instead of arriving in a packet.** Because an agent emits an
ordinary input command and occupies an ordinary player slot, the wire protocol,
snapshots, and the entire client are untouched — a human who joins sees bots as
ordinary remote players, and multiple humans can join at once to interact with
them.

```
            ┌─────────── authoritative 60 Hz server ───────────┐
 humans ───►│  network slots ─┐                                │
 (UDP in)   │                 ├─► world_apply_input ─► snapshot │──► all clients
 agents ───►│  agent slots  ──┘     (one shared sim)     20 Hz  │     (20 Hz)
 (policy)   │   obs → policy → action ─┘                        │
            └──────────────────────────────────────────────────┘
```

## Two layers, two design docs

| Layer | What it does | Doc |
|-------|--------------|-----|
| **Netcode** | raw-UDP transport with seq/ack/ack_bits, client prediction, server reconciliation, interpolation | [`ARCHITECTURE.md`](ARCHITECTURE.md) |
| **AI agent layer** | shared observation builder, multi-discrete policy, hand-written C inference, offline PPO + self-play | [`doc/AI_LAYER.md`](doc/AI_LAYER.md) |

## Build & run

Requires GCC, GLFW3, OpenGL/GLU (client only; the server and all tests are
graphics-free and CI-friendly).

```bash
sudo apt install -y libglfw3 libglfw3-dev libgl1-mesa-dev libglu1-mesa-dev

make                 # builds build/server, build/client, and the test binaries
make test            # runs the full headless test suite (no GPU/display needed)
```

### A human sharing the world with bots

```bash
# stub bots that wander (Phase 0 — needs no trained policy):
./build/server --bots 5
# or a trained navigation policy:
./build/server --bots 5 --policy training/checkpoints/policy.bin

# then connect one or more human clients (each is an ordinary player):
./build/client 127.0.0.1
```

Add `--loss 0.15 --latency 100` to the server or client to demo prediction,
reconciliation, and interpolation holding up over a deliberately bad link.

## Training a policy

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r training/requirements.txt

make sim-lib         # compile the C sim+perception into a cffi extension
make train           # PPO navigation (CPU); episodic return climbs to ~27
make export          # PyTorch weights -> policy.bin + refresh C parity fixtures
make parity          # assert the C forward pass matches PyTorch within 1e-4
```

Self-play (pursuit/evasion):

```bash
python training/ppo.py --task pursuit --arenas 8 --agents-per-arena 2 \
                       --opponent-pool
```

## Why it is correct: train/deploy parity

The training environment and the live server compile the **same** C sources —
`world.c` (the deterministic step) and `agent_obs.c` (the observation builder and
action decoder). Training calls them through cffi; the server calls them
directly. There is therefore no second implementation of the world or the
perception to drift out of sync. Concretely:

- the C policy forward pass matches PyTorch to **3.8e-6** (tolerance 1e-4),
- a corrupt weights file **aborts loudly** rather than silently degrading,
- frame-skip is baked into `policy.bin` so deployment decides at the trained
  cadence, and a stale observation layout is rejected at load.

## Numbers

| Metric | Value |
|--------|-------|
| Per-agent decision cost (obs + forward + decode) | ~16–19 µs |
| Agents fitting one 60 Hz tick (full budget) | ~900–1000 |
| C-vs-PyTorch logit parity (max abs diff) | 3.8e-6 |
| Navigation PPO episodic return | −2.9 → ~27 |
| Netcode convergence under 40% loss / 60 ms | exact (gap 0.00000) |

## Layout

```
include/   protocol, serialize, net, world, camera, agent, policy headers
src/       net.c world.c server.c client.c camera.c   (netcode + sim + render)
           agent_obs.c  agent.c  policy.c             (the AI agent layer)
training/  build_sim.py env.py ppo.py rewards.py export.py opponent_pool.py
tests/     reliability, loopback, obs, policy parity, agent loopback, benchmark
```
