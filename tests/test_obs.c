/*
 * Unit tests for the shared observation builder and action decoder
 * (src/agent_obs.c). These run the exact C code the live server and the training
 * environment both use, so they pin the egocentric transforms, the zero-padding
 * of absent players, the value ranges, and the action decode. No sockets, no
 * display -- safe for CI; any failed assertion exits non-zero.
 */
#include "agent.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

static int tests_run = 0;
#define CHECK(cond) do {                                                  \
        tests_run++;                                                      \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

#define TOL 1e-4f
static int approx(float a, float b) { return fabsf(a - b) <= TOL; }
#define CHECK_NEAR(a, b) CHECK(approx((a), (b)))

/* arena diagonal used by the builder for range normalisation */
static const float DIAG = 2.0f * ARENA_HALF_EXTENT * 1.41421356f;

/* offsets into the obs vector */
#define OFF_OTHERS  OBS_SELF                       /* 5  */
#define OFF_RAYS    (OBS_SELF + K_NEAREST*OBS_PER_OTHER)  /* 29 */
#define OFF_TASK    (OFF_RAYS + OBS_RAYS)          /* 37 */

static void zero_world(World *w) {
    memset(w, 0, sizeof(*w));
    w->dt = TICK_DT;
}

/* place a static (zero-velocity) player: prev_pos == pos */
static void put_static(World *w, int i, float x, float y, float z, int team) {
    w->present[i]      = 1;
    w->team[i]         = team;
    w->players[i].pos[0] = x; w->players[i].pos[1] = y; w->players[i].pos[2] = z;
    w->prev_pos[i][0]  = x; w->prev_pos[i][1]  = y; w->prev_pos[i][2]  = z;
    w->players[i].yaw = 0.0f; w->players[i].pitch = 0.0f;
}

static int test_self_and_velocity(void) {
    World w; zero_world(&w);
    put_static(&w, 0, 0.0f, EYE_HEIGHT, 0.0f, 0);
    /* move +X by 0.1 over one tick with yaw 0 (forward axis = +X) */
    w.prev_pos[0][0] = -0.1f;                  /* pos - prev = +0.1 */
    w.players[0].pitch = 44.5f;                /* half of the +/-89 range */
    w.target[0][0] = 1.0f; w.target[0][1] = EYE_HEIGHT; /* keep target valid */

    float obs[OBS_DIM];
    build_observation(&w, 0, obs);

    float v = 0.1f / TICK_DT;                  /* world +X speed */
    CHECK_NEAR(obs[0], 0.0f);                  /* right component = 0 */
    CHECK_NEAR(obs[1], 0.0f);                  /* vertical = 0        */
    CHECK_NEAR(obs[2], (v / MOVE_SPEED));      /* forward component   */
    CHECK_NEAR(obs[3], 1.0f);                  /* on_ground at eye height */
    CHECK_NEAR(obs[4], 44.5f / PITCH_LIMIT);   /* normalized pitch    */
    printf("  self: local velocity / on_ground / pitch ... ok\n");
    return 0;
}

static int test_other_bearing(void) {
    World w; zero_world(&w);
    put_static(&w, 0, 0.0f, EYE_HEIGHT, 0.0f, 0);
    w.target[0][1] = EYE_HEIGHT;

    /* one enemy 5 units straight ahead (+X, since yaw 0 faces +X) */
    put_static(&w, 1, 5.0f, EYE_HEIGHT, 0.0f, 1 /* other team */);
    float obs[OBS_DIM];
    build_observation(&w, 0, obs);

    CHECK_NEAR(obs[OFF_OTHERS + 0], 5.0f / DIAG);   /* range            */
    CHECK_NEAR(obs[OFF_OTHERS + 1], 0.0f);          /* sinB: dead ahead */
    CHECK_NEAR(obs[OFF_OTHERS + 2], 1.0f);          /* cosB             */
    CHECK_NEAR(obs[OFF_OTHERS + 3], 0.0f);          /* elevation        */
    CHECK_NEAR(obs[OFF_OTHERS + 6], 1.0f);          /* is_enemy         */
    CHECK_NEAR(obs[OFF_OTHERS + 7], 1.0f);          /* line-of-sight    */

    /* to the agent's right (+Z) -> sinB = +1; to the left (-Z) -> -1 */
    World wr; zero_world(&wr);
    put_static(&wr, 0, 0.0f, EYE_HEIGHT, 0.0f, 0); wr.target[0][1] = EYE_HEIGHT;
    put_static(&wr, 1, 0.0f, EYE_HEIGHT, 5.0f, 0);
    build_observation(&wr, 0, obs);
    CHECK_NEAR(obs[OFF_OTHERS + 1], 1.0f);
    CHECK_NEAR(obs[OFF_OTHERS + 2], 0.0f);

    World wl; zero_world(&wl);
    put_static(&wl, 0, 0.0f, EYE_HEIGHT, 0.0f, 0); wl.target[0][1] = EYE_HEIGHT;
    put_static(&wl, 1, 0.0f, EYE_HEIGHT, -5.0f, 0);
    build_observation(&wl, 0, obs);
    CHECK_NEAR(obs[OFF_OTHERS + 1], -1.0f);
    printf("  other players: egocentric bearing (ahead/left/right) ... ok\n");
    return 0;
}

static int test_padding_and_nearest(void) {
    World w; zero_world(&w);
    put_static(&w, 0, 0.0f, EYE_HEIGHT, 0.0f, 0); w.target[0][1] = EYE_HEIGHT;
    /* no other players: all K other-slots must be exactly zero */
    float obs[OBS_DIM];
    build_observation(&w, 0, obs);
    for (int f = 0; f < K_NEAREST * OBS_PER_OTHER; f++)
        CHECK(obs[OFF_OTHERS + f] == 0.0f);

    /* two players: a far one and a near one -> the NEAR one fills slot 0 */
    put_static(&w, 3, 15.0f, EYE_HEIGHT, 0.0f, 0);   /* far  */
    put_static(&w, 5, 2.0f,  EYE_HEIGHT, 0.0f, 0);   /* near */
    build_observation(&w, 0, obs);
    CHECK_NEAR(obs[OFF_OTHERS + 0], 2.0f / DIAG);    /* nearest first */
    CHECK_NEAR(obs[OFF_OTHERS + OBS_PER_OTHER + 0], 15.0f / DIAG);
    /* third slot still zero-padded */
    for (int f = 0; f < OBS_PER_OTHER; f++)
        CHECK(obs[OFF_OTHERS + 2 * OBS_PER_OTHER + f] == 0.0f);
    printf("  K-nearest selection + zero-padding of absent players ... ok\n");
    return 0;
}

static int test_rays_and_task(void) {
    World w; zero_world(&w);
    put_static(&w, 0, 0.0f, EYE_HEIGHT, 0.0f, 0);    /* arena centre, yaw 0 */
    w.target[0][0] = 10.0f; w.target[0][1] = EYE_HEIGHT; w.target[0][2] = 0.0f;

    float obs[OBS_DIM];
    build_observation(&w, 0, obs);

    /* ray 0 points along facing (+X): wall at +20, distance 20 */
    CHECK_NEAR(obs[OFF_RAYS + 0], 20.0f / DIAG);
    /* ray 1 at +45 deg hits the corner: distance 20*sqrt(2) */
    CHECK_NEAR(obs[OFF_RAYS + 1], (20.0f * 1.41421356f) / DIAG);

    /* task: target 10 ahead -> range, sinB=0, cosB=1 */
    CHECK_NEAR(obs[OFF_TASK + 0], 10.0f / DIAG);
    CHECK_NEAR(obs[OFF_TASK + 1], 0.0f);
    CHECK_NEAR(obs[OFF_TASK + 2], 1.0f);
    printf("  environment rays + task encoding ... ok\n");
    return 0;
}

static int test_value_ranges(void) {
    /* a busy world: several players, off-centre, looking around */
    World w; zero_world(&w);
    put_static(&w, 0, 7.0f, EYE_HEIGHT, -3.0f, 0);
    w.players[0].yaw = 37.0f; w.players[0].pitch = -20.0f;
    w.prev_pos[0][0] = 6.8f;                       /* some velocity */
    w.target[0][0] = -12.0f; w.target[0][1] = EYE_HEIGHT; w.target[0][2] = 8.0f;
    put_static(&w, 1, -5.0f, EYE_HEIGHT + 1.0f, 4.0f, 1);
    put_static(&w, 2, 10.0f, EYE_HEIGHT, 10.0f, 0);

    float obs[OBS_DIM];
    build_observation(&w, 0, obs);
    for (int i = 0; i < OBS_DIM; i++) CHECK(isfinite(obs[i]));

    /* ranges and rays are normalised into [0,1]; bearings into [-1,1] */
    CHECK(obs[OFF_TASK + 0] >= 0.0f && obs[OFF_TASK + 0] <= 1.0f);
    for (int r = 0; r < OBS_RAYS; r++)
        CHECK(obs[OFF_RAYS + r] >= 0.0f && obs[OFF_RAYS + r] <= 1.0001f);
    for (int k = 0; k < K_NEAREST; k++) {
        float rng  = obs[OFF_OTHERS + k * OBS_PER_OTHER + 0];
        float sinB = obs[OFF_OTHERS + k * OBS_PER_OTHER + 1];
        float cosB = obs[OFF_OTHERS + k * OBS_PER_OTHER + 2];
        CHECK(rng >= 0.0f && rng <= 1.0f);
        CHECK(sinB >= -1.0001f && sinB <= 1.0001f);
        CHECK(cosB >= -1.0001f && cosB <= 1.0001f);
    }
    printf("  all components finite and within documented ranges ... ok\n");
    return 0;
}

static int test_decode(void) {
    InputCmd cmd;
    float yaw, pitch;

    /* strafe right + forward, no turn, no fire */
    int a[NUM_HEADS] = { 2, 2, 0, 2, 1, 0 };   /* yaw bin 2 = 0deg, pitch bin 1 = 0deg */
    yaw = 0.0f; pitch = 0.0f;
    agent_decode_action(a, &yaw, &pitch, &cmd);
    CHECK(cmd.buttons == (BTN_RIGHT | BTN_FWD));
    CHECK_NEAR(yaw, 0.0f);
    CHECK_NEAR(pitch, 0.0f);
    CHECK(cmd.seq == 0);

    /* strafe left + back + jump + fire */
    int b[NUM_HEADS] = { 0, 0, 1, 2, 1, 1 };
    yaw = 0.0f; pitch = 0.0f;
    agent_decode_action(b, &yaw, &pitch, &cmd);
    CHECK(cmd.buttons == (BTN_LEFT | BTN_BACK | BTN_UP | BTN_FIRE));

    /* yaw delta +2 and -2 */
    int yp[NUM_HEADS] = { 1, 1, 0, 4, 1, 0 };  /* yaw bin 4 = +2deg */
    yaw = 10.0f; pitch = 0.0f;
    agent_decode_action(yp, &yaw, &pitch, &cmd);
    CHECK_NEAR(yaw, 12.0f);

    /* yaw wraps across +/-180 */
    int yw[NUM_HEADS] = { 1, 1, 0, 4, 1, 0 };
    yaw = 179.0f; pitch = 0.0f;
    agent_decode_action(yw, &yaw, &pitch, &cmd);
    CHECK_NEAR(yaw, -179.0f);

    /* pitch clamps at +/-89 */
    int pc[NUM_HEADS] = { 1, 1, 0, 2, 2, 0 };  /* pitch bin 2 = +1deg */
    yaw = 0.0f; pitch = 89.0f;
    agent_decode_action(pc, &yaw, &pitch, &cmd);
    CHECK_NEAR(pitch, PITCH_LIMIT);
    CHECK(cmd.yaw == yaw && cmd.pitch == pitch);
    printf("  action decode: buttons, yaw delta+wrap, pitch clamp ... ok\n");
    return 0;
}

int main(void) {
    printf("observation + decode tests:\n");
    if (test_self_and_velocity())  return 1;
    if (test_other_bearing())      return 1;
    if (test_padding_and_nearest())return 1;
    if (test_rays_and_task())      return 1;
    if (test_value_ranges())       return 1;
    if (test_decode())             return 1;
    printf("all %d observation/decode checks passed\n", tests_run);
    return 0;
}
