#ifndef PULSE_SERIALIZE_H
#define PULSE_SERIALIZE_H

#include <stdint.h>
#include <string.h>
#include <assert.h>

/*
 * Tiny little-endian cursor over a byte buffer.
 *
 * Writers assert on overflow -- our outbound buffers are sized for the worst
 * case, so an overflow is a programming bug and must blow up loudly, not be
 * silently truncated. Readers operate on untrusted network data, so they
 * bounds-check and set `ok = 0` on underflow; callers must check `ok` before
 * trusting the decoded values.
 */

typedef struct {
    uint8_t *data;
    int      cap;
    int      pos;
    int      ok;    /* cleared by a read that ran past the end */
} ByteBuf;

static inline ByteBuf buf_writer(uint8_t *data, int cap) {
    ByteBuf b = { data, cap, 0, 1 };
    return b;
}

static inline ByteBuf buf_reader(const uint8_t *data, int len) {
    ByteBuf b = { (uint8_t *)data, len, 0, 1 };
    return b;
}

static inline void wr_u8(ByteBuf *b, uint8_t v) {
    assert(b->pos + 1 <= b->cap);
    b->data[b->pos++] = v;
}

static inline void wr_u16(ByteBuf *b, uint16_t v) {
    assert(b->pos + 2 <= b->cap);
    b->data[b->pos++] = (uint8_t)(v & 0xff);
    b->data[b->pos++] = (uint8_t)(v >> 8);
}

static inline void wr_u32(ByteBuf *b, uint32_t v) {
    assert(b->pos + 4 <= b->cap);
    b->data[b->pos++] = (uint8_t)(v & 0xff);
    b->data[b->pos++] = (uint8_t)((v >> 8) & 0xff);
    b->data[b->pos++] = (uint8_t)((v >> 16) & 0xff);
    b->data[b->pos++] = (uint8_t)((v >> 24) & 0xff);
}

static inline void wr_f32(ByteBuf *b, float v) {
    uint32_t u;
    memcpy(&u, &v, sizeof u);   /* type-pun via memcpy: no aliasing UB */
    wr_u32(b, u);
}

static inline uint8_t rd_u8(ByteBuf *b) {
    if (b->pos + 1 > b->cap) { b->ok = 0; return 0; }
    return b->data[b->pos++];
}

static inline uint16_t rd_u16(ByteBuf *b) {
    if (b->pos + 2 > b->cap) { b->ok = 0; return 0; }
    uint16_t v = (uint16_t)b->data[b->pos]
               | (uint16_t)((uint16_t)b->data[b->pos + 1] << 8);
    b->pos += 2;
    return v;
}

static inline uint32_t rd_u32(ByteBuf *b) {
    if (b->pos + 4 > b->cap) { b->ok = 0; return 0; }
    uint32_t v = (uint32_t)b->data[b->pos]
               | ((uint32_t)b->data[b->pos + 1] << 8)
               | ((uint32_t)b->data[b->pos + 2] << 16)
               | ((uint32_t)b->data[b->pos + 3] << 24);
    b->pos += 4;
    return v;
}

static inline float rd_f32(ByteBuf *b) {
    uint32_t u = rd_u32(b);
    float v;
    memcpy(&v, &u, sizeof v);
    return v;
}

#endif /* PULSE_SERIALIZE_H */
