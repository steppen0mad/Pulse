#ifndef PULSE_AGENT_H
#define PULSE_AGENT_H

#include <stdint.h>
#include "protocol.h"
#include "world.h"

/*
 * The AI agent layer.
 *
 * An agent is a server-side controller that occupies an ordinary player slot.
 * Every (decision) tick the server builds a fixed-size egocentric observation
 * from authoritative world state, runs a tiny policy network, decodes the
 * multi-discrete action into the SAME InputCmd a human produces, and applies it
 * through the SAME world_apply_input. So the wire protocol, snapshots, and the
 * entire client are untouched -- a human sees an agent as just another player.
 *
 * The two functions that MUST be bit-identical between training and deployment
 * -- build_observation() and agent_decode_action() -- live in src/agent_obs.c,
 * which is compiled into BOTH the live server and the Python training shared
 * library (via cffi). This header is their single source of truth for layout.
 */

/* ------------------------------------------------------------------ *
 *  Observation layout (egocentric, fixed size). OBS_DIM == 40.
 *  Bump OBS_VERSION whenever any of this changes; a policy.bin carrying a
 *  different version is rejected at load (no silent train/deploy skew).
 * ------------------------------------------------------------------ */
#define OBS_VERSION    1

#define K_NEAREST      3      /* nearest other players encoded in the obs            */
#define OBS_SELF       5      /* local vel (vx,vy,vz), on_ground, normalized pitch   */
#define OBS_PER_OTHER  8      /* range, sinB, cosB, elev, rvx, rvy, is_enemy, los    */
#define OBS_RAYS       8      /* fan of normalized ray-vs-wall distances             */
#define OBS_TASK       3      /* target range, sinB, cosB                            */
#define OBS_DIM (OBS_SELF + K_NEAREST * OBS_PER_OTHER + OBS_RAYS + OBS_TASK)  /* 40 */

/* ------------------------------------------------------------------ *
 *  Action layout (multi-discrete). The head ORDER is the export contract
 *  shared with training/export.py and the policy.bin header.
 * ------------------------------------------------------------------ */
enum {
    HEAD_MOVE_X = 0,   /* {-1,0,+1} : strafe left / none / right                 */
    HEAD_MOVE_Z,       /* {-1,0,+1} : back / none / forward                      */
    HEAD_JUMP,         /* {0,1}     : up                                         */
    HEAD_YAW,          /* {-2,-1,0,+1,+2} degrees added to maintained yaw        */
    HEAD_PITCH,        /* {-1,0,+1} degrees added to maintained pitch (clamped)  */
    HEAD_FIRE,         /* {0,1}     : fire (combat phase; reserved)              */
    NUM_HEADS
};

#define ACTION_LOGITS_TOTAL (3 + 3 + 2 + 5 + 3 + 2)   /* == 18 */
#define MAX_HEAD_SIZE        5

/* Sizes per head, indexed by the HEAD_* enum. Defined once in agent_obs.c. */
extern const int   HEAD_SIZES[NUM_HEADS];
/* Discrete bin -> physical delta tables (parity-critical). Defined in agent_obs.c. */
extern const float YAW_DELTA_DEG[5];
extern const float PITCH_DELTA_DEG[3];

#define PITCH_LIMIT   89.0f    /* mirrors camera.c's pitch clamp exactly */

/* ------------------------------------------------------------------ *
 *  Policy network dimensions (must equal training/ppo.py and export.py).
 *  obs(40) -> Dense(128) tanh -> Dense(128) tanh -> { per-head logits }
 * ------------------------------------------------------------------ */
#define POLICY_HIDDEN  128
#define POLICY_LAYERS  2       /* hidden layers; the heads are a third, linear layer */

/* ------------------------------------------------------------------ *
 *  World view consumed by build_observation().
 *
 *  This is a read-only aggregate. On the server it is filled each tick from the
 *  authoritative g.clients[] (which stays the source of truth); in training the
 *  cffi env owns it directly. Velocity is NOT stored: it is derived inside
 *  build_observation as (pos - prev_pos)/dt, so it is computed one way only.
 * ------------------------------------------------------------------ */
typedef struct {
    PlayerState players[MAX_CLIENTS];
    int         present[MAX_CLIENTS];        /* 1 if this slot holds a player    */
    float       prev_pos[MAX_CLIENTS][3];    /* position one tick ago (velocity) */
    int         team[MAX_CLIENTS];           /* is-enemy is team[i] != team[me]  */
    float       target[MAX_CLIENTS][3];      /* per-agent navigation target      */
    float       dt;                          /* seconds; == TICK_DT              */
} World;

/* Build the OBS_DIM observation vector for agent slot `agent_id`. SHARED between
 * the server and the training .so -- see src/agent_obs.c. */
void build_observation(const World *w, int agent_id, float out[OBS_DIM]);

/* Decode a chosen multi-discrete action (one class index per head) into an
 * InputCmd, maintaining absolute yaw/pitch across calls. SHARED so the server
 * controller and the Python env decode identically. */
void agent_decode_action(const int heads[NUM_HEADS],
                         float *yaw, float *pitch, InputCmd *out);

/* ------------------------------------------------------------------ *
 *  Server-side controller (src/agent.c).
 * ------------------------------------------------------------------ */
typedef enum {
    CONTROLLER_NETWORK = 0,   /* input arrives in packets (a human) -- the default */
    CONTROLLER_AGENT          /* input is produced locally by a policy/stub        */
} ControllerType;

typedef enum {
    AGENT_STUB_RANDOM = 0,    /* deterministic random walk (Phase 0, no --policy)  */
    AGENT_STUB_FIXED,         /* constant "walk forward" (diagnostics)             */
    AGENT_POLICY              /* run the loaded policy network                     */
} AgentMode;

struct Policy;   /* opaque; defined in policy.c */

typedef struct {
    AgentMode      mode;
    struct Policy *policy;             /* NULL in stub modes                       */
    int            frame_skip;         /* decide every k ticks; hold in between    */
    int            ticks_since_decision;
    InputCmd       held;               /* action held between decision ticks       */
    float          yaw, pitch;         /* maintained absolute orientation          */
    uint32_t       rng;                /* per-agent deterministic stream (stub)    */
} Agent;

/* Initialise a controller. frame_skip must equal the policy's baked-in k (the
 * server asserts this when a policy is loaded). seed makes stub agents
 * reproducible from a fixed seed (used by the loopback test). */
void agent_init(Agent *a, AgentMode mode, struct Policy *policy,
                int frame_skip, float yaw0, float pitch0, uint32_t seed);

/* Produce this tick's InputCmd for agent slot `id`. On a decision tick it builds
 * the observation and chooses a fresh action; otherwise it returns the held
 * action (frame-skip parity with training). Does NOT call world_apply_input --
 * the server owns the single apply path. */
void agent_think(Agent *a, const World *w, int id, InputCmd *out);

#endif /* PULSE_AGENT_H */
