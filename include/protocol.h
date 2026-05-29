#ifndef PULSE_PROTOCOL_H
#define PULSE_PROTOCOL_H

#include <stdint.h>

/*
 * Wire protocol shared by the authoritative server and every client.
 *
 * Everything here is intentionally compact: inputs and snapshots are sent many
 * times per second, so the per-message overhead matters.
 */

#define PULSE_PROTOCOL_ID   0x50554c53u   /* "PULS" -- rejects foreign/stale datagrams */
#define PULSE_DEFAULT_PORT  42424

#define TICK_RATE           60            /* authoritative simulation steps per second */
#define SNAPSHOT_RATE       20            /* world snapshots broadcast per second       */
#define TICKS_PER_SNAPSHOT  (TICK_RATE / SNAPSHOT_RATE)   /* == 3 */
#define TICK_DT             (1.0f / (float)TICK_RATE)      /* fixed timestep, seconds   */

#define MAX_CLIENTS         8
#define MAX_INPUTS_PER_PKT  32            /* redundant resend window (loss tolerance)   */
#define MAX_EVENTS_PER_PKT  4             /* recent events piggybacked on each snapshot */
#define INTERP_DELAY        0.100f        /* render remote players 100 ms in the past   */

#define CONNECT_TIMEOUT     5.0           /* seconds of silence before a peer is dropped */
#define HEARTBEAT_INTERVAL  0.1           /* keepalive cadence when otherwise idle       */

/* packet types (PacketHeader.type) */
enum {
    PKT_CONNECT = 1,   /* client -> server : request a player slot        */
    PKT_ACCEPT,        /* server -> client : slot granted, carries our id */
    PKT_INPUT,         /* client -> server : a window of input commands   */
    PKT_SNAPSHOT,      /* server -> client : authoritative world state    */
    PKT_HEARTBEAT,     /* either direction : keepalive                    */
    PKT_DISCONNECT     /* either direction : leaving cleanly              */
};

/* button bitfield carried in an InputCmd */
enum {
    BTN_FWD   = 1 << 0,
    BTN_BACK  = 1 << 1,
    BTN_LEFT  = 1 << 2,
    BTN_RIGHT = 1 << 3,
    BTN_UP    = 1 << 4,
    BTN_DOWN  = 1 << 5
};

/* discrete events (piggybacked on snapshots, deduped client-side by id) */
enum {
    EV_PLAYER_JOIN = 1,
    EV_PLAYER_LEAVE
};

/* One client input command. The server applies each unique seq exactly once,
 * advancing the simulation by TICK_DT; the client applies the same command to
 * its local prediction, which is what makes prediction match authority. */
typedef struct {
    uint32_t seq;       /* monotonically increasing, per client */
    uint8_t  buttons;   /* BTN_* bitfield                       */
    float    yaw;       /* look orientation, degrees            */
    float    pitch;
} InputCmd;

#endif /* PULSE_PROTOCOL_H */
