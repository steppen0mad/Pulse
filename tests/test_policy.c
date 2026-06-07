#include "policy.h"
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>

static int tests_run = 0;
#define CHECK(cond) do {                                                  \
        tests_run++;                                                      \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

#define MODEL_PATH "tests/data/policy_ref.bin"
#define IO_PATH    "tests/data/policy_ref_io.bin"
#define TOL        1e-4f

static int read_exact(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }

static int test_parity(void) {
    Policy *p = policy_load(MODEL_PATH);

    FILE *f = fopen(IO_PATH, "rb");
    CHECK(f != NULL);
    int n_samples, obs_dim, total;
    CHECK(read_exact(f, &n_samples, 4));
    CHECK(read_exact(f, &obs_dim, 4));
    CHECK(read_exact(f, &total, 4));
    CHECK(obs_dim == OBS_DIM);
    CHECK(total == ACTION_LOGITS_TOTAL);

    float *obs = malloc((size_t)n_samples * obs_dim * sizeof(float));
    float *ref = malloc((size_t)n_samples * total * sizeof(float));
    CHECK(obs && ref);
    CHECK(read_exact(f, obs, (size_t)n_samples * obs_dim * sizeof(float)));
    CHECK(read_exact(f, ref, (size_t)n_samples * total * sizeof(float)));
    fclose(f);

    float max_diff = 0.0f;
    int argmax_mismatches = 0;
    for (int s = 0; s < n_samples; s++) {
        const float *o  = obs + (size_t)s * obs_dim;
        const float *rl = ref + (size_t)s * total;
        float cl[ACTION_LOGITS_TOTAL];
        policy_forward(p, o, cl);
        for (int k = 0; k < total; k++) {
            float d = fabsf(cl[k] - rl[k]);
            if (d > max_diff) max_diff = d;
            CHECK(d <= TOL);
        }
        int ch[NUM_HEADS], rh[NUM_HEADS];
        policy_argmax_decode(cl, ch);
        policy_argmax_decode(rl, rh);
        for (int h = 0; h < NUM_HEADS; h++)
            if (ch[h] != rh[h]) argmax_mismatches++;
    }
    free(obs); free(ref);
    policy_free(p);

    CHECK(argmax_mismatches == 0);
    printf("  C forward pass matches PyTorch over %d samples "
           "(max logit diff %.2e, all argmax agree) ... ok\n", n_samples, max_diff);
    return 0;
}

static int test_bad_file_aborts(void) {
    const char *bad = "tests/data/_bad_policy.tmp";
    FILE *f = fopen(bad, "wb");
    CHECK(f != NULL);
    fwrite("PPO", 1, 3, f);
    fclose(f);

    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        if (!freopen("/dev/null", "w", stderr)) _exit(2);
        Policy *p = policy_load(bad);
        (void)p;
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    remove(bad);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) != 0);
    printf("  corrupt policy.bin aborts the loader (no silent fallback) ... ok\n");
    return 0;
}

int main(void) {
    printf("policy parity tests:\n");
    if (test_parity())          return 1;
    if (test_bad_file_aborts()) return 1;
    printf("all %d policy checks passed\n", tests_run);
    return 0;
}
