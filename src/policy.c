#include "policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static void die(const char *path, const char *why) {
    fprintf(stderr, "[policy] %s: %s\n", path, why);
    exit(1);
}

static uint32_t rd_u32le(FILE *f, const char *path) {
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4) die(path, "unexpected end of file in header");
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static int rd_i32le(FILE *f, const char *path) {
    return (int)rd_u32le(f, path);
}

static void dense(const Layer *L, const float *x, float *y) {
    for (int o = 0; o < L->out; o++) {
        const float *w = L->W + (size_t)o * L->in;
        float acc = L->b[o];
        for (int i = 0; i < L->in; i++) acc += w[i] * x[i];
        y[o] = (L->act == 1) ? tanhf(acc) : acc;
    }
}

Policy *policy_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die(path, "cannot open weights file");

    unsigned char magic[4];
    if (fread(magic, 1, 4, f) != 4) die(path, "truncated (no magic)");
    if (memcmp(magic, "PPO1", 4) != 0) die(path, "bad magic (not a policy.bin)");

    int version    = rd_i32le(f, path);
    int obs_dim    = rd_i32le(f, path);
    int n_hidden   = rd_i32le(f, path);
    int hidden_dim = rd_i32le(f, path);
    int num_heads  = rd_i32le(f, path);
    if (version    != 1)              die(path, "unsupported version");
    if (obs_dim    != OBS_DIM)        die(path, "obs_dim mismatch vs agent.h");
    if (n_hidden   != POLICY_LAYERS)  die(path, "n_hidden mismatch vs agent.h");
    if (hidden_dim != POLICY_HIDDEN)  die(path, "hidden_dim mismatch vs agent.h");
    if (num_heads  != NUM_HEADS)      die(path, "num_heads mismatch vs agent.h");

    int head_sizes[NUM_HEADS];
    for (int h = 0; h < NUM_HEADS; h++) {
        head_sizes[h] = rd_i32le(f, path);
        if (head_sizes[h] != HEAD_SIZES[h]) die(path, "head_sizes mismatch vs agent.h");
    }

    int frame_skip = rd_i32le(f, path);
    if (frame_skip < 1) die(path, "frame_skip must be >= 1");
    uint32_t obs_ver = rd_u32le(f, path);
    if ((int)obs_ver != OBS_VERSION) die(path, "obs layout version mismatch (stale policy)");

    size_t total = 0;
    total += (size_t)hidden_dim * obs_dim    + hidden_dim;
    total += (size_t)hidden_dim * hidden_dim + hidden_dim;
    for (int h = 0; h < NUM_HEADS; h++)
        total += (size_t)head_sizes[h] * hidden_dim + head_sizes[h];

    float *blob = (float *)malloc(total * sizeof(float));
    if (!blob) die(path, "out of memory");
    if (fread(blob, sizeof(float), total, f) != total) die(path, "truncated weight data");

    unsigned char extra;
    if (fread(&extra, 1, 1, f) != 0) die(path, "trailing bytes after weights");
    fclose(f);

    Policy *p = (Policy *)calloc(1, sizeof(Policy));
    if (!p) die(path, "out of memory");
    p->blob        = blob;
    p->blob_floats = total;
    p->frame_skip  = frame_skip;

    size_t off = 0;
    int dims_in[POLICY_LAYERS] = { obs_dim, hidden_dim };
    for (int l = 0; l < POLICY_LAYERS; l++) {
        p->hidden[l].in  = dims_in[l];
        p->hidden[l].out = hidden_dim;
        p->hidden[l].act = 1;
        p->hidden[l].W   = blob + off; off += (size_t)hidden_dim * dims_in[l];
        p->hidden[l].b   = blob + off; off += hidden_dim;
    }
    for (int h = 0; h < NUM_HEADS; h++) {
        p->heads[h].in  = hidden_dim;
        p->heads[h].out = head_sizes[h];
        p->heads[h].act = 0;
        p->heads[h].W   = blob + off; off += (size_t)head_sizes[h] * hidden_dim;
        p->heads[h].b   = blob + off; off += head_sizes[h];
    }
    if (off != total) die(path, "internal layout accounting error");

    return p;
}

void policy_free(Policy *p) {
    if (!p) return;
    free(p->blob);
    free(p);
}

void policy_forward(const Policy *p, const float obs[OBS_DIM],
                    float logits[ACTION_LOGITS_TOTAL]) {
    float bufs[POLICY_LAYERS][POLICY_HIDDEN];
    const float *x = obs;
    for (int l = 0; l < POLICY_LAYERS; l++) {
        dense(&p->hidden[l], x, bufs[l]);
        x = bufs[l];
    }
    int off = 0;
    for (int h = 0; h < NUM_HEADS; h++) {
        dense(&p->heads[h], x, logits + off);
        off += p->heads[h].out;
    }
}

void policy_argmax_decode(const float logits[ACTION_LOGITS_TOTAL],
                          int heads[NUM_HEADS]) {
    int off = 0;
    for (int h = 0; h < NUM_HEADS; h++) {
        int   best_i = 0;
        float best_v = logits[off];
        for (int c = 1; c < HEAD_SIZES[h]; c++) {
            if (logits[off + c] > best_v) { best_v = logits[off + c]; best_i = c; }
        }
        heads[h] = best_i;
        off += HEAD_SIZES[h];
    }
}
