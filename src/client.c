/*
 * Pulse client: input, client-side prediction, server reconciliation, remote
 * entity interpolation, and rendering.
 *
 * Three netcode ideas live here:
 *   - PREDICTION: the local player's inputs are applied immediately to a local
 *     copy of its state, so movement feels instant despite network latency.
 *   - RECONCILIATION: when a snapshot arrives it carries the id of the last
 *     input the server processed for us. We snap to that authoritative state
 *     and replay every input the server hasn't acked yet -- so a misprediction
 *     is silently corrected without rubber-banding the inputs already shown.
 *   - INTERPOLATION: other players are rendered INTERP_DELAY (100 ms) in the
 *     past, smoothly interpolated between the two snapshots that bracket that
 *     render time, which hides packet jitter and loss.
 */
#include "net.h"
#include "world.h"
#include "camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <arpa/inet.h>

#include <GLFW/glfw3.h>
#include <GL/gl.h>
#include <GL/glu.h>

#define WIN_W 1280
#define WIN_H 720
#define PENDING_MAX 256
#define SAMPLE_MAX  64

/* ---- networking state ---- */
static UdpSocket          g_sock;
static Reliable           g_rel;
static struct sockaddr_in g_server;
static int                g_my_id = -1;

/* ---- prediction / reconciliation ---- */
static PlayerState g_self;                 /* locally predicted state */
static uint32_t    g_input_seq = 0;
static InputCmd    g_pending[PENDING_MAX]; /* inputs not yet acked by the server */
static int         g_pending_count = 0;
static uint32_t    g_server_acked_input = 0;

/* ---- remote players + interpolation ---- */
typedef struct { double t; PlayerState s; } Sample;
typedef struct {
    int    present;
    int    count;
    Sample buf[SAMPLE_MAX];
} RemoteEntity;
static RemoteEntity g_remote[MAX_CLIENTS];

/* ---- events / HUD ---- */
static uint32_t g_last_event_id = 0;
static uint32_t g_last_snap_tick = 0;
static int      g_player_count = 0;

static Camera g_camera;

/* =========================================================================
 * Small math helpers
 * ====================================================================== */
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static float lerp_angle(float a, float b, float t) {
    float d = fmodf(b - a, 360.0f);
    if (d < -180.0f) d += 360.0f;
    if (d >  180.0f) d -= 360.0f;
    return a + d * t;
}

/* =========================================================================
 * Pending-input ring (oldest first)
 * ====================================================================== */
static void pending_push(const InputCmd *cmd) {
    if (g_pending_count >= PENDING_MAX) {
        memmove(&g_pending[0], &g_pending[1], sizeof(InputCmd) * (PENDING_MAX - 1));
        g_pending_count = PENDING_MAX - 1;
    }
    g_pending[g_pending_count++] = *cmd;
}

static void pending_drop_acked(uint32_t acked_seq) {
    int keep = 0;
    while (keep < g_pending_count && g_pending[keep].seq <= acked_seq)
        keep++;
    if (keep > 0) {
        memmove(&g_pending[0], &g_pending[keep], sizeof(InputCmd) * (g_pending_count - keep));
        g_pending_count -= keep;
    }
}

/* =========================================================================
 * Interpolation
 * ====================================================================== */
static void remote_push(int id, double t, const PlayerState *s) {
    RemoteEntity *e = &g_remote[id];
    if (e->count >= SAMPLE_MAX) {
        memmove(&e->buf[0], &e->buf[1], sizeof(Sample) * (SAMPLE_MAX - 1));
        e->count = SAMPLE_MAX - 1;
    }
    e->buf[e->count].t = t;
    e->buf[e->count].s = *s;
    e->count++;
}

/* Reconstruct a remote player's state at render_time by interpolating between
 * the two buffered snapshots that bracket it. Clamps (never extrapolates) at
 * the ends of the buffer. */
static int remote_sample(int id, double render_time, PlayerState *out) {
    RemoteEntity *e = &g_remote[id];
    if (!e->present || e->count == 0) return 0;
    if (render_time <= e->buf[0].t)            { *out = e->buf[0].s; return 1; }
    if (render_time >= e->buf[e->count - 1].t) { *out = e->buf[e->count - 1].s; return 1; }

    for (int i = 0; i < e->count - 1; i++) {
        double t0 = e->buf[i].t, t1 = e->buf[i + 1].t;
        if (render_time >= t0 && render_time <= t1) {
            float a = (t1 > t0) ? (float)((render_time - t0) / (t1 - t0)) : 0.0f;
            const PlayerState *s0 = &e->buf[i].s, *s1 = &e->buf[i + 1].s;
            out->pos[0] = lerpf(s0->pos[0], s1->pos[0], a);
            out->pos[1] = lerpf(s0->pos[1], s1->pos[1], a);
            out->pos[2] = lerpf(s0->pos[2], s1->pos[2], a);
            out->yaw    = lerp_angle(s0->yaw,   s1->yaw,   a);
            out->pitch  = lerp_angle(s0->pitch, s1->pitch, a);
            return 1;
        }
    }
    *out = e->buf[e->count - 1].s;
    return 1;
}

/* =========================================================================
 * Packet handling
 * ====================================================================== */
static void handle_snapshot(ByteBuf *b, double now) {
    uint32_t last_input = rd_u32(b);
    uint8_t  pcount     = rd_u8(b);

    PlayerState auth_self;
    int have_self = 0;
    int seen[MAX_CLIENTS] = {0};

    for (int i = 0; i < pcount; i++) {
        uint8_t id = rd_u8(b);
        PlayerState s;
        s.pos[0] = rd_f32(b);
        s.pos[1] = rd_f32(b);
        s.pos[2] = rd_f32(b);
        s.yaw    = rd_f32(b);
        s.pitch  = rd_f32(b);
        if (!b->ok) return;

        if (id == (uint8_t)g_my_id) {
            auth_self = s;
            have_self = 1;
        } else if (id < MAX_CLIENTS) {
            seen[id] = 1;
            g_remote[id].present = 1;
            remote_push(id, now, &s);
        }
    }
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (!seen[i] && i != g_my_id) g_remote[i].present = 0;

    /* events (deduped by monotonically increasing id) */
    uint8_t ecount = rd_u8(b);
    for (int i = 0; i < ecount; i++) {
        uint32_t eid    = rd_u32(b);
        uint8_t  etype  = rd_u8(b);
        uint8_t  player = rd_u8(b);
        if (!b->ok) return;
        if (eid > g_last_event_id) {
            g_last_event_id = eid;
            printf("[client] player %u %s\n", player,
                   etype == EV_PLAYER_JOIN ? "joined" : "left");
        }
    }

    g_player_count = pcount;   /* g_last_snap_tick is set by the caller from the header */

    /* reconciliation */
    if (have_self) {
        g_self = auth_self;
        pending_drop_acked(last_input);
        for (int i = 0; i < g_pending_count; i++)
            world_apply_input(&g_self, &g_pending[i], TICK_DT);
        g_server_acked_input = last_input;
    }
}

static void handle_packet(uint8_t *data, int len, double now) {
    ByteBuf b = buf_reader(data, len);
    PacketHeader h;
    if (!net_read_header(&b, &h)) return;

    rel_on_recv(&g_rel, &h, now);

    switch (h.type) {
        case PKT_ACCEPT: {
            uint8_t id = rd_u8(&b);
            if (!b.ok) return;
            if (g_my_id < 0) {
                g_my_id = id;
                printf("[client] connected as player %d\n", g_my_id);
            }
            break;
        }
        case PKT_SNAPSHOT:
            handle_snapshot(&b, now);
            g_last_snap_tick = h.tick;
            break;
        case PKT_HEARTBEAT:
            break;
        default:
            break;
    }
}

/* =========================================================================
 * Sending
 * ====================================================================== */
static void send_connect(double now) {
    uint8_t buf[HEADER_SIZE];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &g_rel, PKT_CONNECT, 0, now);
    net_send(&g_sock, &g_server, buf, b.pos, now);
}

/* Send the whole window of unacked inputs (capped). Resending them is what
 * makes a single lost datagram a non-event: the next packet carries the same
 * commands again until the server acks them. */
static void send_inputs(double now) {
    uint8_t buf[MAX_PACKET];
    ByteBuf b = buf_writer(buf, sizeof buf);
    net_write_header(&b, &g_rel, PKT_INPUT, g_input_seq, now);

    int count = g_pending_count;
    if (count > MAX_INPUTS_PER_PKT) count = MAX_INPUTS_PER_PKT;
    int start = g_pending_count - count;   /* most recent `count` commands */

    wr_u32(&b, (uint32_t)g_my_id);
    wr_u8 (&b, (uint8_t)count);
    for (int i = start; i < g_pending_count; i++) {
        wr_u32(&b, g_pending[i].seq);
        wr_u8 (&b, g_pending[i].buttons);
        wr_f32(&b, g_pending[i].yaw);
        wr_f32(&b, g_pending[i].pitch);
    }
    net_send(&g_sock, &g_server, buf, b.pos, now);
}

/* =========================================================================
 * Input sampling + GLFW callbacks
 * ====================================================================== */
static uint8_t sample_buttons(GLFWwindow *w) {
    uint8_t btn = 0;
    if (glfwGetKey(w, GLFW_KEY_W)          == GLFW_PRESS) btn |= BTN_FWD;
    if (glfwGetKey(w, GLFW_KEY_S)          == GLFW_PRESS) btn |= BTN_BACK;
    if (glfwGetKey(w, GLFW_KEY_A)          == GLFW_PRESS) btn |= BTN_LEFT;
    if (glfwGetKey(w, GLFW_KEY_D)          == GLFW_PRESS) btn |= BTN_RIGHT;
    if (glfwGetKey(w, GLFW_KEY_SPACE)      == GLFW_PRESS) btn |= BTN_UP;
    if (glfwGetKey(w, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS) btn |= BTN_DOWN;
    return btn;
}

static void mouse_callback(GLFWwindow *w, double xpos, double ypos) {
    (void)w;
    camera_on_mouse(&g_camera, xpos, ypos);
}

/* =========================================================================
 * Rendering
 * ====================================================================== */
static void draw_grid(void) {
    glColor3f(0.3f, 0.3f, 0.3f);
    glBegin(GL_LINES);
    for (int i = -20; i <= 20; i++) {
        glVertex3f((float)i, 0, -20); glVertex3f((float)i, 0, 20);
        glVertex3f(-20, 0, (float)i); glVertex3f(20, 0, (float)i);
    }
    glEnd();
}

static void draw_cube(float x, float y, float z, float size, float r, float g, float bl) {
    float h = size / 2.0f;
    glColor3f(r, g, bl);
    glBegin(GL_QUADS);
    glVertex3f(x-h,y-h,z+h); glVertex3f(x+h,y-h,z+h); glVertex3f(x+h,y+h,z+h); glVertex3f(x-h,y+h,z+h);
    glVertex3f(x-h,y-h,z-h); glVertex3f(x-h,y+h,z-h); glVertex3f(x+h,y+h,z-h); glVertex3f(x+h,y-h,z-h);
    glVertex3f(x-h,y+h,z-h); glVertex3f(x-h,y+h,z+h); glVertex3f(x+h,y+h,z+h); glVertex3f(x+h,y+h,z-h);
    glVertex3f(x-h,y-h,z-h); glVertex3f(x+h,y-h,z-h); glVertex3f(x+h,y-h,z+h); glVertex3f(x-h,y-h,z+h);
    glVertex3f(x+h,y-h,z-h); glVertex3f(x+h,y+h,z-h); glVertex3f(x+h,y+h,z+h); glVertex3f(x+h,y-h,z+h);
    glVertex3f(x-h,y-h,z-h); glVertex3f(x-h,y-h,z+h); glVertex3f(x-h,y+h,z+h); glVertex3f(x-h,y+h,z-h);
    glEnd();
}

/* A connection-quality swatch in the corner: green/yellow/red by packet loss. */
static void draw_hud_overlay(float loss) {
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, WIN_W, 0, WIN_H, -1, 1);
    glMatrixMode(GL_MODELVIEW);  glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);

    if      (loss < 0.05f) glColor3f(0.1f, 0.8f, 0.1f);
    else if (loss < 0.20f) glColor3f(0.9f, 0.8f, 0.1f);
    else                   glColor3f(0.9f, 0.1f, 0.1f);
    glBegin(GL_QUADS);
    glVertex2f(WIN_W - 36, WIN_H - 36); glVertex2f(WIN_W - 12, WIN_H - 36);
    glVertex2f(WIN_W - 12, WIN_H - 12); glVertex2f(WIN_W - 36, WIN_H - 12);
    glEnd();

    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
}

static void render(void) {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    float front[3];
    camera_front(&g_camera, front);
    gluLookAt(g_self.pos[0], g_self.pos[1], g_self.pos[2],
              g_self.pos[0] + front[0], g_self.pos[1] + front[1], g_self.pos[2] + front[2],
              0.0f, 1.0f, 0.0f);

    draw_grid();
    /* static sandbox props */
    draw_cube(0, 1, 0, 2, 0.8f, 0.3f, 0.3f);
    draw_cube(5, 1, 3, 1.5f, 0.3f, 0.8f, 0.3f);
    draw_cube(-3, 0.5f, -5, 1, 0.3f, 0.3f, 0.8f);

    /* other players, interpolated 100 ms in the past */
    double render_time = now_seconds() - INTERP_DELAY;
    for (int id = 0; id < MAX_CLIENTS; id++) {
        if (id == g_my_id || !g_remote[id].present) continue;
        PlayerState s;
        if (!remote_sample(id, render_time, &s)) continue;
        float r = 0.4f + 0.6f * ((id * 53 % 7) / 7.0f);
        float g = 0.4f + 0.6f * ((id * 97 % 5) / 5.0f);
        float bl = 0.9f;
        draw_cube(s.pos[0], s.pos[1], s.pos[2], 1.0f, r, g, bl);
    }
}

/* =========================================================================
 * main
 * ====================================================================== */
int main(int argc, char **argv) {
    const char *host    = "127.0.0.1";
    uint16_t    port    = PULSE_DEFAULT_PORT;
    float       loss    = 0.0f;
    int         latency = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--loss")    && i + 1 < argc) loss    = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latency = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--port")    && i + 1 < argc) port    = (uint16_t)atoi(argv[++i]);
        else if (argv[i][0] != '-')                             host    = argv[i];
        else { fprintf(stderr, "usage: %s [host] [--port N] [--loss 0..1] [--latency MS]\n", argv[0]); return 2; }
    }

    srand((unsigned)time(NULL));

    /* socket + server address */
    if (!net_open(&g_sock, 0)) return 1;     /* ephemeral local port */
    net_set_sim(&g_sock, loss, latency);
    rel_init(&g_rel);

    memset(&g_server, 0, sizeof g_server);
    g_server.sin_family = AF_INET;
    g_server.sin_port   = htons(port);
    if (inet_pton(AF_INET, host, &g_server.sin_addr) != 1) {
        fprintf(stderr, "[client] invalid host address: %s\n", host);
        return 1;
    }

    camera_init(&g_camera);

    /* GL window */
    if (!glfwInit()) { fprintf(stderr, "[client] glfwInit failed\n"); return 1; }
    GLFWwindow *window = glfwCreateWindow(WIN_W, WIN_H, "Pulse", NULL, NULL);
    if (!window) { fprintf(stderr, "[client] window creation failed\n"); glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(window, mouse_callback);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0, (double)WIN_W / (double)WIN_H, 0.1, 100.0);

    printf("[client] connecting to %s:%u%s ...\n", host, port,
           (loss > 0.0f || latency > 0) ? " (with simulated network impairment)" : "");

    double now            = now_seconds();
    double last_connect   = 0.0;
    double input_accum    = 0.0;
    double last_frame     = now;
    double connect_start  = now;
    double last_hud       = now;
    double fps            = 0.0;

    while (!glfwWindowShouldClose(window)) {
        now = now_seconds();
        double dt = now - last_frame;
        last_frame = now;
        if (dt > 0.0) fps = 1.0 / dt;

        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, 1);

        net_update(&g_sock, now);

        /* drain inbound */
        struct sockaddr_in from;
        uint8_t buf[MAX_PACKET];
        int n;
        while ((n = net_recv(&g_sock, &from, buf, sizeof buf)) > 0)
            handle_packet(buf, n, now);

        if (g_my_id < 0) {
            /* still handshaking */
            if (now - last_connect > 0.25) { send_connect(now); last_connect = now; }
            if (now - connect_start > CONNECT_TIMEOUT) {
                fprintf(stderr, "[client] no response from server after %.0fs, giving up\n",
                        CONNECT_TIMEOUT);
                break;
            }
        } else {
            /* fixed-rate input sampling, prediction, and (redundant) send */
            input_accum += dt;
            int guard = 0;
            while (input_accum >= (double)TICK_DT && guard++ < 8) {
                input_accum -= (double)TICK_DT;

                InputCmd cmd;
                cmd.seq     = ++g_input_seq;
                cmd.buttons = sample_buttons(window);
                cmd.yaw     = g_camera.yaw;
                cmd.pitch   = g_camera.pitch;

                world_apply_input(&g_self, &cmd, TICK_DT);   /* prediction */
                pending_push(&cmd);
                send_inputs(now);
            }
        }

        render();
        draw_hud_overlay(rel_loss(&g_rel));
        glfwSwapBuffers(window);

        /* live network HUD in the title bar */
        if (now - last_hud >= 0.25) {
            last_hud = now;
            char title[256];
            snprintf(title, sizeof title,
                     "Pulse | id %d | RTT %.0f ms | loss %.0f%% | snap tick %u | players %d | %.0f fps",
                     g_my_id, g_rel.rtt * 1000.0, rel_loss(&g_rel) * 100.0,
                     g_last_snap_tick, g_player_count, fps);
            glfwSetWindowTitle(window, title);
        }
    }

    /* tell the server we're leaving (best effort) */
    if (g_my_id >= 0) {
        uint8_t buf[HEADER_SIZE];
        ByteBuf b = buf_writer(buf, sizeof buf);
        net_write_header(&b, &g_rel, PKT_DISCONNECT, 0, now_seconds());
        net_send(&g_sock, &g_server, buf, b.pos, now_seconds());
        net_update(&g_sock, now_seconds() + 1.0);   /* flush any delayed packets */
    }

    net_close(&g_sock);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
