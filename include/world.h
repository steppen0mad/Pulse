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
 * Shared arena bounds. The simulation clamps every player's position inside
 * this box at the end of world_apply_input, so the clamp is bit-for-bit
 * identical for client prediction, server authority, and the training
 * environment -- the same determinism discipline as the rest of the sim. The
 * AI observation builder and navigation reward sense this same box.
 */
#define ARENA_HALF_EXTENT 20.0f   /* +/- X and Z walls; matches the client's floor grid */
#define ARENA_FLOOR        0.0f   /* lowest Y                                            */
#define ARENA_CEIL        10.0f   /* highest Y (players can fly with UP/DOWN)            */
#define EYE_HEIGHT         1.7f   /* spawn/standing height; the on-ground reference      */

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
