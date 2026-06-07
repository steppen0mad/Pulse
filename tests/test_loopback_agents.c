/*
 * Integration test for the AI agent layer end to end.
 *
 *   1. Determinism + bounds: a headless rollout of stub agents, driven by the
 *      real agent_think + world_apply_input, stays inside the arena every tick
 *      and is bit-for-bit reproducible from a fixed seed.
 *   2. A real localhost client connects to a server populated with agents and
 *      decodes their positions out of ordinary snapshots -- exercising the same
 *      wire path the GL client uses, proving agents are "just players."
 *
 * Headless, deterministic, non-zero exit on any failed assertion.
 */
#include "net.h"
#include "world.h"
#include "agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TEST_PORT  42600
#define N_AGENTS   6

static int tests_run = 0;
#define CHECK(cond) do {                                                  \
        tests_run++;                                                      \
        if (!(cond)) {                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static int in_bounds(const float p[3]) {
    const float e = 1e-3f;
    return p[0] >= -ARENA_HALF_EXTENT - e && p[0] <= ARENA_HALF_EXTENT + e &&
           p[2] >= -ARENA_HALF_EXTENT - e && p[2] <= ARENA_HALF_EXTENT + e &&
           p[1] >= ARENA_FLOOR - e        && p[1] <= ARENA_CEIL + e;
}

/* ---- a small reusable agent rollout that mirrors the server's drive loop ---- */
typedef struct {
    World        world;
    PlayerState  state[MAX_CLIENTS];
    Agent        agent[MAX_CLIENTS];
    int          n;
} Rollout;

static void rollout_init(Rollout *r, int n, uint32_t seed_base) {
    memset(r, 0, sizeof(*r));
    r->n = n;
    r->world.dt = TICK_DT;
    for (int i = 0; i < n; i++) {
        r->state[i].pos[0] = (float)i * 2.0f - (float)n;
        r->state[i].pos[1] = EYE_HEIGHT;
        r->state[i].pos[2] = (float)(i % 5) * 2.0f;
        r->state[i].yaw    = -90.0f;
        r->world.present[i] = 1;
        memcpy(r->world.prev_pos[i], r->state[i].pos, sizeof(float) * 3);
        r->world.target[i][0] = 0.0f; r->world.target[i][1] = EYE_HEIGHT; r->world.target[i][2] = 0.0f;
        agent_init(&r->agent[i], AGENT_STUB_RANDOM, NULL, 4, -90.0f, 0.0f, seed_base ^ (uint32_t)(i + 1));
    }
}

/* advance one tick; returns 0 ok, 1 if any agent left the arena */
static int rollout_tick(Rollout *r) {
    r->world.dt = TICK_DT;
    for (int i = 0; i < r->n; i++) r->world.players[i] = r->state[i];
    for (int i = 0; i < r->n; i++) {
        InputCmd cmd;
        agent_think(&r->agent[i], &r->world, i, &cmd);
        world_apply_input(&r->state[i], &cmd, TICK_DT);
        if (!in_bounds(r->state[i].pos)) return 1;
    }
    for (int i = 0; i < r->n; i++)
        memcpy(r->world.prev_pos[i], r->world.players[i].pos, sizeof(float) * 3);
    return 0;
}

static int test_determinism_and_bounds(void) {
    Rollout a, b;
    rollout_init(&a, N_AGENTS, 0xABCD1234u);
    rollout_init(&b, N_AGENTS, 0xABCD1234u);

    for (int t = 0; t < 1800; t++) {           /* 30 s at 60 Hz */
        CHECK(rollout_tick(&a) == 0);           /* never leaves the arena */
        CHECK(rollout_tick(&b) == 0);
    }
    /* identical seed -> identical trajectories, bit for bit */
    for (int i = 0; i < N_AGENTS; i++)
        for (int k = 0; k < 3; k++)
            CHECK(a.state[i].pos[k] == b.state[i].pos[k]);
    printf("  %d stub agents stay in-arena for 1800 ticks, reproducible from seed ... ok\n", N_AGENTS);
    return 0;
}

/* ---- a minimal server that drives agents and snapshots them to one client ---- */
typedef struct {
    UdpSocket          sock;
    Reliable           rel;
    struct sockaddr_in client;
    int                have_client;
    int                client_slot;
    Rollout            roll;
} MiniServer;

static void mini_send_snapshot(MiniServer *s, double now) {
    if (!s->have_client) return;
    uint8_t buf[MAX_PACKET];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &s->rel, PKT_SNAPSHOT, 0, now);
    wr_u32(&b, 0);                               /* last_input (client unused here) */
    int count = s->roll.n + (s->have_client ? 1 : 0);
    wr_u8(&b, (uint8_t)count);
    for (int i = 0; i < s->roll.n; i++) {
        wr_u8 (&b, (uint8_t)i);
        wr_f32(&b, s->roll.state[i].pos[0]);
        wr_f32(&b, s->roll.state[i].pos[1]);
        wr_f32(&b, s->roll.state[i].pos[2]);
        wr_f32(&b, s->roll.state[i].yaw);
        wr_f32(&b, s->roll.state[i].pitch);
    }
    if (s->have_client) {                        /* the human's own slot */
        wr_u8 (&b, (uint8_t)s->client_slot);
        wr_f32(&b, 0.0f); wr_f32(&b, EYE_HEIGHT); wr_f32(&b, 0.0f);
        wr_f32(&b, 0.0f); wr_f32(&b, 0.0f);
    }
    wr_u8(&b, 0);                                /* no events */
    net_send(&s->sock, &s->client, buf, b.pos, now);
}

static void mini_server_recv(MiniServer *s, double now) {
    struct sockaddr_in from;
    uint8_t buf[MAX_PACKET];
    int n;
    while ((n = net_recv(&s->sock, &from, buf, sizeof buf)) > 0) {
        ByteBuf b = buf_reader(buf, n);
        PacketHeader h;
        if (!net_read_header(&b, &h)) continue;
        if (h.type == PKT_CONNECT && !s->have_client) {
            s->client = from; s->have_client = 1; s->client_slot = s->roll.n;
            uint8_t ab[MAX_PACKET];
            ByteBuf w = buf_writer(ab, sizeof ab);
            net_write_header(&w, &s->rel, PKT_ACCEPT, 0, now);
            wr_u8(&w, (uint8_t)s->client_slot);
            net_send(&s->sock, &s->client, ab, w.pos, now);
        }
        rel_on_recv(&s->rel, &h, now);
    }
}

static int test_client_sees_agents(void) {
    MiniServer srv;
    memset(&srv, 0, sizeof srv);
    rollout_init(&srv.roll, N_AGENTS, 0x55AA55AAu);
    if (!net_open(&srv.sock, TEST_PORT)) { fprintf(stderr, "server bind failed\n"); return 1; }
    rel_init(&srv.rel);

    UdpSocket csock; Reliable crel;
    if (!net_open(&csock, 0)) { fprintf(stderr, "client bind failed\n"); return 1; }
    rel_init(&crel);
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port   = htons(TEST_PORT);
    server.sin_addr.s_addr = htonl(0x7f000001);

    int my_id = -1, snapshots = 0, max_players = 0, parse_failures = 0, oob = 0;
    double now = 1.0;

    for (int tick = 0; tick < 900; tick++) {
        now += (double)TICK_DT;
        net_update(&srv.sock, now);
        net_update(&csock, now);

        /* client handshake */
        if (my_id < 0 && (tick % 15 == 0)) {
            uint8_t cb[MAX_PACKET];
            ByteBuf w = buf_writer(cb, sizeof cb);
            net_write_header(&w, &crel, PKT_CONNECT, 0, now);
            net_send(&csock, &server, cb, w.pos, now);
        }

        mini_server_recv(&srv, now);

        /* server: advance agents + assert bounds, snapshot at 20 Hz */
        srv.roll.world.dt = TICK_DT;
        for (int i = 0; i < srv.roll.n; i++) srv.roll.world.players[i] = srv.roll.state[i];
        for (int i = 0; i < srv.roll.n; i++) {
            InputCmd cmd;
            agent_think(&srv.roll.agent[i], &srv.roll.world, i, &cmd);
            world_apply_input(&srv.roll.state[i], &cmd, TICK_DT);
            if (!in_bounds(srv.roll.state[i].pos)) oob++;
        }
        for (int i = 0; i < srv.roll.n; i++)
            memcpy(srv.roll.world.prev_pos[i], srv.roll.world.players[i].pos, sizeof(float) * 3);
        if (tick % TICKS_PER_SNAPSHOT == 0) mini_send_snapshot(&srv, now);

        /* client: drain, parse snapshots */
        struct sockaddr_in from;
        uint8_t buf[MAX_PACKET];
        int n;
        while ((n = net_recv(&csock, &from, buf, sizeof buf)) > 0) {
            ByteBuf b = buf_reader(buf, n);
            PacketHeader h;
            if (!net_read_header(&b, &h)) continue;
            rel_on_recv(&crel, &h, now);
            if (h.type == PKT_ACCEPT) { my_id = rd_u8(&b); continue; }
            if (h.type != PKT_SNAPSHOT) continue;

            (void)rd_u32(&b);
            uint8_t pcount = rd_u8(&b);
            int agents_seen = 0;
            for (int i = 0; i < pcount; i++) {
                uint8_t id = rd_u8(&b);
                PlayerState ps;
                ps.pos[0] = rd_f32(&b); ps.pos[1] = rd_f32(&b); ps.pos[2] = rd_f32(&b);
                ps.yaw = rd_f32(&b); ps.pitch = rd_f32(&b);
                if (!b.ok) { parse_failures++; break; }
                if (id < N_AGENTS) { agents_seen++; if (!in_bounds(ps.pos)) oob++; }
            }
            uint8_t ec = rd_u8(&b);
            for (int e = 0; e < ec; e++) { (void)rd_u32(&b); (void)rd_u8(&b); (void)rd_u8(&b); }
            if (!b.ok) parse_failures++;
            if (agents_seen > max_players) max_players = agents_seen;
            snapshots++;
        }
    }

    net_close(&srv.sock);
    net_close(&csock);

    CHECK(my_id == N_AGENTS);          /* client got the slot after the agents */
    CHECK(snapshots > 10);             /* snapshots actually flowed             */
    CHECK(parse_failures == 0);        /* every snapshot decoded cleanly        */
    CHECK(max_players == N_AGENTS);    /* the client saw all agents             */
    CHECK(oob == 0);                   /* nobody ever left the arena            */
    printf("  real localhost client decoded %d agents over %d snapshots, all in-bounds ... ok\n",
           max_players, snapshots);
    return 0;
}

int main(void) {
    srand(1);
    printf("agent loopback tests:\n");
    if (test_determinism_and_bounds()) return 1;
    if (test_client_sees_agents())     return 1;
    printf("all %d agent-layer checks passed\n", tests_run);
    return 0;
}
