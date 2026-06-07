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
    CHECK(!seq_greater(5, 5));
    CHECK(seq_greater(0, 65535));
    CHECK(!seq_greater(65535, 0));
    CHECK(seq_greater(0, 40000));
    CHECK(!seq_greater(40000, 0));
    printf("  seq wraparound comparison ......... ok\n");
    return 0;
}

static int test_ack_bits_in_order(void) {
    Reliable b; rel_init(&b);
    for (uint16_t seq = 0; seq <= 3; seq++) {
        PacketHeader h = { PULSE_PROTOCOL_ID, PKT_INPUT, seq, 0, 0, 0 };
        rel_on_recv(&b, &h, 1.0);
    }
    CHECK(b.remote_seq == 3);
    CHECK(b.recv_bits == 0x7);
    printf("  ack_bits, in-order delivery ....... ok\n");
    return 0;
}

static int test_ack_bits_with_gap(void) {
    Reliable b; rel_init(&b);
    PacketHeader h0 = { PULSE_PROTOCOL_ID, PKT_INPUT, 0, 0, 0, 0 };
    PacketHeader h2 = { PULSE_PROTOCOL_ID, PKT_INPUT, 2, 0, 0, 0 };
    rel_on_recv(&b, &h0, 1.0);
    rel_on_recv(&b, &h2, 1.0);
    CHECK(b.remote_seq == 2);
    CHECK((b.recv_bits & (1u << 0)) == 0);
    CHECK((b.recv_bits & (1u << 1)) != 0);
    printf("  ack_bits, with a dropped packet ... ok\n");
    return 0;
}

static int test_late_packet(void) {
    Reliable b; rel_init(&b);
    PacketHeader h5 = { PULSE_PROTOCOL_ID, PKT_INPUT, 5, 0, 0, 0 };
    PacketHeader h3 = { PULSE_PROTOCOL_ID, PKT_INPUT, 3, 0, 0, 0 };
    rel_on_recv(&b, &h5, 1.0);
    rel_on_recv(&b, &h3, 1.0);
    CHECK(b.remote_seq == 5);
    CHECK((b.recv_bits & (1u << 1)) != 0);
    printf("  out-of-order (late) packet ........ ok\n");
    return 0;
}

static int test_ack_marks_sent_and_rtt(void) {
    Reliable a; rel_init(&a);
    Reliable b; rel_init(&b);

    PacketHeader a0 = make_header(&a, 0.0);
    PacketHeader a1 = make_header(&a, 0.0);
    PacketHeader a2 = make_header(&a, 0.0);
    CHECK(a.n_sent == 3);
    CHECK(a.n_acked == 0);

    rel_on_recv(&b, &a0, 0.05);
    rel_on_recv(&b, &a1, 0.05);
    rel_on_recv(&b, &a2, 0.05);

    PacketHeader breply = make_header(&b, 0.05);
    CHECK(breply.ack == 2);

    rel_on_recv(&a, &breply, 0.10);
    CHECK(a.n_acked == 3);
    CHECK(a.rtt > 0.0);
    CHECK(a.rtt > 0.05 && a.rtt < 0.2);
    CHECK(rel_loss(&a) == 0.0f);
    printf("  ack processing + RTT estimate ..... ok\n");
    return 0;
}

static int test_loss_accounting(void) {
    Reliable a; rel_init(&a);
    Reliable b; rel_init(&b);
    PacketHeader a0 = make_header(&a, 0.0);
    make_header(&a, 0.0);
    make_header(&a, 0.0);
    rel_on_recv(&b, &a0, 0.01);
    PacketHeader breply = make_header(&b, 0.01);
    rel_on_recv(&a, &breply, 0.02);
    CHECK(a.n_sent == 3);
    CHECK(a.n_acked == 1);
    CHECK(rel_loss(&a) > 0.6f && rel_loss(&a) < 0.7f);
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
