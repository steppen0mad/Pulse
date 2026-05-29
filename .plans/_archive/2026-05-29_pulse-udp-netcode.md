---
status: done
date: 2026-05-29
title: Build the UDP netcode layer Pulse's CV bullets describe
---

# Pulse — Implement the Multiplayer UDP Netcode  (COMPLETED)

## Outcome
The repo previously had only a single-player camera (`src/main.c`). Built out the
full authoritative-UDP netcode the CV describes. All work verified headlessly.

## Shipped
- `include/protocol.h`, `include/serialize.h` — wire format + LE buffer cursor.
- `include/world.h` / `src/world.c` — deterministic shared sim step.
- `include/camera.h` / `src/camera.c` — mouse-look (extracted from old main.c).
- `include/net.h` / `src/net.c` — non-blocking UDP + seq/ack/ack_bits reliability,
  RTT/loss estimation, heartbeats/timeouts, artificial loss/latency for demos.
- `src/server.c` — authoritative 60 Hz sim, 20 Hz snapshots, join/leave events,
  per-client last-processed-input, `--loss`/`--latency` flags.
- `src/client.c` — prediction, reconciliation, 100 ms interpolation, redundant
  input resend, GL render + live net-stats HUD, `--loss`/`--latency` flags.
- `tests/test_reliability.c` (24 checks) + `tests/test_loopback.c` (real localhost
  UDP, converges exactly under up to 40% loss / 60 ms latency).
- `Makefile` targets: all/server/client/tests/test/run-server/run-client/clean.
- `ARCHITECTURE.md` design doc. `.gitignore` added; old `src/main.c` removed.

## Verification (all green)
- `make all` — zero warnings under -Wall -Wextra.
- `make test` — reliability unit tests + loopback integration tests pass.
- Live smoke: real `build/server` accepted a headless probe, streamed 20 Hz
  snapshots, processed ~60 inputs/s, advanced authoritative position correctly.

## Known follow-ups (not done — intentionally)
- README.md still describes the OLD single-player project and its build command
  references the deleted `src/main.c`; user opted not to have it rewritten.
- `test_glfw.c` / `test_glfw` binary at repo root are a leftover GL diagnostic.
- Nothing committed yet — user must review and commit when ready.
