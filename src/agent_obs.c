/*
 * Shared perception + action decode for the AI agent layer.
 *
 * This translation unit is compiled into BOTH the live server and the Python
 * training shared library (via cffi). Everything here must therefore be
 * deterministic and dependency-free (only <math.h>): if the observation seen in
 * training differs in any way from the observation seen at deployment, the
 * policy silently breaks. Keeping the obs builder and the action decoder in one
 * shared file is the same discipline world.c already uses for the simulation.
 */
#include "agent.h"

#include <math.h>
#include <assert.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- contract tables (single definition; declared extern in agent.h) ---- */
const int   HEAD_SIZES[NUM_HEADS]   = { 3, 3, 2, 5, 3, 2 };
const float YAW_DELTA_DEG[5]        = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
const float PITCH_DELTA_DEG[3]      = { -1.0f, 0.0f, 1.0f };

/* Normalisers. The arena's horizontal diagonal bounds any in-box separation. */
#define ARENA_DIAG  (2.0f * ARENA_HALF_EXTENT * 1.41421356f)   /* ~= 56.57 */
#define EPS         1e-6f

/* Rotate a world-space XZ vector into the agent's egocentric frame.
 *   forward axis = (cos yaw, sin yaw)   -- the BTN_FWD travel direction
 *   right   axis = (-sin yaw, cos yaw)  -- the BTN_RIGHT travel direction
 * so +fwd is straight ahead and +right is to the agent's right. */
static void to_local(float yawDeg, float dx, float dz, float *fwd, float *right) {
    float yawRad = yawDeg * (float)M_PI / 180.0f;
    float cy = cosf(yawRad), sy = sinf(yawRad);
    *fwd   =  dx * cy + dz * sy;
    *right = -dx * sy + dz * cy;
}

/* Velocity of slot i in world space, derived the one and only way. */
static void world_velocity(const World *w, int i, float v[3]) {
    float dt = (w->dt > EPS) ? w->dt : TICK_DT;
    v[0] = (w->players[i].pos[0] - w->prev_pos[i][0]) / dt;
    v[1] = (w->players[i].pos[1] - w->prev_pos[i][1]) / dt;
    v[2] = (w->players[i].pos[2] - w->prev_pos[i][2]) / dt;
}

/* Distance from `from` along unit XZ direction (dx,dz) to the arena wall. The
 * arena is a convex box, so this is the nearest positive axis-plane crossing. */
static float ray_to_wall(const float from[3], float dx, float dz) {
    float best = ARENA_DIAG;   /* default: full diagonal if direction is degenerate */
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

    /* ---- Self (5) ---- */
    /* local-frame velocity: vx=right, vy=vertical, vz=forward; norm by speed. */
    float v_right = -self_vel[0] * sy + self_vel[2] * cy;
    float v_fwd   =  self_vel[0] * cy + self_vel[2] * sy;
    out[o++] = v_right / MOVE_SPEED;
    out[o++] = self_vel[1] / MOVE_SPEED;
    out[o++] = v_fwd / MOVE_SPEED;
    out[o++] = (self->pos[1] <= ARENA_FLOOR + EYE_HEIGHT + 0.01f) ? 1.0f : 0.0f;
    out[o++] = self->pitch / PITCH_LIMIT;

    /* ---- K nearest other players (OBS_PER_OTHER each) ---- */
    /* Select the K smallest 3D ranges among present others (K and N are tiny). */
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
        /* insertion into the K-nearest list */
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
            for (int f = 0; f < OBS_PER_OTHER; f++) out[o++] = 0.0f;   /* zero-pad absent */
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
        out[o++] = 1.0f;   /* line-of-sight: convex empty arena -> always visible */
    }

    /* ---- Environment rays (OBS_RAYS): fan around the facing direction ---- */
    for (int r = 0; r < OBS_RAYS; r++) {
        float ang = yawRad + (float)r * (2.0f * (float)M_PI / (float)OBS_RAYS);
        float dist = ray_to_wall(self->pos, cosf(ang), sinf(ang));
        float n = dist / ARENA_DIAG;
        if (n < 0.0f) n = 0.0f; else if (n > 1.0f) n = 1.0f;
        out[o++] = n;
    }

    /* ---- Task (3): range + bearing to this agent's target ---- */
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
    int mx   = heads[HEAD_MOVE_X];   /* 0..2 -> -1,0,+1 */
    int mz   = heads[HEAD_MOVE_Z];
    int jump = heads[HEAD_JUMP];
    int yb   = heads[HEAD_YAW];
    int pb   = heads[HEAD_PITCH];
    int fire = heads[HEAD_FIRE];

    /* indices come from a per-head argmax, so they must be in range; assert
     * loudly rather than silently masking a bug. */
    assert(mx >= 0 && mx < 3 && mz >= 0 && mz < 3 && jump >= 0 && jump < 2);
    assert(yb >= 0 && yb < 5 && pb >= 0 && pb < 3 && fire >= 0 && fire < 2);

    uint8_t buttons = 0;
    if (mx == 0) buttons |= BTN_LEFT;    else if (mx == 2) buttons |= BTN_RIGHT;
    if (mz == 0) buttons |= BTN_BACK;    else if (mz == 2) buttons |= BTN_FWD;
    if (jump)    buttons |= BTN_UP;
    if (fire)    buttons |= BTN_FIRE;

    float ny = *yaw + YAW_DELTA_DEG[yb];
    while (ny >= 180.0f) ny -= 360.0f;   /* keep bounded (cos/sin make wrap harmless) */
    while (ny < -180.0f) ny += 360.0f;
    *yaw = ny;

    float np = *pitch + PITCH_DELTA_DEG[pb];
    if (np >  PITCH_LIMIT) np =  PITCH_LIMIT;
    if (np < -PITCH_LIMIT) np = -PITCH_LIMIT;
    *pitch = np;

    out->seq     = 0;            /* agents do not use input seqs */
    out->buttons = buttons;
    out->yaw     = *yaw;
    out->pitch   = *pitch;
}
