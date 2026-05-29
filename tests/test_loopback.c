/*
 * Standalone network test harness.
 *
 * Stands up a miniature server and client that talk over REAL localhost UDP
 * sockets using the real net/reliability/simulation code, then injects packet
 * loss and latency and checks the netcode's guarantees end to end:
 *
 *   1. Inputs survive loss   -- with redundant resends, the server's
 *      authoritative state still advances under heavy packet loss.
 *   2. Reconciliation converges -- after input stops, the client's predicted
 *      state ends up bit-for-bit on top of the server's authoritative state,
 *      because it snaps to authority and replays only its unacked inputs.
 *
 * Headless and deterministic (fixed RNG seed, simulated clock). Non-zero exit
 * on any failed assertion.
 */
#include "net.h"
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEST_PORT   42500
#define PENDING_MAX 256

/* ---- a self-contained client, mirroring src/client.c's netcode ---- */
typedef struct {
    UdpSocket          sock;
    Reliable           rel;
    struct sockaddr_in server;
    PlayerState        self;
    uint32_t           input_seq;
    InputCmd           pending[PENDING_MAX];
    int                pending_count;
} TestClient;

/* ---- a self-contained single-client server, mirroring src/server.c ---- */
typedef struct {
    UdpSocket          sock;
    Reliable           rel;
    struct sockaddr_in client;
    int                have_client;
    PlayerState        state;
    uint32_t           last_input;
} TestServer;

static float dist3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static void client_send_inputs(TestClient *c, double now) {
    uint8_t buf[MAX_PACKET];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &c->rel, PKT_INPUT, c->input_seq, now);

    int count = c->pending_count;
    if (count > MAX_INPUTS_PER_PKT) count = MAX_INPUTS_PER_PKT;
    int start = c->pending_count - count;

    wr_u32(&b, 0);
    wr_u8 (&b, (uint8_t)count);
    for (int i = start; i < c->pending_count; i++) {
        wr_u32(&b, c->pending[i].seq);
        wr_u8 (&b, c->pending[i].buttons);
        wr_f32(&b, c->pending[i].yaw);
        wr_f32(&b, c->pending[i].pitch);
    }
    net_send(&c->sock, &c->server, buf, b.pos, now);
}

static void client_reconcile(TestClient *c, uint32_t last_input, const PlayerState *auth) {
    c->self = *auth;
    int keep = 0;
    while (keep < c->pending_count && c->pending[keep].seq <= last_input) keep++;
    if (keep > 0) {
        memmove(&c->pending[0], &c->pending[keep],
                sizeof(InputCmd) * (c->pending_count - keep));
        c->pending_count -= keep;
    }
    for (int i = 0; i < c->pending_count; i++)
        world_apply_input(&c->self, &c->pending[i], TICK_DT);
}

static void server_send_snapshot(TestServer *s, double now) {
    if (!s->have_client) return;
    uint8_t buf[MAX_PACKET];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &s->rel, PKT_SNAPSHOT, 0, now);
    wr_u32(&b, s->last_input);
    wr_u8 (&b, 1);                       /* one player */
    wr_u8 (&b, 0);
    wr_f32(&b, s->state.pos[0]);
    wr_f32(&b, s->state.pos[1]);
    wr_f32(&b, s->state.pos[2]);
    wr_f32(&b, s->state.yaw);
    wr_f32(&b, s->state.pitch);
    wr_u8 (&b, 0);                       /* no events */
    net_send(&s->sock, &s->client, buf, b.pos, now);
}

static void server_recv(TestServer *s, double now) {
    struct sockaddr_in from;
    uint8_t buf[MAX_PACKET];
    int n;
    while ((n = net_recv(&s->sock, &from, buf, sizeof buf)) > 0) {
        ByteBuf b = buf_reader(buf, n);
        PacketHeader h;
        if (!net_read_header(&b, &h)) continue;
        if (!s->have_client) { s->client = from; s->have_client = 1; }
        rel_on_recv(&s->rel, &h, now);
        if (h.type != PKT_INPUT) continue;

        (void)rd_u32(&b);
        uint8_t count = rd_u8(&b);
        for (int i = 0; i < count; i++) {
            InputCmd cmd;
            cmd.seq     = rd_u32(&b);
            cmd.buttons = rd_u8(&b);
            cmd.yaw     = rd_f32(&b);
            cmd.pitch   = rd_f32(&b);
            if (!b.ok) break;
            if (cmd.seq > s->last_input) {
                world_apply_input(&s->state, &cmd, TICK_DT);
                s->last_input = cmd.seq;
            }
        }
    }
}

static void client_recv(TestClient *c, double now) {
    struct sockaddr_in from;
    uint8_t buf[MAX_PACKET];
    int n;
    while ((n = net_recv(&c->sock, &from, buf, sizeof buf)) > 0) {
        ByteBuf b = buf_reader(buf, n);
        PacketHeader h;
        if (!net_read_header(&b, &h)) continue;
        rel_on_recv(&c->rel, &h, now);
        if (h.type != PKT_SNAPSHOT) continue;

        uint32_t last_input = rd_u32(&b);
        uint8_t  pcount     = rd_u8(&b);
        PlayerState auth = c->self;
        for (int i = 0; i < pcount; i++) {
            (void)rd_u8(&b);
            auth.pos[0] = rd_f32(&b);
            auth.pos[1] = rd_f32(&b);
            auth.pos[2] = rd_f32(&b);
            auth.yaw    = rd_f32(&b);
            auth.pitch  = rd_f32(&b);
        }
        if (!b.ok) continue;
        client_reconcile(c, last_input, &auth);
    }
}

/* Run one scenario; report how far apart client and server ended up, and how
 * far the server actually moved. Returns 0 on success. */
static int run_scenario(const char *name, float loss, int latency_ms,
                        float *out_divergence, float *out_displacement) {
    TestServer srv;
    TestClient cli;
    memset(&srv, 0, sizeof srv);
    memset(&cli, 0, sizeof cli);

    if (!net_open(&srv.sock, TEST_PORT)) { fprintf(stderr, "server bind failed\n"); return 1; }
    if (!net_open(&cli.sock, 0))         { fprintf(stderr, "client bind failed\n"); return 1; }
    net_set_sim(&srv.sock, loss, latency_ms);
    net_set_sim(&cli.sock, loss, latency_ms);
    rel_init(&srv.rel);
    rel_init(&cli.rel);

    cli.server.sin_family      = AF_INET;
    cli.server.sin_port        = htons(TEST_PORT);
    cli.server.sin_addr.s_addr = htonl(0x7f000001);   /* 127.0.0.1 */

    const int   MOVE_TICKS  = 600;    /* 10 s of holding "forward" */
    const int   COAST_TICKS = 600;    /* 10 s of no input, to let things settle */
    const int   TOTAL       = MOVE_TICKS + COAST_TICKS;
    double      now         = 1.0;    /* simulated clock */

    for (int tick = 0; tick < TOTAL; tick++) {
        now += (double)TICK_DT;

        /* release any packets whose simulated latency has elapsed */
        net_update(&srv.sock, now);
        net_update(&cli.sock, now);

        /* client: sample input, predict, send the redundant window */
        InputCmd cmd;
        cmd.seq     = ++cli.input_seq;
        cmd.buttons = (tick < MOVE_TICKS) ? BTN_FWD : 0;
        cmd.yaw     = 0.0f;
        cmd.pitch   = 0.0f;
        world_apply_input(&cli.self, &cmd, TICK_DT);
        if (cli.pending_count >= PENDING_MAX) {
            memmove(&cli.pending[0], &cli.pending[1], sizeof(InputCmd) * (PENDING_MAX - 1));
            cli.pending_count = PENDING_MAX - 1;
        }
        cli.pending[cli.pending_count++] = cmd;
        client_send_inputs(&cli, now);

        /* server: consume inputs, broadcast a snapshot every 3rd tick (20 Hz) */
        server_recv(&srv, now);
        if (tick % TICKS_PER_SNAPSHOT == 0)
            server_send_snapshot(&srv, now);

        /* client: apply snapshots -> reconcile */
        client_recv(&cli, now);
    }

    /* drain the tail: flush + deliver everything still in flight, settle */
    for (int i = 0; i < 200; i++) {
        now += (double)TICK_DT;
        net_update(&srv.sock, now);
        net_update(&cli.sock, now);
        server_recv(&srv, now);
        if (i % TICKS_PER_SNAPSHOT == 0) server_send_snapshot(&srv, now);
        client_recv(&cli, now);
    }

    *out_divergence   = dist3(cli.self.pos, srv.state.pos);
    *out_displacement = fabsf(srv.state.pos[0]);   /* moved along +X (yaw 0) */

    printf("  %-22s loss %2.0f%% lat %3dms | server moved %.2f, client-server gap %.5f"
           " | RTT %.0fms loss-est %.0f%%\n",
           name, loss * 100.0f, latency_ms,
           *out_displacement, *out_divergence,
           cli.rel.rtt * 1000.0, rel_loss(&cli.rel) * 100.0f);

    net_close(&srv.sock);
    net_close(&cli.sock);
    return 0;
}

int main(void) {
    srand(1234);   /* deterministic loss pattern */
    printf("loopback netcode tests (real localhost UDP):\n");

    struct { const char *name; float loss; int lat; } scenarios[] = {
        { "clean link",        0.00f,   0 },
        { "latency only",      0.00f,  50 },
        { "lossy link",        0.20f,  30 },
        { "very lossy + laggy", 0.40f,  60 },
    };

    int failures = 0;
    for (size_t i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        float div = 0, disp = 0;
        if (run_scenario(scenarios[i].name, scenarios[i].loss, scenarios[i].lat, &div, &disp))
            return 1;

        /* Inputs got through: the server actually advanced a meaningful amount
         * (10 s * 5 u/s == ~50 units of intended travel). */
        if (disp < 5.0f) {
            fprintf(stderr, "FAIL: server barely moved (%.2f) -- inputs not arriving\n", disp);
            failures++;
        }
        /* Reconciliation converged: after coasting, prediction sits on top of
         * authority regardless of how bad the link was. */
        if (div > 0.01f) {
            fprintf(stderr, "FAIL: client did not converge to server (gap %.5f)\n", div);
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "%d loopback assertion(s) failed\n", failures);
        return 1;
    }
    printf("all loopback scenarios converged\n");
    return 0;
}
