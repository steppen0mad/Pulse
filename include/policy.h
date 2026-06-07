#ifndef PULSE_POLICY_H
#define PULSE_POLICY_H

#include <stddef.h>
#include "agent.h"

/*
 * Hand-written, dependency-free policy inference.
 *
 * No BLAS, no libtorch, no Python at deployment: a forward pass is a handful of
 * matrix-vector products (policy.c). Weights are loaded from a flat little-endian
 * policy.bin produced by training/export.py. The byte layout is a cross-track
 * contract documented in policy.c and the plan; policy_load validates every
 * field against the agent.h constants and ABORTS on any mismatch -- it never
 * returns a partially-built or zeroed policy (no silent fallback).
 */

typedef struct {
    int          in, out;
    int          act;       /* 0 = linear, 1 = tanh */
    const float *W;         /* row-major, out x in: row o is W + (size_t)o*in */
    const float *b;         /* length out */
} Layer;

typedef struct Policy {
    int    frame_skip;                 /* decision cadence baked in at export */
    Layer  hidden[POLICY_LAYERS];      /* obs -> hidden -> hidden (tanh)      */
    Layer  heads[NUM_HEADS];           /* each reads the final hidden vector  */
    float *blob;                       /* single owning allocation for all W/b */
    size_t blob_floats;
} Policy;

/* Load weights from policy.bin. Aborts the process (loudly) on any error:
 * missing file, bad magic/version, dim mismatch, short read, or trailing bytes.
 * Returns a non-NULL, fully-initialised Policy. */
Policy *policy_load(const char *path);
void    policy_free(Policy *p);

/* Run the forward pass: obs[OBS_DIM] -> logits[ACTION_LOGITS_TOTAL], concatenated
 * in HEAD_* order. */
void policy_forward(const Policy *p, const float obs[OBS_DIM],
                    float logits[ACTION_LOGITS_TOTAL]);

/* Per-head argmax over the concatenated logits -> one class index per head.
 * First-max tie-break, matching NumPy's argmax (deployment is deterministic). */
void policy_argmax_decode(const float logits[ACTION_LOGITS_TOTAL],
                          int heads[NUM_HEADS]);

#endif /* PULSE_POLICY_H */
