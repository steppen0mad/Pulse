#ifndef PULSE_NET_H
#define PULSE_NET_H

#include <stdint.h>
#include <netinet/in.h>
#include "serialize.h"
#include "protocol.h"

/*
 * UDP transport + a reliability layer built directly on raw sockets.
 *
 * There is no TCP-style guaranteed in-order delivery here. Instead we use the
 * classic seq/ack/ack_bits scheme: every packet carries a 16-bit sequence
 * number, an ack of the most recent sequence we have seen from the peer, and a
 * 32-bit field acking the 32 sequences before that. From those acks each side
 * derives round-trip time and packet loss, and learns which of its own packets
 * arrived -- without ever blocking or retransmitting at the transport layer.
 * Reliability that the game needs (inputs, events) is layered on top via
 * redundant resends, not here.
 */

#define HEADER_SIZE     17     /* id(4) + type(1) + seq(2) + ack(2) + ackbits(4) + tick(4) */
#define MAX_PACKET      1200   /* stays comfortably under the Ethernet MTU */
#define SENT_BUFFER     256    /* in-flight packets tracked for ack/RTT */
#define DELAY_QUEUE_MAX 2048   /* capacity of the artificial-latency queue */

typedef struct {
    uint32_t protocol_id;
    uint8_t  type;
    uint16_t seq;
    uint16_t ack;
    uint32_t ack_bits;
    uint32_t tick;
} PacketHeader;

/* ----------------------------------------------------------------------------
 * Reliability endpoint: one per peer (the server keeps one per client).
 * -------------------------------------------------------------------------- */
typedef struct {
    uint16_t local_seq;     /* next sequence number to send */
    uint16_t remote_seq;    /* highest sequence seen from the peer */
    int      have_remote;   /* have we received anything yet? */
    uint32_t recv_bits;     /* bit i set => (remote_seq - (i+1)) was received */

    struct {
        uint16_t seq;
        double   time;      /* send timestamp, for RTT */
        int      acked;
        int      in_use;
    } sent[SENT_BUFFER];

    double   rtt;           /* smoothed round-trip time, seconds */
    uint64_t n_sent;
    uint64_t n_acked;
    double   last_recv;     /* for timeout detection */
    double   last_send;     /* for heartbeat scheduling */
} Reliable;

void     rel_init(Reliable *r);
uint16_t rel_alloc_seq(Reliable *r, double now);          /* stamps + records an outbound packet */
void     rel_on_recv(Reliable *r, const PacketHeader *h, double now);
float    rel_loss(const Reliable *r);                     /* fraction lost, 0..1 */

/* True if 16-bit sequence `a` is more recent than `b`, handling wraparound. */
static inline int seq_greater(uint16_t a, uint16_t b) {
    return (a != b) && ((uint16_t)(a - b) < 0x8000u);
}

/* ----------------------------------------------------------------------------
 * Non-blocking UDP socket, with optional artificial loss/latency for demos.
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t            data[MAX_PACKET];
    int                len;
    struct sockaddr_in to;
    double             due;     /* wall time at which this queued packet is sent */
} DelayedPkt;

typedef struct {
    int        fd;
    float      sim_loss;        /* probability [0,1] a sent packet is dropped */
    int        sim_latency_ms;  /* artificial one-way delay added on send */
    DelayedPkt dq[DELAY_QUEUE_MAX];
    int        dq_count;
} UdpSocket;

/* Open + bind a non-blocking UDP socket. port 0 picks an ephemeral port.
 * Returns 1 on success, 0 on failure (after printing the cause). */
int  net_open(UdpSocket *s, uint16_t port);
void net_set_sim(UdpSocket *s, float loss, int latency_ms);
void net_send(UdpSocket *s, const struct sockaddr_in *to,
              const uint8_t *data, int len, double now);
void net_update(UdpSocket *s, double now);   /* release any due delayed packets */
/* Receive one datagram. Returns byte count (>0), 0 if none pending, -1 on a
 * real error (already reported). */
int  net_recv(UdpSocket *s, struct sockaddr_in *from, uint8_t *data, int maxlen);
void net_close(UdpSocket *s);

/* Header (de)serialization, wired to the reliability layer. */
void net_write_header(ByteBuf *b, Reliable *r, uint8_t type, uint32_t tick, double now);
int  net_read_header(ByteBuf *b, PacketHeader *h);   /* 0 if protocol id mismatch / truncated */

double now_seconds(void);   /* monotonic clock, seconds */

#endif /* PULSE_NET_H */
