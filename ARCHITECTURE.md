# Pulse — Netcode Architecture

This document explains how Pulse's multiplayer works and *why* it is built this
way. It is the design behind a first-person sandbox where many clients share one
world over UDP, with movement that feels instant locally despite real network
latency and loss.

## The problem

Networked action games face three tensions at once:

1. **Latency** — a round trip to the server is tens of milliseconds. If a client
   waited for the server to confirm every move before showing it, controls would
   feel sluggish.
2. **Cheating / divergence** — if each client simulated its own truth, clients
   would drift apart and players could trivially lie about their position.
3. **Loss & jitter** — UDP datagrams are dropped and reordered. TCP would "fix"
   this with head-of-line blocking, which is *worse* for games: one lost packet
   stalls everything behind it.

Pulse resolves these with the standard authoritative-server model (the same
shape used by Quake 3 and Valve's Source engine), built directly on UDP.

## Component map

```
include/protocol.h   wire constants, packet/message layout, tick rates
include/serialize.h  little-endian buffer cursor (bounds-checked reads)
src/net.c            UDP sockets + the seq/ack/ack_bits reliability layer
src/world.c          the deterministic simulation step (shared by both sides)
src/server.c         authoritative loop: 60 Hz sim, 20 Hz snapshots, events
src/client.c         prediction, reconciliation, interpolation, render + HUD
tests/               headless reliability unit tests + a loopback integration test
```

The simulation in `world.c` is compiled into **both** the server and the client.
That shared, deterministic step is the linchpin: because the same `(state, input,
dt)` produces the same result on both sides, a client can predict its own motion
and later land exactly where the server puts it.

## Transport: reliability over raw UDP (`net.c`)

Every datagram carries a fixed 17-byte header:

```
protocol_id (u32)  rejects foreign / stale packets
type        (u8)   CONNECT / ACCEPT / INPUT / SNAPSHOT / HEARTBEAT / DISCONNECT
seq         (u16)  this packet's sequence number
ack         (u16)  the most recent seq we have received from the peer
ack_bits    (u32)  a bitfield acking the 32 sequences before `ack`
tick        (u32)  sender's tick
```

This is the Gaffer-On-Games reliability scheme. We never retransmit at the
transport layer and never block. Instead:

- Each side tracks the highest sequence it has seen from the peer (`remote_seq`)
  and a 32-bit history of which of the preceding sequences arrived. That history
  *is* the outgoing `ack` + `ack_bits`.
- When a packet arrives, its `ack`/`ack_bits` tell us which of *our* packets the
  peer received. From the send timestamps of those packets we derive a smoothed
  **round-trip time** (EWMA) and a **packet-loss** estimate — the numbers shown
  live in the client HUD.
- 16-bit sequence numbers wrap; `seq_greater()` compares them modulo 2¹⁶ so the
  logic is correct across the wraparound boundary.

Because the transport is "unreliable but informed," each higher layer chooses its
own reliability strategy rather than paying for one-size-fits-all guarantees.

## Server: authoritative, fixed-timestep (`server.c`)

The server runs a **fixed 60 Hz** simulation. It is the only authority over
player position; clients send *intentions* (button states), never positions, so
a client cannot teleport by lying.

- **Inputs** arrive as small commands `{seq, buttons, yaw, pitch}`. Each unique
  `seq` is applied exactly once via `world_apply_input` at the fixed `TICK_DT`,
  advancing that player's authoritative state. The server remembers the last
  input seq it processed per client.
- **Snapshots** go out at **20 Hz** (every 3rd tick) to every client. A snapshot
  carries every player's state *and* — crucially — the `last_processed_input`
  for the receiving client, which the client needs to reconcile.
- **Events** (join/leave) piggyback on snapshots. The server keeps the few most
  recent events and includes them on every snapshot until they age out; clients
  dedupe by a monotonic event id. This makes rare events reliable without a
  dedicated ack channel, and the player roster in each snapshot self-heals
  membership even if an event is missed entirely.
- **Heartbeats & timeouts**: a client that goes silent past `CONNECT_TIMEOUT` is
  dropped; the server sends a keepalive if it would otherwise have nothing to
  say within `HEARTBEAT_INTERVAL`.

Snapshotting at 20 Hz while simulating at 60 Hz is a deliberate bandwidth/quality
trade: position changes little over 50 ms, and the client's interpolation (below)
smooths the gaps.

## Client: prediction, reconciliation, interpolation (`client.c`)

The client renders two kinds of entities very differently.

### Your own player — predict, then reconcile

- **Prediction.** The moment you press a key, the client applies that input to a
  local copy of your state with the *same* `world_apply_input` the server uses,
  and renders it immediately. No waiting for the server → zero input latency.
- Every applied input is kept in a **pending buffer** until the server
  acknowledges it.
- **Reconciliation.** When a snapshot arrives, the client snaps your state to the
  server's authoritative value, discards every pending input the server has now
  processed (`seq <= last_processed_input`), and **replays the inputs still
  pending** on top of the authoritative state. The result: your position is
  corrected to authority without throwing away the inputs you've already issued,
  so a misprediction resolves invisibly instead of rubber-banding.

### Other players — interpolate in the past

Remote players only update at the 20 Hz snapshot rate, and those snapshots
arrive jittered and occasionally lost. Rendering them at the latest known
position would look choppy and teleport on loss. Instead the client buffers
incoming snapshots and renders every remote player **`INTERP_DELAY` = 100 ms in
the past**, linearly interpolating position (and shortest-arc interpolating yaw)
between the two buffered snapshots that bracket that render time. Holding a small
window of history hides jitter and rides through a dropped snapshot at the cost
of showing others a tenth of a second late — the standard latency/smoothness
trade for non-local entities.

### Surviving packet loss — input redundancy

Rather than reliably retransmit lost inputs, the client simply **resends every
unacked input in each outgoing packet** (a capped window). Inputs are tiny, so
duplicating the last few costs little bandwidth, and the server's "apply each seq
once" rule makes duplicates free. A single lost datagram therefore never stalls
the simulation — the next packet already carries the same commands again. The
loopback test confirms the server's state still advances the full expected
distance under 40% packet loss.

## Verifying it (`tests/`, headless)

- **`test_reliability.c`** — pure unit tests of the seq/ack/ack_bits math:
  wraparound comparison, ack-bit generation with gaps and reordering, ack
  processing marking sent packets, RTT estimation, and loss accounting.
- **`test_loopback.c`** — stands up a miniature server and client over *real*
  localhost UDP using the production net/sim code, injects loss and latency, and
  asserts the two guarantees that matter: the authoritative state still advances
  under loss (inputs get through), and after input stops the client's predicted
  state converges **exactly** onto the server's. Runs across clean, lossy
  (20%), and very lossy + laggy (40% / 60 ms) links.

Both are graphics-free and exit non-zero on failure, so `make test` is a
self-contained, CI-friendly proof of the netcode — no GPU or display required.

## Demoing impairment

Both `server` and `client` accept `--loss <0..1>` and `--latency <ms>` flags that
drive the same artificial loss/delay used in the tests, so you can watch
prediction, reconciliation, and interpolation hold up live on a deliberately bad
link. The client HUD (title bar + a green/yellow/red corner swatch) shows RTT,
estimated loss, the latest snapshot tick, and player count in real time.
