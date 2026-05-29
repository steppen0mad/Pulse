#ifndef PULSE_WORLD_H
#define PULSE_WORLD_H

#include "protocol.h"

/* Authoritative state of one player. Kept deliberately small -- this is what
 * snapshots carry and what client prediction reproduces. */
typedef struct {
    float pos[3];
    float yaw;
    float pitch;
} PlayerState;

#define MOVE_SPEED 5.0f   /* world units per second */

/*
 * The shared, deterministic simulation step. Compiled into BOTH the server
 * (where it is authoritative) and the client (where it drives prediction).
 *
 * Determinism is the whole point: given the same (state, cmd, dt), the result
 * is bit-for-bit identical on both sides, so a client replaying its unacked
 * inputs lands exactly where the server will put it.
 */
void world_apply_input(PlayerState *s, const InputCmd *cmd, float dt);

#endif /* PULSE_WORLD_H */
