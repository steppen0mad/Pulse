# Current Work — Pulse

_Last updated: 2026-06-07_

## State: AI agent layer COMPLETE (Phases 0–3), on branch `ai-agent-layer`

Pulse is now an AI behaviour simulation that humans share: neural-network agents
and humans inhabit the same authoritative world over the unchanged UDP protocol.
See `doc/AI_LAYER.md` for the design and `Pulse_AI_Layer_Plan.md` for the full plan.

### What's done
- **Phase 0** — controller abstraction + shared perception (pure C, no ML).
  `include/agent.h`, `src/agent_obs.c` (shared `build_observation` +
  `agent_decode_action`), `src/agent.c` (frame-skip controller), `src/policy.c`
  (dependency-free forward pass + `policy.bin` loader), server `--bots/--policy/
  --frame-skip`, shared arena clamp in `world.c`, `MAX_CLIENTS` 8→32.
- **Phase 1** — cffi training env (`training/build_sim.py`, `env.py`) + single-file
  multi-discrete PPO (`ppo.py`) + nav reward (`rewards.py`). Navigation converges:
  episodic return −2.9 → ~27–28 (reproducible from seed 1, ~1.3M steps, ~7 min CPU).
- **Phase 2** — `export.py` (PyTorch → `policy.bin`, no transpose), `tests/test_policy.c`
  C-vs-PyTorch parity (max diff 3.8e-6, tol 1e-4) + forked bad-file abort. Server
  `--policy` deploys; frame-skip read from header.
- **Phase 3** — pursuit/evasion rewards + roles in `env.py`; `opponent_pool.py`
  (frozen snapshots) + PPO learner mask. Both self-play paths run.
- **Phase 4 (combat)** — NOT done (stretch; `BTN_FIRE` reserved, `fire` head exists).
- **Phase 5** — README rewritten, `doc/AI_LAYER.md` added.

## How to build / verify
- `make && make test` → builds everything + runs the full headless suite (no GPU,
  no Python — parity fixtures in `tests/data/` are committed). All green.
- Live demo: `./build/server --bots 5 --policy training/checkpoints/policy.bin`
  then `./build/client 127.0.0.1` (zero client changes; multiple humans can join).
- Training: `python3 -m venv .venv && . .venv/bin/activate &&
  pip install -r training/requirements.txt`, then `make sim-lib train export parity`.

## Key invariants (don't break — see `doc/AI_LAYER.md` parity section)
- `world.c` + `agent_obs.c` are compiled into BOTH the server and the cffi `.so`,
  with `-ffp-contract=off` both sides. One decode (`agent_decode_action`), one
  velocity formula (in `build_observation` from `World.prev_pos`).
- `policy.bin` bakes frame-skip + obs version; `policy_load` aborts on any
  mismatch (NO fallback). `export.py` writes weights with NO transpose.

## Open follow-ups
1. `checkpoints/` and `policy.bin` are gitignored (build artifacts); regenerate
   with `make train export`. The committed `tests/data/*.bin` fixtures are from a
   converged nav model.
2. Branch `ai-agent-layer` not yet merged to `main`.
3. Phase 4 combat (hitscan + health) is the remaining stretch.
