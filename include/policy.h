#ifndef PULSE_POLICY_H
#define PULSE_POLICY_H

#include <stddef.h>
#include "agent.h"


typedef struct {
    int          in, out;
    int          act;
    const float *W;
    const float *b;
} Layer;

typedef struct Policy {
    int    frame_skip;
    Layer  hidden[POLICY_LAYERS];
    Layer  heads[NUM_HEADS];
    float *blob;
    size_t blob_floats;
} Policy;

Policy *policy_load(const char *path);
void    policy_free(Policy *p);

void policy_forward(const Policy *p, const float obs[OBS_DIM],
                    float logits[ACTION_LOGITS_TOTAL]);

void policy_argmax_decode(const float logits[ACTION_LOGITS_TOTAL],
                          int heads[NUM_HEADS]);

#endif
