#ifndef PULSE_AGENT_H
#define PULSE_AGENT_H

#include <stdint.h>
#include "protocol.h"
#include "world.h"


#define OBS_VERSION    1

#define K_NEAREST      3
#define OBS_SELF       5
#define OBS_PER_OTHER  8
#define OBS_RAYS       8
#define OBS_TASK       3
#define OBS_DIM (OBS_SELF + K_NEAREST * OBS_PER_OTHER + OBS_RAYS + OBS_TASK)

enum {
    HEAD_MOVE_X = 0,
    HEAD_MOVE_Z,
    HEAD_JUMP,
    HEAD_YAW,
    HEAD_PITCH,
    HEAD_FIRE,
    NUM_HEADS
};

#define ACTION_LOGITS_TOTAL (3 + 3 + 2 + 5 + 3 + 2)
#define MAX_HEAD_SIZE        5

extern const int   HEAD_SIZES[NUM_HEADS];
extern const float YAW_DELTA_DEG[5];
extern const float PITCH_DELTA_DEG[3];

#define PITCH_LIMIT   89.0f

#define POLICY_HIDDEN  128
#define POLICY_LAYERS  2

typedef struct {
    PlayerState players[MAX_CLIENTS];
    int         present[MAX_CLIENTS];
    float       prev_pos[MAX_CLIENTS][3];
    int         team[MAX_CLIENTS];
    float       target[MAX_CLIENTS][3];
    float       dt;
} World;

void build_observation(const World *w, int agent_id, float out[OBS_DIM]);

void agent_decode_action(const int heads[NUM_HEADS],
                         float *yaw, float *pitch, InputCmd *out);

typedef enum {
    CONTROLLER_NETWORK = 0,
    CONTROLLER_AGENT
} ControllerType;

typedef enum {
    AGENT_STUB_RANDOM = 0,
    AGENT_STUB_FIXED,
    AGENT_POLICY
} AgentMode;

struct Policy;

typedef struct {
    AgentMode      mode;
    struct Policy *policy;
    int            frame_skip;
    int            ticks_since_decision;
    InputCmd       held;
    float          yaw, pitch;
    uint32_t       rng;
} Agent;

void agent_init(Agent *a, AgentMode mode, struct Policy *policy,
                int frame_skip, float yaw0, float pitch0, uint32_t seed);

void agent_think(Agent *a, const World *w, int id, InputCmd *out);

#endif
