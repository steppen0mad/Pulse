#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>

//clock
double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double frand(void) {
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

//reliability layer (seq / ack / ack_bits)
void rel_init(Reliable *r) {
    memset(r, 0, sizeof *r);
}

uint16_t rel_alloc_seq(Reliable *r, double now) {
    uint16_t seq = r->local_seq++;
    int idx = seq % SENT_BUFFER;
    r->sent[idx].seq    = seq;
    r->sent[idx].time   = now;
    r->sent[idx].acked  = 0;
    r->sent[idx].in_use = 1;
    r->n_sent++;
    r->last_send = now;
    return seq;
}

//mark one of our previously sent packets as acked and fold its RTT sample in
static void rel_ack_one(Reliable *r, uint16_t seq, double now) {
    int idx = seq % SENT_BUFFER;
    if (!r->sent[idx].in_use || r->sent[idx].seq != seq || r->sent[idx].acked)
        return;
    r->sent[idx].acked = 1;
    r->n_acked++;

    double sample = now - r->sent[idx].time;
    if (sample >= 0.0) {
        if (r->rtt == 0.0) r->rtt = sample;
        else               r->rtt += 0.1 * (sample - r->rtt);   /* EWMA */
    }
}

void rel_on_recv(Reliable *r, const PacketHeader *h, double now) {
    r->last_recv = now;

    /* (1) Record that we received the peer's sequence h->seq, maintaining the
     *     received-history bitfield relative to the newest sequence seen. */
    if (!r->have_remote) {
        r->remote_seq  = h->seq;
        r->recv_bits   = 0;
        r->have_remote = 1;
    } else if (seq_greater(h->seq, r->remote_seq)) {
        uint16_t shift = (uint16_t)(h->seq - r->remote_seq);
        if (shift >= 32) {
            r->recv_bits = 0;                       //the old window fell off entirely
        } else {
            //slide the window forward; the previous newest now sits at bit shift-1
            r->recv_bits = (r->recv_bits << shift) | (1u << (shift - 1));
        }
        r->remote_seq = h->seq;
    } else if (h->seq != r->remote_seq) {
        uint16_t diff = (uint16_t)(r->remote_seq - h->seq);     //an older packet, possibly reordered
        if (diff >= 1 && diff <= 32)
            r->recv_bits |= (1u << (diff - 1));
    }

    /* (2) Process the peer's acks of OUR packets: h->ack plus the 32 before it. */
    rel_ack_one(r, h->ack, now);
    for (int i = 0; i < 32; i++)
        if (h->ack_bits & (1u << i))
            rel_ack_one(r, (uint16_t)(h->ack - (i + 1)), now);
}

float rel_loss(const Reliable *r) {
    if (r->n_sent == 0) return 0.0f;
    float delivered = (float)r->n_acked / (float)r->n_sent;
    float loss = 1.0f - delivered;
    return loss < 0.0f ? 0.0f : loss;
}

 //header (de)serialization
void net_write_header(ByteBuf *b, Reliable *r, uint8_t type, uint32_t tick, double now) {
    uint16_t seq = rel_alloc_seq(r, now);
    wr_u32(b, PULSE_PROTOCOL_ID);
    wr_u8 (b, type);
    wr_u16(b, seq);
    wr_u16(b, r->remote_seq);   //ack: newest sequence we've seen from the peer 
    wr_u32(b, r->recv_bits);    //ack_bits: the 32 before that
    wr_u32(b, tick);
}

int net_read_header(ByteBuf *b, PacketHeader *h) {
    h->protocol_id = rd_u32(b);
    if (h->protocol_id != PULSE_PROTOCOL_ID)
        return 0;
    h->type     = rd_u8(b);
    h->seq      = rd_u16(b);
    h->ack      = rd_u16(b);
    h->ack_bits = rd_u32(b);
    h->tick     = rd_u32(b);
    return b->ok;
}

//UDP socket
int net_open(UdpSocket *s, uint16_t port) {
    memset(s, 0, sizeof *s);

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) {
        perror("socket");
        return 0;
    }

    int flags = fcntl(s->fd, F_GETFL, 0);
    if (flags < 0 || fcntl(s->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK");
        close(s->fd);
        return 0;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (bind(s->fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(s->fd);
        return 0;
    }
    return 1;
}

void net_set_sim(UdpSocket *s, float loss, int latency_ms) {
    s->sim_loss       = loss;
    s->sim_latency_ms = latency_ms;
}

static void raw_send(UdpSocket *s, const struct sockaddr_in *to,
                     const uint8_t *data, int len) {
    ssize_t n = sendto(s->fd, data, (size_t)len, 0,
                       (const struct sockaddr *)to, sizeof *to);
    if (n < 0)
        perror("sendto");   //report, but a single bad send must not kill the loop
}

void net_send(UdpSocket *s, const struct sockaddr_in *to,
              const uint8_t *data, int len, double now) {
    if (len > MAX_PACKET) {
        fprintf(stderr, "net_send: packet of %d bytes exceeds MAX_PACKET\n", len);
        return;
    }

    //simulated packet loss: deliberately drop, the way a lossy link would
    if (s->sim_loss > 0.0f && frand() < (double)s->sim_loss)
        return;

    //simulated latency: hold the packet in a queue until its release time
    if (s->sim_latency_ms > 0) {
        if (s->dq_count >= DELAY_QUEUE_MAX) {
            fprintf(stderr, "net_send: delay queue full, sending immediately\n");
            raw_send(s, to, data, len);
            return;
        }
        DelayedPkt *p = &s->dq[s->dq_count++];
        memcpy(p->data, data, (size_t)len);
        p->len = len;
        p->to  = *to;
        p->due = now + (double)s->sim_latency_ms / 1000.0;
        return;
    }

    raw_send(s, to, data, len);
}

void net_update(UdpSocket *s, double now) {
    int w = 0;
    for (int i = 0; i < s->dq_count; i++) {
        if (s->dq[i].due <= now) {
            raw_send(s, &s->dq[i].to, s->dq[i].data, s->dq[i].len);
        } else {
            if (w != i) s->dq[w] = s->dq[i];   //compact the still-pending entries
            w++;
        }
    }
    s->dq_count = w;
}

int net_recv(UdpSocket *s, struct sockaddr_in *from, uint8_t *data, int maxlen) {
    socklen_t fromlen = sizeof *from;
    ssize_t n = recvfrom(s->fd, data, (size_t)maxlen, 0,
                         (struct sockaddr *)from, &fromlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;       //nothing to read right now
        perror("recvfrom");
        return -1;
    }
    return (int)n;
}

void net_close(UdpSocket *s) {
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}
