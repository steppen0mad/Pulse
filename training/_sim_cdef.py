"""
Single source of truth for the cffi binding to the C simulation + perception.

The training environment must run the EXACT same C code the live server runs --
world_apply_input (the deterministic step), build_observation (perception), and
agent_decode_action (the action map). Compiling those real sources into a shared
library and calling them from Python is what guarantees the trained policy
behaves identically at deployment. Nothing about the world is reimplemented in
Python.

This module is imported by build_sim.py (to compile the extension) and by env.py
(which imports the compiled module). Keeping the cdef in one place stops the
struct layout from drifting between build and use.

The struct transcription below is checked two ways:
  * API mode: set_source #includes the real headers, so the C compiler verifies
    every signature/struct field against world.h / agent.h.
  * A runtime assertion in build_sim.py compares ffi.sizeof("World") against the
    C sizeof(World) exposed by world_struct_size().
"""

CDEF = r"""
typedef struct { float pos[3]; float yaw; float pitch; } PlayerState;
typedef struct { uint32_t seq; uint8_t buttons; float yaw; float pitch; } InputCmd;

typedef struct {
    PlayerState players[32];
    int         present[32];
    float       prev_pos[32][3];
    int         team[32];
    float       target[32][3];
    float       dt;
} World;

/* deterministic sim + shared perception/decode (the train/deploy parity surface) */
void world_apply_input(PlayerState *s, const InputCmd *cmd, float dt);
void build_observation(const World *w, int agent_id, float *out);
void agent_decode_action(const int *heads, float *yaw, float *pitch, InputCmd *out);

/* contract tables, read directly so Python never re-declares them */
extern const int   HEAD_SIZES[6];
extern const float YAW_DELTA_DEG[5];
extern const float PITCH_DELTA_DEG[3];

/* layout/size parity helper */
int world_struct_size(void);

/* integer constants pulled from the real headers (cffi reads them from the C) */
#define OBS_DIM ...
#define NUM_HEADS ...
#define OBS_VERSION ...
#define POLICY_HIDDEN ...
#define POLICY_LAYERS ...
#define MAX_CLIENTS ...
#define TICK_RATE ...

/* float constants (cffi's #define probe is integer-only, so declare these as
 * read-only consts -- the values still come from the real header macros) */
static const float ARENA_HALF_EXTENT;
static const float ARENA_FLOOR;
static const float ARENA_CEIL;
static const float EYE_HEIGHT;
static const float MOVE_SPEED;
"""

SOURCES = ["src/world.c", "src/agent_obs.c"]
INCLUDE_DIRS = ["include"]

EXTRA_COMPILE_ARGS = ["-O2", "-std=gnu11", "-ffp-contract=off"]

MODULE_NAME = "_pulse_sim"
