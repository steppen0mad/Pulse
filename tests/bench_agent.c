#include "agent.h"
#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint32_t rng = 0x1234567u;
static float frand_unit(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return ((float)(rng >> 8) / (float)(1u << 24)) * 2.0f - 1.0f;
}

static Policy *make_random_policy(void) {
    size_t total = 0;
    total += (size_t)POLICY_HIDDEN * OBS_DIM      + POLICY_HIDDEN;
    total += (size_t)POLICY_HIDDEN * POLICY_HIDDEN + POLICY_HIDDEN;
    for (int h = 0; h < NUM_HEADS; h++)
        total += (size_t)HEAD_SIZES[h] * POLICY_HIDDEN + HEAD_SIZES[h];

    float *blob = (float *)malloc(total * sizeof(float));
    for (size_t i = 0; i < total; i++) blob[i] = 0.1f * frand_unit();

    Policy *p = (Policy *)calloc(1, sizeof(Policy));
    p->blob = blob; p->blob_floats = total; p->frame_skip = 4;
    size_t off = 0;
    int dims_in[POLICY_LAYERS] = { OBS_DIM, POLICY_HIDDEN };
    for (int l = 0; l < POLICY_LAYERS; l++) {
        p->hidden[l].in = dims_in[l]; p->hidden[l].out = POLICY_HIDDEN; p->hidden[l].act = 1;
        p->hidden[l].W = blob + off; off += (size_t)POLICY_HIDDEN * dims_in[l];
        p->hidden[l].b = blob + off; off += POLICY_HIDDEN;
    }
    for (int h = 0; h < NUM_HEADS; h++) {
        p->heads[h].in = POLICY_HIDDEN; p->heads[h].out = HEAD_SIZES[h]; p->heads[h].act = 0;
        p->heads[h].W = blob + off; off += (size_t)HEAD_SIZES[h] * POLICY_HIDDEN;
        p->heads[h].b = blob + off; off += HEAD_SIZES[h];
    }
    return p;
}

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(void) {
    World w; memset(&w, 0, sizeof w);
    w.dt = TICK_DT;
    for (int i = 0; i < 8; i++) {
        w.present[i] = 1;
        w.players[i].pos[0] = frand_unit() * ARENA_HALF_EXTENT;
        w.players[i].pos[1] = EYE_HEIGHT;
        w.players[i].pos[2] = frand_unit() * ARENA_HALF_EXTENT;
        w.players[i].yaw    = frand_unit() * 180.0f;
        memcpy(w.prev_pos[i], w.players[i].pos, sizeof(float) * 3);
        w.team[i] = i & 1;
    }
    w.target[0][0] = 5.0f; w.target[0][1] = EYE_HEIGHT;

    Policy *p = make_random_policy();

    float obs[OBS_DIM], logits[ACTION_LOGITS_TOTAL];
    int   heads[NUM_HEADS];
    volatile int sink = 0;

    const int BATCHES = 11;
    const int ITERS   = 200000;
    double per_agent_ns[BATCHES];

    for (int bi = 0; bi < BATCHES; bi++) {
        double t0 = now_ns();
        for (int it = 0; it < ITERS; it++) {
            build_observation(&w, 0, obs);
            policy_forward(p, obs, logits);
            policy_argmax_decode(logits, heads);
            sink += heads[0];
        }
        double t1 = now_ns();
        per_agent_ns[bi] = (t1 - t0) / (double)ITERS;
    }
    (void)sink;

    qsort(per_agent_ns, BATCHES, sizeof(double), cmp_double);
    double median_ns = per_agent_ns[BATCHES / 2];
    double per_agent_us = median_ns / 1000.0;
    double tick_budget_us = 1000000.0 / (double)TICK_RATE;
    int    fit = (int)(tick_budget_us / per_agent_us);

    printf("agent decision microbenchmark (build_obs + forward + decode):\n");
    printf("  median per-agent cost : %.3f us  (%.0f ns)\n", per_agent_us, median_ns);
    printf("  tick budget           : %.1f us  (%d Hz)\n", tick_budget_us, TICK_RATE);
    printf("  agents per tick (1x)  : ~%d at 100%% budget; ~%d at 10%% budget\n",
           fit, fit / 10);

    policy_free(p);

    if (per_agent_us > 50.0) {
        fprintf(stderr, "FAIL: per-agent cost %.3f us exceeds 50 us ceiling\n", per_agent_us);
        return 1;
    }
    return 0;
}
