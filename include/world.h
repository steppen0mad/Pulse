#ifndef PULSE_WORLD_H
#define PULSE_WORLD_H

#include "protocol.h"

typedef struct {
    float pos[3];
    float yaw;
    float pitch;
} PlayerState;

#define MOVE_SPEED 5.0f

#define ARENA_HALF_EXTENT 20.0f
#define ARENA_FLOOR        0.0f
#define ARENA_CEIL        10.0f
#define EYE_HEIGHT         1.7f

void world_apply_input(PlayerState *s, const InputCmd *cmd, float dt);

#endif
