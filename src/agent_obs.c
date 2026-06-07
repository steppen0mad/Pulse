#include "agent.h"

#include <math.h>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const int   HEAD_SIZES[NUM_HEADS]   = { 3, 3, 2, 5, 3, 2 };
const float YAW_DELTA_DEG[5]        = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
const float PITCH_DELTA_DEG[3]      = { -1.0f, 0.0f, 1.0f };

#define ARENA_DIAG  (2.0f * ARENA_HALF_EXTENT * 1.41421356f)
#define EPS         1e-6f

static void to_local(float yawDeg, float dx, float dz, float *fwd, float *right) {
    float yawRad = yawDeg * (float)M_PI / 180.0f;
    float cy = cosf(yawRad), sy = sinf(yawRad);
    *fwd   =  dx * cy + dz * sy;
    *right = -dx * sy + dz * cy;
}

static void world_velocity(const World *w, int i, float v[3]) {
    float dt = (w->dt > EPS) ? w->dt : TICK_DT;
    v[0] = (w->players[i].pos[0] - w->prev_pos[i][0]) / dt;
    v[1] = (w->players[i].pos[1] - w->prev_pos[i][1]) / dt;
    v[2] = (w->players[i].pos[2] - w->prev_pos[i][2]) / dt;
}

static float ray_to_wall(const float from[3], float dx, float dz) {
    float best = ARENA_DIAG;
    if (fabsf(dx) > EPS) {
        float wall = (dx > 0.0f) ? ARENA_HALF_EXTENT : -ARENA_HALF_EXTENT;
        float t = (wall - from[0]) / dx;
        if (t >= 0.0f && t < best) best = t;
    }
    if (fabsf(dz) > EPS) {
        float wall = (dz > 0.0f) ? ARENA_HALF_EXTENT : -ARENA_HALF_EXTENT;
        float t = (wall - from[2]) / dz;
        if (t >= 0.0f && t < best) best = t;
    }
    return best;
}

void build_observation(const World *w, int agent_id, float out[OBS_DIM]) {
    assert(w && agent_id >= 0 && agent_id < MAX_CLIENTS && w->present[agent_id]);

    const PlayerState *self = &w->players[agent_id];
    float yaw = self->yaw;
    float yawRad = yaw * (float)M_PI / 180.0f;
    float cy = cosf(yawRad), sy = sinf(yawRad);

    float self_vel[3];
    world_velocity(w, agent_id, self_vel);

    int o = 0;

    float v_right = -self_vel[0] * sy + self_vel[2] * cy;
    float v_fwd   =  self_vel[0] * cy + self_vel[2] * sy;
    out[o++] = v_right / MOVE_SPEED;
    out[o++] = self_vel[1] / MOVE_SPEED;
    out[o++] = v_fwd / MOVE_SPEED;
    out[o++] = (self->pos[1] <= ARENA_FLOOR + EYE_HEIGHT + 0.01f) ? 1.0f : 0.0f;
    out[o++] = self->pitch / PITCH_LIMIT;

    int   chosen[K_NEAREST];
    float chosen_d[K_NEAREST];
    int   n_chosen = 0;
    for (int k = 0; k < K_NEAREST; k++) { chosen[k] = -1; chosen_d[k] = 0.0f; }

    for (int j = 0; j < MAX_CLIENTS; j++) {
        if (j == agent_id || !w->present[j]) continue;
        float dx = w->players[j].pos[0] - self->pos[0];
        float dy = w->players[j].pos[1] - self->pos[1];
        float dz = w->players[j].pos[2] - self->pos[2];
        float d  = sqrtf(dx*dx + dy*dy + dz*dz);
        if (n_chosen < K_NEAREST) {
            int p = n_chosen++;
            while (p > 0 && chosen_d[p-1] > d) { chosen_d[p] = chosen_d[p-1]; chosen[p] = chosen[p-1]; p--; }
            chosen_d[p] = d; chosen[p] = j;
        } else if (d < chosen_d[K_NEAREST-1]) {
            int p = K_NEAREST - 1;
            while (p > 0 && chosen_d[p-1] > d) { chosen_d[p] = chosen_d[p-1]; chosen[p] = chosen[p-1]; p--; }
            chosen_d[p] = d; chosen[p] = j;
        }
    }

    for (int k = 0; k < K_NEAREST; k++) {
        if (k >= n_chosen || chosen[k] < 0) {
            for (int f = 0; f < OBS_PER_OTHER; f++) out[o++] = 0.0f;
            continue;
        }
        int j = chosen[k];
        float dx = w->players[j].pos[0] - self->pos[0];
        float dy = w->players[j].pos[1] - self->pos[1];
        float dz = w->players[j].pos[2] - self->pos[2];
        float range3 = chosen_d[k];

        float fwd, right;
        to_local(yaw, dx, dz, &fwd, &right);
        float rh = sqrtf(fwd*fwd + right*right);
        float sinB = (rh > EPS) ? right / rh : 0.0f;
        float cosB = (rh > EPS) ? fwd  / rh : 1.0f;
        float elev = (range3 > EPS) ? dy / range3 : 0.0f;

        float ov[3];
        world_velocity(w, j, ov);
        float rvx_w = ov[0] - self_vel[0];
        float rvz_w = ov[2] - self_vel[2];
        float rv_right = -rvx_w * sy + rvz_w * cy;
        float rv_fwd   =  rvx_w * cy + rvz_w * sy;

        float rng = range3 / ARENA_DIAG; if (rng > 1.0f) rng = 1.0f;
        out[o++] = rng;
        out[o++] = sinB;
        out[o++] = cosB;
        out[o++] = elev;
        out[o++] = rv_right / MOVE_SPEED;
        out[o++] = rv_fwd   / MOVE_SPEED;
        out[o++] = (w->team[j] != w->team[agent_id]) ? 1.0f : 0.0f;
        out[o++] = 1.0f;
    }

    for (int r = 0; r < OBS_RAYS; r++) {
        float ang = yawRad + (float)r * (2.0f * (float)M_PI / (float)OBS_RAYS);
        float dist = ray_to_wall(self->pos, cosf(ang), sinf(ang));
        float n = dist / ARENA_DIAG;
        if (n < 0.0f) n = 0.0f; else if (n > 1.0f) n = 1.0f;
        out[o++] = n;
    }

    {
        float dx = w->target[agent_id][0] - self->pos[0];
        float dy = w->target[agent_id][1] - self->pos[1];
        float dz = w->target[agent_id][2] - self->pos[2];
        float range3 = sqrtf(dx*dx + dy*dy + dz*dz);
        float fwd, right;
        to_local(yaw, dx, dz, &fwd, &right);
        float rh = sqrtf(fwd*fwd + right*right);
        float rng = range3 / ARENA_DIAG; if (rng > 1.0f) rng = 1.0f;
        out[o++] = rng;
        out[o++] = (rh > EPS) ? right / rh : 0.0f;
        out[o++] = (rh > EPS) ? fwd  / rh : 1.0f;
    }

    assert(o == OBS_DIM);
}

void agent_decode_action(const int heads[NUM_HEADS],
                         float *yaw, float *pitch, InputCmd *out) {
    int mx   = heads[HEAD_MOVE_X];
    int mz   = heads[HEAD_MOVE_Z];
    int jump = heads[HEAD_JUMP];
    int yb   = heads[HEAD_YAW];
    int pb   = heads[HEAD_PITCH];
    int fire = heads[HEAD_FIRE];

    assert(mx >= 0 && mx < 3 && mz >= 0 && mz < 3 && jump >= 0 && jump < 2);
    assert(yb >= 0 && yb < 5 && pb >= 0 && pb < 3 && fire >= 0 && fire < 2);

    uint8_t buttons = 0;
    if (mx == 0) buttons |= BTN_LEFT;    else if (mx == 2) buttons |= BTN_RIGHT;
    if (mz == 0) buttons |= BTN_BACK;    else if (mz == 2) buttons |= BTN_FWD;
    if (jump)    buttons |= BTN_UP;
    if (fire)    buttons |= BTN_FIRE;

    float ny = *yaw + YAW_DELTA_DEG[yb];
    while (ny >= 180.0f) ny -= 360.0f;
    while (ny < -180.0f) ny += 360.0f;
    *yaw = ny;

    float np = *pitch + PITCH_DELTA_DEG[pb];
    if (np >  PITCH_LIMIT) np =  PITCH_LIMIT;
    if (np < -PITCH_LIMIT) np = -PITCH_LIMIT;
    *pitch = np;

    out->seq     = 0;
    out->buttons = buttons;
    out->yaw     = *yaw;
    out->pitch   = *pitch;
}
