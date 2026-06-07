/*
 * Server-side agent controller.
 *
 * Each agent occupies an ordinary player slot. On a decision tick it builds the
 * shared observation, picks an action (policy argmax, or a deterministic stub in
 * Phase-0 / test modes), and decodes it into an InputCmd via the shared
 * agent_decode_action. Between decision ticks it holds the last action -- this
 * frame-skip cadence MUST match training, so the policy sees the same effective
 * control rate it was trained at.
 *
 * This file does no networking and never calls world_apply_input: the server
 * owns the single apply path, exactly as it does for human input.
 */
#include "agent.h"
#include "policy.h"

#include <string.h>
#include <assert.h>

/* tiny deterministic PRNG so stub agents are reproducible from a fixed seed */
static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

void agent_init(Agent *a, AgentMode mode, struct Policy *policy,
                int frame_skip, float yaw0, float pitch0, uint32_t seed) {
    assert(a && frame_skip >= 1);
    memset(a, 0, sizeof(*a));
    a->mode       = mode;
    a->policy     = policy;
    a->frame_skip = frame_skip;
    /* start ready to decide on the very first tick */
    a->ticks_since_decision = frame_skip;
    a->yaw   = yaw0;
    a->pitch = pitch0;
    a->rng   = seed ? seed : 0x9e3779b9u;

    a->held.seq     = 0;
    a->held.buttons = 0;
    a->held.yaw     = yaw0;
    a->held.pitch   = pitch0;
}

void agent_think(Agent *a, const World *w, int id, InputCmd *out) {
    if (a->ticks_since_decision >= a->frame_skip) {
        a->ticks_since_decision = 0;

        int heads[NUM_HEADS];
        switch (a->mode) {
            case AGENT_POLICY: {
                assert(a->policy);
                float obs[OBS_DIM];
                float logits[ACTION_LOGITS_TOTAL];
                build_observation(w, id, obs);
                policy_forward(a->policy, obs, logits);
                policy_argmax_decode(logits, heads);
                break;
            }
            case AGENT_STUB_RANDOM:
                for (int h = 0; h < NUM_HEADS; h++)
                    heads[h] = (int)(xorshift32(&a->rng) % (uint32_t)HEAD_SIZES[h]);
                break;
            case AGENT_STUB_FIXED:
            default: {
                /* walk straight forward, no turn, no fire */
                static const int fixed[NUM_HEADS] = { 1, 2, 0, 2, 1, 0 };
                memcpy(heads, fixed, sizeof fixed);
                break;
            }
        }

        agent_decode_action(heads, &a->yaw, &a->pitch, &a->held);
    }

    a->ticks_since_decision++;
    *out = a->held;
}
