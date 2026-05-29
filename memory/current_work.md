# Current Work — Pulse

_Last updated: 2026-05-29_

## State: netcode implementation COMPLETE, not yet committed

The project now implements the full UDP netcode described on the owner's CV
(authoritative server, prediction, reconciliation, interpolation, reliability).
See `.plans/_archive/2026-05-29_pulse-udp-netcode.md` for the full breakdown and
`ARCHITECTURE.md` for the design.

## How to build / verify
- `make` → builds `build/server`, `build/client`, and the test binaries.
- `make test` → runs headless reliability unit tests + loopback integration test
  (no display needed). Currently green.
- `make run-server` then `make run-client 127.0.0.1` → live GL demo. Add
  `--loss 0.15 --latency 100` to either to demo bad-network resilience.

## Open follow-ups
1. **README.md is stale** — still describes the old single-player engine and its
   build command points at the deleted `src/main.c`. Owner chose not to rewrite
   it during the build; should be fixed before sharing the repo.
2. `test_glfw.c` + `test_glfw` binary at repo root: leftover GL diagnostic, could
   be removed.
3. Nothing is committed — review `git status` and commit when ready (currently on
   `main`; consider a feature branch).
