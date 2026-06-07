#ifndef PULSE_NET_H
#define PULSE_NET_H

#include <stdint.h>
#include <netinet/in.h>
#include "serialize.h"
#include "protocol.h"


#define HEADER_SIZE     17
#define MAX_PACKET      1200
#define SENT_BUFFER     256
#define DELAY_QUEUE_MAX 2048

typedef struct {
    uint32_t protocol_id;
    uint8_t  type;
    uint16_t seq;
    uint16_t ack;
    uint32_t ack_bits;
    uint32_t tick;
} PacketHeader;

typedef struct {
    uint16_t local_seq;
    uint16_t remote_seq;
    int      have_remote;
    uint32_t recv_bits;

    struct {
        uint16_t seq;
        double   time;
        int      acked;
        int      in_use;
    } sent[SENT_BUFFER];

    double   rtt;
    uint64_t n_sent;
    uint64_t n_acked;
    double   last_recv;
    double   last_send;
} Reliable;

void     rel_init(Reliable *r);
uint16_t rel_alloc_seq(Reliable *r, double now);
void     rel_on_recv(Reliable *r, const PacketHeader *h, double now);
float    rel_loss(const Reliable *r);

static inline int seq_greater(uint16_t a, uint16_t b) {
    return (a != b) && ((uint16_t)(a - b) < 0x8000u);
}

typedef struct {
    uint8_t            data[MAX_PACKET];
    int                len;
    struct sockaddr_in to;
    double             due;
} DelayedPkt;

typedef struct {
    int        fd;
    float      sim_loss;
    int        sim_latency_ms;
    DelayedPkt dq[DELAY_QUEUE_MAX];
    int        dq_count;
} UdpSocket;

int  net_open(UdpSocket *s, uint16_t port);
void net_set_sim(UdpSocket *s, float loss, int latency_ms);
void net_send(UdpSocket *s, const struct sockaddr_in *to,
              const uint8_t *data, int len, double now);
void net_update(UdpSocket *s, double now);
int  net_recv(UdpSocket *s, struct sockaddr_in *from, uint8_t *data, int maxlen);
void net_close(UdpSocket *s);

void net_write_header(ByteBuf *b, Reliable *r, uint8_t type, uint32_t tick, double now);
int  net_read_header(ByteBuf *b, PacketHeader *h);

double now_seconds(void);

#endif
