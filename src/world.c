#include "world.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void world_apply_input(PlayerState *s, const InputCmd *cmd, float dt) {
    s->yaw   = cmd->yaw;
    s->pitch = cmd->pitch;

    float v      = MOVE_SPEED * dt;
    float yawRad = s->yaw * (float)M_PI / 180.0f;
    float cy     = cosf(yawRad);
    float sy     = sinf(yawRad);

    if (cmd->buttons & BTN_FWD)   { s->pos[0] += cy * v; s->pos[2] += sy * v; }
    if (cmd->buttons & BTN_BACK)  { s->pos[0] -= cy * v; s->pos[2] -= sy * v; }
    if (cmd->buttons & BTN_LEFT)  { s->pos[0] += sy * v; s->pos[2] -= cy * v; }
    if (cmd->buttons & BTN_RIGHT) { s->pos[0] -= sy * v; s->pos[2] += cy * v; }
    if (cmd->buttons & BTN_UP)    { s->pos[1] += v; }
    if (cmd->buttons & BTN_DOWN)  { s->pos[1] -= v; }

    if      (s->pos[0] >  ARENA_HALF_EXTENT) s->pos[0] =  ARENA_HALF_EXTENT;
    else if (s->pos[0] < -ARENA_HALF_EXTENT) s->pos[0] = -ARENA_HALF_EXTENT;
    if      (s->pos[2] >  ARENA_HALF_EXTENT) s->pos[2] =  ARENA_HALF_EXTENT;
    else if (s->pos[2] < -ARENA_HALF_EXTENT) s->pos[2] = -ARENA_HALF_EXTENT;
    if      (s->pos[1] >  ARENA_CEIL)        s->pos[1] =  ARENA_CEIL;
    else if (s->pos[1] <  ARENA_FLOOR)       s->pos[1] =  ARENA_FLOOR;
}
