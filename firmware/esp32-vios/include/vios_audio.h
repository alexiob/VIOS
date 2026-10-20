/* VIOS lean audio engine (docs/superpowers/specs/2026-10-20-vios-audio.md). MIT.
 *
 * Three voices synthesized one sample at a time from the sound ISR, with note
 * timing counted in samples: no task, no timer, no internal-RAM heap. Tunes are
 * stored as text (VIOS tune notation, see the spec) and parsed one note at a
 * time when a voice's event counter runs out. Plain C++11 so the host tests
 * compile it without fabgl; callers serialize engine calls (vios_audio_vdp.h
 * holds one spinlock around the sampler and every command).
 */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VIOS_AUDIO_ALLOC
#define VIOS_AUDIO_ALLOC(n) malloc(n)
#endif
#ifndef VIOS_AUDIO_FREE
#define VIOS_AUDIO_FREE(p) free(p)
#endif

#define VIOS_AUDIO_RATE 16384u
#define VIOS_AUDIO_VOICES 3u
#define VIOS_AUDIO_SLOTS 8u
#define VIOS_AUDIO_NONE 0xFFu        /* no tune slot / no error */
#define VIOS_AUDIO_FOREVER 0xFFFFFFFFu  /* indefinite note (VDP 0x85 duration 65535) */
#define VIOS_AUDIO_TONE_AMP 101u     /* v12 of 15 */

enum { VIOS_WAVE_SQUARE, VIOS_WAVE_TRIANGLE, VIOS_WAVE_SAWTOOTH, VIOS_WAVE_NOISE };

struct vios_audio_voice {
    uint32_t phase, inc;   /* 32-bit phase; inc = Hz * 2^32 / RATE */
    uint32_t left, gate;   /* samples until the next event; samples still sounding */
    uint16_t pos, lfsr;    /* tune cursor; noise shift register */
    uint8_t amp, wave, slot, octave, length, tempo, q;
};
struct vios_audio {
    struct vios_audio_voice v[VIOS_AUDIO_VOICES];
    uint8_t *tune[VIOS_AUDIO_SLOTS];
    uint8_t size[VIOS_AUDIO_SLOTS];
    uint8_t master, err_slot, err_pos;
    uint32_t notes;        /* notes started (tunes and tones), for status checks */
};

/* Phase increments of C8..B8 at 16384 Hz; lower octaves shift right. */
static const uint32_t vios_audio_octave8[12] = {1097337143u, 1162588193u, 1231719236u, 1304961221u,
    1382558204u, 1464769446u, 1551869149u, 1644148031u, 1741914112u, 1845493760u, 1955232481u, 2071496753u};

static inline void vios_audio_init(struct vios_audio *a) {
    memset(a, 0, sizeof *a);
    for (unsigned i = 0; i < VIOS_AUDIO_VOICES; ++i) { a->v[i].slot = VIOS_AUDIO_NONE; a->v[i].lfsr = 0xACE1u; }
    a->master = 127; a->err_slot = VIOS_AUDIO_NONE;
}
static inline int vios_audio_voice_busy(const struct vios_audio_voice *c) { return c->left || c->slot != VIOS_AUDIO_NONE; }
static inline unsigned vios_audio_busy(const struct vios_audio *a) {
    unsigned mask = 0;
    for (unsigned i = 0; i < VIOS_AUDIO_VOICES; ++i) if (vios_audio_voice_busy(&a->v[i])) mask |= 1u << i;
    return mask;
}
static inline void vios_audio_cursor(struct vios_audio_voice *c, uint8_t slot) {
    c->slot = slot; c->pos = 0; c->left = c->gate = 0;
    c->octave = 4; c->length = 4; c->tempo = 120; c->q = 7; c->amp = VIOS_AUDIO_TONE_AMP; c->wave = VIOS_WAVE_SQUARE;
}

static inline int vios_audio_number(const uint8_t *s, unsigned n, unsigned *p, unsigned *out) {
    unsigned v = 0, digits = 0;
    while (*p < n && s[*p] >= '0' && s[*p] <= '9' && digits < 3) { v = v * 10 + (unsigned)(s[*p] - '0'); ++*p; ++digits; }
    *out = v;
    return digits != 0;
}

/* One token sequence of text s[0..n) from c->pos: 1 a note or rest (inc 0) with
 * its length and sounding samples; 0 end of text; 2 a jump (`*` -> *jump NONE,
 * `&n` -> slot n); -1 a syntax error at c->pos. Settings update the cursor. */
static inline int vios_audio_token(const uint8_t *s, unsigned n, struct vios_audio_voice *c,
                                   uint32_t *inc, uint32_t *dur, uint32_t *gate, unsigned *jump) {
    static const int8_t semis[7] = {9, 11, 0, 2, 4, 5, 7};  /* a..g */
    for (;;) {
        unsigned p = c->pos, v;
        if (p >= n) return 0;
        uint8_t raw = s[p++], ch = (uint8_t)(raw | 0x20);   /* letters to lower case; symbols keep bit 5 */
        if (raw <= ' ') { c->pos = (uint16_t)p; continue; }
        if (ch == '*') { *jump = VIOS_AUDIO_NONE; c->pos = (uint16_t)p; return 2; }
        if (ch == '&') {
            if (!vios_audio_number(s, n, &p, &v) || v >= VIOS_AUDIO_SLOTS) return -1;
            *jump = v; c->pos = (uint16_t)p; return 2;
        }
        if ((ch >= 'a' && ch <= 'g') || ch == 'r' || ch == 'p') {
            int rest = ch == 'r' || ch == 'p', semi = 0, oct = c->octave;
            if (!rest) {
                semi = semis[ch - 'a'];
                if (p < n && (s[p] == '#' || s[p] == '+')) { ++semi; ++p; }
                else if (p < n && s[p] == '-') { --semi; ++p; }
                if (semi < 0) { semi += 12; --oct; } else if (semi > 11) { semi -= 12; ++oct; }
                if (oct < 0 || oct > 8) return -1;
            }
            unsigned len = c->length;
            if (vios_audio_number(s, n, &p, &v)) { if (v < 1 || v > 64) return -1; len = v; }
            uint32_t d = VIOS_AUDIO_RATE * 240u / c->tempo / len;
            if (p < n && s[p] == '.') { d += d / 2; ++p; }
            c->pos = (uint16_t)p;
            *inc = rest ? 0 : vios_audio_octave8[semi] >> (8 - oct);
            *dur = d;
            *gate = rest ? 0 : c->q >= 8 ? d : d * c->q / 8;
            return 1;
        }
        if (ch == '<' || ch == '>') {
            if (ch == '<' ? c->octave == 0 : c->octave >= 8) return -1;
            c->octave = (uint8_t)(c->octave + (ch == '<' ? -1 : 1)); c->pos = (uint16_t)p; continue;
        }
        if (!vios_audio_number(s, n, &p, &v)) return -1;
        switch (ch) {
            case 'o': if (v > 8) return -1; c->octave = (uint8_t)v; break;
            case 'l': if (v < 1 || v > 64) return -1; c->length = (uint8_t)v; break;
            case 't': if (v < 32 || v > 255) return -1; c->tempo = (uint8_t)v; break;
            case 'v': if (v > 15) return -1; c->amp = (uint8_t)(v * 127 / 15); break;
            case 'w': if (v > 3) return -1; c->wave = (uint8_t)v; break;
            case 'q': if (v < 1 || v > 8) return -1; c->q = (uint8_t)v; break;
            default: return -1;
        }
        c->pos = (uint16_t)p;
    }
}

/* Whole-text check: returns -1 if valid, else the offset of the bad token. */
static inline int vios_audio_check(const uint8_t *s, unsigned n) {
    struct vios_audio_voice c; uint32_t inc, dur, gate; unsigned jump;
    vios_audio_cursor(&c, 0);
    for (;;) {
        int r = vios_audio_token(s, n, &c, &inc, &dur, &gate, &jump);
        if (r == 0) return -1;
        if (r < 0) return c.pos;
    }
}

/* Next event of a playing voice, following `*` and `&` (at most 4 without a note). */
static inline int vios_audio_next(const struct vios_audio *a, struct vios_audio_voice *c, uint32_t *inc, uint32_t *dur, uint32_t *gate) {
    for (unsigned hops = 0;;) {
        if (c->slot >= VIOS_AUDIO_SLOTS || !a->tune[c->slot]) return 0;
        unsigned jump;
        int r = vios_audio_token(a->tune[c->slot], a->size[c->slot], c, inc, dur, gate, &jump);
        if (r != 2) return r;
        if (++hops > 4) return 0;
        if (jump != VIOS_AUDIO_NONE) c->slot = (uint8_t)jump;
        c->pos = 0;
    }
}

static inline int vios_audio_wave(struct vios_audio_voice *c) {
    uint32_t old = c->phase;
    c->phase += c->inc;
    switch (c->wave) {
        case VIOS_WAVE_TRIANGLE: { int t = (int)(c->phase >> 24); return t < 128 ? t * 2 - 128 : 383 - t * 2; }
        case VIOS_WAVE_SAWTOOTH: return (int)(c->phase >> 24) - 128;
        case VIOS_WAVE_NOISE:
            if ((old ^ c->phase) >> 28) c->lfsr = (uint16_t)((c->lfsr >> 1) ^ (-(c->lfsr & 1u) & 0xB400u));
            return c->lfsr & 1u ? 127 : -128;
        default: return c->phase >> 31 ? 127 : -128;
    }
}

/* One output sample, -127..127. */
static inline int vios_audio_sample(struct vios_audio *a) {
    int mix = 0;
    for (unsigned i = 0; i < VIOS_AUDIO_VOICES; ++i) {
        struct vios_audio_voice *c = &a->v[i];
        if (!c->left) {
            if (c->slot == VIOS_AUDIO_NONE) continue;
            uint32_t inc, dur, gate;
            if (vios_audio_next(a, c, &inc, &dur, &gate) != 1) { c->slot = VIOS_AUDIO_NONE; c->gate = 0; continue; }
            c->inc = inc; c->left = dur; c->gate = gate;
            if (inc) ++a->notes;
        }
        if (c->left != VIOS_AUDIO_FOREVER) --c->left;
        if (c->gate) {
            if (c->gate != VIOS_AUDIO_FOREVER) --c->gate;
            if (c->inc) mix += vios_audio_wave(c) * c->amp / 128;
        }
    }
    mix = mix * a->master / 127;
    return mix > 127 ? 127 : mix < -127 ? -127 : mix;
}

/* Commands. */
static inline void vios_audio_voice_tone(struct vios_audio *a, unsigned voice, unsigned freq, uint32_t samples, unsigned amp) {
    struct vios_audio_voice *c = &a->v[voice];
    if (freq > 8191) freq = 8191;
    c->slot = VIOS_AUDIO_NONE; c->wave = VIOS_WAVE_SQUARE; c->amp = (uint8_t)(amp > 127 ? 127 : amp);
    c->inc = (uint32_t)freq << 18; c->left = c->gate = samples;
    if (samples && freq) ++a->notes;
}
static inline uint32_t vios_audio_ms(unsigned ms) { return (uint32_t)ms * VIOS_AUDIO_RATE / 1000u; }
/* TONE: the first idle voice, else voice 0, so a tone does not cut music. Returns the voice. */
static inline unsigned vios_audio_tone(struct vios_audio *a, unsigned freq, unsigned ms) {
    unsigned voice = 0;
    while (voice < VIOS_AUDIO_VOICES && vios_audio_voice_busy(&a->v[voice])) ++voice;
    if (voice == VIOS_AUDIO_VOICES) voice = 0;
    vios_audio_voice_tone(a, voice, freq, vios_audio_ms(ms), VIOS_AUDIO_TONE_AMP);
    return voice;
}
static inline void vios_audio_stop(struct vios_audio *a, unsigned voice) {
    for (unsigned i = 0; i < VIOS_AUDIO_VOICES; ++i)
        if (voice == i || voice >= VIOS_AUDIO_VOICES) { a->v[i].slot = VIOS_AUDIO_NONE; a->v[i].left = a->v[i].gate = 0; }
}
static inline int vios_audio_play(struct vios_audio *a, unsigned slot, unsigned voice) {
    if (slot >= VIOS_AUDIO_SLOTS || voice >= VIOS_AUDIO_VOICES || !a->tune[slot]) return 0;
    vios_audio_cursor(&a->v[voice], (uint8_t)slot);
    return 1;
}
/* Store text (already allocated with VIOS_AUDIO_ALLOC, owned by the engine on
 * success) in a slot; voices on that slot stop. *old receives the buffer to free
 * once the lock is released. Returns 1 stored, 0 rejected (text not taken). */
static inline int vios_audio_store(struct vios_audio *a, unsigned slot, uint8_t *text, unsigned n, uint8_t **old) {
    *old = 0;
    if (slot >= VIOS_AUDIO_SLOTS || n > 255) return 0;
    int bad = text || !n ? vios_audio_check(text, n) : 0;
    if (bad >= 0) { a->err_slot = (uint8_t)slot; a->err_pos = (uint8_t)bad; return 0; }
    for (unsigned i = 0; i < VIOS_AUDIO_VOICES; ++i) if (a->v[i].slot == slot) vios_audio_stop(a, i);
    *old = a->tune[slot]; a->tune[slot] = n ? text : 0; a->size[slot] = (uint8_t)n;
    a->err_slot = VIOS_AUDIO_NONE; a->err_pos = 0;
    return 1;
}

static inline size_t vios_audio_status(const struct vios_audio *a, char *out, size_t size) {
    size_t used = (size_t)snprintf(out, size, "AUDIO busy=%u master=%u notes=%lu err=%u@%u",
        vios_audio_busy(a), a->master, (unsigned long)a->notes, a->err_slot, a->err_pos);
    for (unsigned i = 0; i < VIOS_AUDIO_VOICES && used < size; ++i) {
        const struct vios_audio_voice *c = &a->v[i];
        used += (size_t)snprintf(out + used, size - used, " v%u=%luHz/w%u/a%u/s%u@%u/left%lu", i,
            (unsigned long)(((uint64_t)c->inc * VIOS_AUDIO_RATE + (1u << 31)) >> 32), c->wave, c->amp,
            c->slot, c->pos, (unsigned long)c->left);
    }
    if (used < size) used += (size_t)snprintf(out + used, size - used, " slots=");
    for (unsigned i = 0; i < VIOS_AUDIO_SLOTS && used < size; ++i)
        used += (size_t)snprintf(out + used, size - used, i ? ",%u" : "%u", a->size[i]);
    return used < size ? used : size - 1;
}
