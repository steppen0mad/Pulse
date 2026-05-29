/*
 * Pulse authoritative server (headless).
 *
 * Runs a fixed 60 Hz simulation. Clients send input commands; the server is the
 * single source of truth for position. Twenty times a second it broadcasts a
 * snapshot of every player's authoritative state, plus the id of the last input
 * it processed for that client (which the client needs for reconciliation).
 * Join/leave events ride along on snapshots. Peers that go silent time out.
 */
#include "net.h"
#include "world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>

typedef struct {
    int                in_use;
    struct sockaddr_in addr;
    Reliable           rel;
    PlayerState        state;
    uint32_t           last_input;   /* highest input seq we have applied */
} Client;

typedef struct { uint32_t id; uint8_t type; uint8_t player; } Event;

typedef struct {
    UdpSocket sock;
    Client    clients[MAX_CLIENTS];
    uint32_t  tick;

    /* The most recent events, resent on every snapshot until pushed out by
     * newer ones. Clients dedupe by id, so this is reliable enough for the
     * rare join/leave traffic without a per-event ack protocol. */
    Event     recent[MAX_EVENTS_PER_PKT];
    int       recent_count;
    uint32_t  next_event_id;
} Server;

static Server         g;
static volatile sig_atomic_t g_running = 1;

static void on_sigint(int sig) { (void)sig; g_running = 0; }

static void nap_us(long us) {
    struct timespec ts = { us / 1000000L, (us % 1000000L) * 1000L };
    nanosleep(&ts, NULL);
}

static int addr_eq(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static int find_client(const struct sockaddr_in *addr) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g.clients[i].in_use && addr_eq(&g.clients[i].addr, addr))
            return i;
    return -1;
}

static void push_event(uint8_t type, uint8_t player) {
    Event e = { g.next_event_id++, type, player };
    if (g.recent_count < MAX_EVENTS_PER_PKT) {
        g.recent[g.recent_count++] = e;
    } else {
        memmove(&g.recent[0], &g.recent[1], sizeof(Event) * (MAX_EVENTS_PER_PKT - 1));
        g.recent[MAX_EVENTS_PER_PKT - 1] = e;
    }
}

static void send_accept(int slot, double now) {
    uint8_t buf[MAX_PACKET];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &g.clients[slot].rel, PKT_ACCEPT, g.tick, now);
    wr_u8(&b, (uint8_t)slot);
    net_send(&g.sock, &g.clients[slot].addr, buf, b.pos, now);
}

static void send_snapshot(int slot, double now) {
    uint8_t buf[MAX_PACKET];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &g.clients[slot].rel, PKT_SNAPSHOT, g.tick, now);

    wr_u32(&b, g.clients[slot].last_input);   /* drives this client's reconciliation */

    int count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (g.clients[i].in_use) count++;
    wr_u8(&b, (uint8_t)count);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g.clients[i].in_use) continue;
        wr_u8 (&b, (uint8_t)i);
        wr_f32(&b, g.clients[i].state.pos[0]);
        wr_f32(&b, g.clients[i].state.pos[1]);
        wr_f32(&b, g.clients[i].state.pos[2]);
        wr_f32(&b, g.clients[i].state.yaw);
        wr_f32(&b, g.clients[i].state.pitch);
    }

    wr_u8(&b, (uint8_t)g.recent_count);
    for (int i = 0; i < g.recent_count; i++) {
        wr_u32(&b, g.recent[i].id);
        wr_u8 (&b, g.recent[i].type);
        wr_u8 (&b, g.recent[i].player);
    }

    net_send(&g.sock, &g.clients[slot].addr, buf, b.pos, now);
}

static void send_heartbeat(int slot, double now) {
    uint8_t buf[HEADER_SIZE];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &g.clients[slot].rel, PKT_HEARTBEAT, g.tick, now);
    net_send(&g.sock, &g.clients[slot].addr, buf, b.pos, now);
}

static void drop_client(int slot) {
    printf("[server] client %d disconnected\n", slot);
    g.clients[slot].in_use = 0;
    push_event(EV_PLAYER_LEAVE, (uint8_t)slot);
}

static void handle_connect(const struct sockaddr_in *from, double now) {
    int slot = find_client(from);
    if (slot >= 0) {           /* duplicate CONNECT (our ACCEPT was lost) -- resend */
        send_accept(slot, now);
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g.clients[i].in_use) continue;
        memset(&g.clients[i], 0, sizeof(Client));
        g.clients[i].in_use = 1;
        g.clients[i].addr   = *from;
        rel_init(&g.clients[i].rel);
        g.clients[i].rel.last_recv = now;
        /* spawn offset by slot so players don't pile up on connect */
        g.clients[i].state.pos[0] = (float)i * 2.0f;
        g.clients[i].state.pos[1] = 1.7f;
        g.clients[i].state.pos[2] = 5.0f;
        g.clients[i].state.yaw    = -90.0f;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from->sin_addr, ip, sizeof ip);
        printf("[server] client %d connected from %s:%d\n",
               i, ip, ntohs(from->sin_port));

        send_accept(i, now);
        push_event(EV_PLAYER_JOIN, (uint8_t)i);
        return;
    }
    fprintf(stderr, "[server] connection refused: server full\n");
}

static void handle_input(int slot, ByteBuf *b) {
    (void)rd_u32(b);                 /* client_id: informational, we key on addr */
    uint8_t count = rd_u8(b);
    for (int i = 0; i < count; i++) {
        InputCmd cmd;
        cmd.seq     = rd_u32(b);
        cmd.buttons = rd_u8(b);
        cmd.yaw     = rd_f32(b);
        cmd.pitch   = rd_f32(b);
        if (!b->ok) return;          /* truncated/garbage packet: stop */
        /* Commands arrive oldest-first and the client resends unacked ones, so
         * we apply only those past what we have already processed. Input seqs
         * are 32-bit and monotonic per session, so a plain comparison is safe. */
        if (cmd.seq > g.clients[slot].last_input) {
            world_apply_input(&g.clients[slot].state, &cmd, TICK_DT);
            g.clients[slot].last_input = cmd.seq;
        }
    }
}

static void handle_packet(uint8_t *data, int len, const struct sockaddr_in *from, double now) {
    ByteBuf b = buf_reader(data, len);
    PacketHeader h;
    if (!net_read_header(&b, &h))
        return;                      /* foreign or truncated datagram */

    if (h.type == PKT_CONNECT) {
        handle_connect(from, now);
        return;
    }

    int slot = find_client(from);
    if (slot < 0)
        return;                      /* unknown peer that isn't connecting */

    rel_on_recv(&g.clients[slot].rel, &h, now);

    switch (h.type) {
        case PKT_INPUT:      handle_input(slot, &b); break;
        case PKT_HEARTBEAT:  break;  /* last_recv already refreshed */
        case PKT_DISCONNECT: drop_client(slot);      break;
        default: break;
    }
}

int main(int argc, char **argv) {
    uint16_t port    = PULSE_DEFAULT_PORT;
    float    loss    = 0.0f;
    int      latency = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)         port    = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loss") && i + 1 < argc)    loss    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latency = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--port N] [--loss 0..1] [--latency MS]\n", argv[0]); return 2; }
    }

    signal(SIGINT, on_sigint);
    srand((unsigned)time(NULL));

    memset(&g, 0, sizeof g);
    g.next_event_id = 1;
    if (!net_open(&g.sock, port)) {
        fprintf(stderr, "[server] failed to bind port %u\n", port);
        return 1;
    }
    net_set_sim(&g.sock, loss, latency);

    printf("[server] Pulse authoritative server on UDP %u (%d Hz sim, %d Hz snapshots)\n",
           port, TICK_RATE, SNAPSHOT_RATE);
    if (loss > 0.0f || latency > 0)
        printf("[server] simulating %.0f%% loss, %d ms latency\n", loss * 100.0f, latency);

    double tick_time   = now_seconds();
    double last_status = tick_time;
    uint8_t buf[MAX_PACKET];

    while (g_running) {
        double now = now_seconds();
        net_update(&g.sock, now);

        /* drain all pending datagrams */
        struct sockaddr_in from;
        int n;
        while ((n = net_recv(&g.sock, &from, buf, sizeof buf)) > 0)
            handle_packet(buf, n, &from, now);

        /* fixed-timestep ticks */
        while (now - tick_time >= (double)TICK_DT) {
            tick_time += (double)TICK_DT;
            g.tick++;

            for (int i = 0; i < MAX_CLIENTS; i++)
                if (g.clients[i].in_use && now - g.clients[i].rel.last_recv > CONNECT_TIMEOUT)
                    drop_client(i);

            if (g.tick % TICKS_PER_SNAPSHOT == 0) {
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (g.clients[i].in_use) send_snapshot(i, now);
            } else {
                for (int i = 0; i < MAX_CLIENTS; i++)
                    if (g.clients[i].in_use &&
                        now - g.clients[i].rel.last_send > HEARTBEAT_INTERVAL)
                        send_heartbeat(i, now);
            }
        }

        if (now - last_status >= 1.0) {
            last_status = now;
            int n_conn = 0;
            for (int i = 0; i < MAX_CLIENTS; i++) if (g.clients[i].in_use) n_conn++;
            printf("[server] tick %u | %d player(s)\n", g.tick, n_conn);
            fflush(stdout);
        }

        nap_us(500);   /* yield; we are far from saturating a 60 Hz schedule */
    }

    printf("\n[server] shutting down\n");
    net_close(&g.sock);
    return 0;
}
