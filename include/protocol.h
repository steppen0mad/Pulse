#ifndef PULSE_PROTOCOL_H
#define PULSE_PROTOCOL_H

#include <stdint.h>


#define PULSE_PROTOCOL_ID   0x50554c53u
#define PULSE_DEFAULT_PORT  42424

#define TICK_RATE           60
#define SNAPSHOT_RATE       20
#define TICKS_PER_SNAPSHOT  (TICK_RATE / SNAPSHOT_RATE)
#define TICK_DT             (1.0f / (float)TICK_RATE)

#define MAX_CLIENTS         32
#define MAX_INPUTS_PER_PKT  32
#define MAX_EVENTS_PER_PKT  4
#define INTERP_DELAY        0.100f

#define CONNECT_TIMEOUT     5.0
#define HEARTBEAT_INTERVAL  0.1

enum {
    PKT_CONNECT = 1,
    PKT_ACCEPT,
    PKT_INPUT,
    PKT_SNAPSHOT,
    PKT_HEARTBEAT,
    PKT_DISCONNECT
};

enum {
    BTN_FWD   = 1 << 0,
    BTN_BACK  = 1 << 1,
    BTN_LEFT  = 1 << 2,
    BTN_RIGHT = 1 << 3,
    BTN_UP    = 1 << 4,
    BTN_DOWN  = 1 << 5,
    BTN_FIRE  = 1 << 6
};

enum {
    EV_PLAYER_JOIN = 1,
    EV_PLAYER_LEAVE
};

typedef struct {
    uint32_t seq;
    uint8_t  buttons;
    float    yaw;
    float    pitch;
} InputCmd;

#endif
