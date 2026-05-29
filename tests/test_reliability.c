/*
 * Unit tests for the reliability layer (seq/ack/ack_bits). No sockets, no
 * display -- pure logic, so it runs anywhere and is safe for CI. Any failed
 * assertion aborts with a non-zero exit code.
 */
#include "net.h"

#include <stdio.h>
#include <assert.h>

static int tests_run = 0;
#define CHECK(cond) do {                                              \
        tests_run++;                                                  \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                 \
        }                                                             \
    } while (0)

/* Build a header the way net_write_header would, but without a socket. */
static PacketHeader make_header(Reliable *r, double now) {
    PacketHeader h;
    h.protocol_id = PULSE_PROTOCOL_ID;
    h.type        = PKT_INPUT;
    h.seq         = rel_alloc_seq(r, now);
    h.ack         = r->remote_seq;
    h.ack_bits    = r->recv_bits;
    h.tick        = 0;
    return h;
}

static int test_seq_wraparound(void) {
    CHECK(seq_greater(1, 0));
    CHECK(!seq_greater(0, 1));
    CHECK(!seq_greater(5, 5));                 /* equal is not "greater" */
    CHECK(seq_greater(0, 65535));              /* 0 comes right after 65535 */
    CHECK(!seq_greater(65535, 0));
    CHECK(seq_greater(0, 40000));              /* 40000 is "behind" via wrap, so 0 is newer */
    CHECK(!seq_greater(40000, 0));
    printf("  seq wraparound comparison ......... ok\n");
    return 0;
}

static int test_ack_bits_in_order(void) {
    /* B receives A's packets 0,1,2,3 in order. After packet 3, remote_seq==3
     * and the three prior (0,1,2) are recorded in recv_bits at bits 0,1,2. */
    Reliable b; rel_init(&b);
    for (uint16_t seq = 0; seq <= 3; seq++) {
        PacketHeader h = { PULSE_PROTOCOL_ID, PKT_INPUT, seq, 0, 0, 0 };
        rel_on_recv(&b, &h, 1.0);
    }
    CHECK(b.remote_seq == 3);
    CHECK(b.recv_bits == 0x7);   /* bits 0,1,2 -> seqs 2,1,0 */
    printf("  ack_bits, in-order delivery ....... ok\n");
    return 0;
}

static int test_ack_bits_with_gap(void) {
    /* B receives 0, loses 1, receives 2. remote_seq==2; bit for seq 0 set
     * (diff 2 -> bit 1), bit for the lost seq 1 (diff 1 -> bit 0) clear. */
    Reliable b; rel_init(&b);
    PacketHeader h0 = { PULSE_PROTOCOL_ID, PKT_INPUT, 0, 0, 0, 0 };
    PacketHeader h2 = { PULSE_PROTOCOL_ID, PKT_INPUT, 2, 0, 0, 0 };
    rel_on_recv(&b, &h0, 1.0);
    rel_on_recv(&b, &h2, 1.0);
    CHECK(b.remote_seq == 2);
    CHECK((b.recv_bits & (1u << 0)) == 0);   /* seq 1 was lost */
    CHECK((b.recv_bits & (1u << 1)) != 0);   /* seq 0 arrived */
    printf("  ack_bits, with a dropped packet ... ok\n");
    return 0;
}

static int test_late_packet(void) {
    /* A reordered older packet should still be recorded, not advance remote_seq. */
    Reliable b; rel_init(&b);
    PacketHeader h5 = { PULSE_PROTOCOL_ID, PKT_INPUT, 5, 0, 0, 0 };
    PacketHeader h3 = { PULSE_PROTOCOL_ID, PKT_INPUT, 3, 0, 0, 0 };
    rel_on_recv(&b, &h5, 1.0);
    rel_on_recv(&b, &h3, 1.0);   /* arrives late */
    CHECK(b.remote_seq == 5);            /* unchanged */
    CHECK((b.recv_bits & (1u << 1)) != 0);   /* seq 3 is 2 behind 5 -> bit 1 */
    printf("  out-of-order (late) packet ........ ok\n");
    return 0;
}

static int test_ack_marks_sent_and_rtt(void) {
    /* Full round trip: A sends, B receives + acks back, A learns it was acked
     * and derives an RTT sample. */
    Reliable a; rel_init(&a);
    Reliable b; rel_init(&b);

    /* A sends three packets at t=0. */
    PacketHeader a0 = make_header(&a, 0.0);
    PacketHeader a1 = make_header(&a, 0.0);
    PacketHeader a2 = make_header(&a, 0.0);
    CHECK(a.n_sent == 3);
    CHECK(a.n_acked == 0);

    /* B receives all three. */
    rel_on_recv(&b, &a0, 0.05);
    rel_on_recv(&b, &a1, 0.05);
    rel_on_recv(&b, &a2, 0.05);

    /* B replies at t=0.05; its header acks A's latest (seq 2) + the 2 before. */
    PacketHeader breply = make_header(&b, 0.05);
    CHECK(breply.ack == 2);

    /* A receives the reply at t=0.10 -> all three of A's packets are acked. */
    rel_on_recv(&a, &breply, 0.10);
    CHECK(a.n_acked == 3);
    CHECK(a.rtt > 0.0);                /* RTT sample folded in (~0.10s) */
    CHECK(a.rtt > 0.05 && a.rtt < 0.2);
    CHECK(rel_loss(&a) == 0.0f);       /* nothing lost */
    printf("  ack processing + RTT estimate ..... ok\n");
    return 0;
}

static int test_loss_accounting(void) {
    /* Two of A's packets are never acked -> loss reflects that. */
    Reliable a; rel_init(&a);
    Reliable b; rel_init(&b);
    PacketHeader a0 = make_header(&a, 0.0);
    make_header(&a, 0.0);   /* seq 1 -- "lost", never reaches B */
    make_header(&a, 0.0);   /* seq 2 -- "lost", never reaches B */
    rel_on_recv(&b, &a0, 0.01);
    PacketHeader breply = make_header(&b, 0.01);
    rel_on_recv(&a, &breply, 0.02);
    CHECK(a.n_sent == 3);
    CHECK(a.n_acked == 1);
    CHECK(rel_loss(&a) > 0.6f && rel_loss(&a) < 0.7f);   /* ~0.666 */
    printf("  packet-loss accounting ............ ok\n");
    return 0;
}

int main(void) {
    printf("reliability tests:\n");
    if (test_seq_wraparound())        return 1;
    if (test_ack_bits_in_order())     return 1;
    if (test_ack_bits_with_gap())     return 1;
    if (test_late_packet())           return 1;
    if (test_ack_marks_sent_and_rtt())return 1;
    if (test_loss_accounting())       return 1;
    printf("all %d reliability checks passed\n", tests_run);
    return 0;
}
