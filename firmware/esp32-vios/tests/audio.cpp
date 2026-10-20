// Host checks for the lean audio engine (include/vios_audio.h).
#include <assert.h>
#include <stdio.h>
#include "vios_audio.h"

static uint8_t *text(const char *s) { size_t n = strlen(s); uint8_t *t = (uint8_t *)malloc(n ? n : 1); memcpy(t, s, n); return t; }
static int store(vios_audio *a, unsigned slot, const char *s) {
    uint8_t *t = text(s), *old;
    int ok = vios_audio_store(a, slot, t, (unsigned)strlen(s), &old);
    if (!ok) free(t);
    free(old);
    return ok;
}
static uint32_t hz(uint32_t inc) { return (uint32_t)(((uint64_t)inc * VIOS_AUDIO_RATE + (1u << 31)) >> 32); }
/* Run n samples; return how many were non-zero (sounding). */
static unsigned run(vios_audio *a, unsigned n) { unsigned loud = 0; while (n--) if (vios_audio_sample(a)) ++loud; return loud; }
static void release(vios_audio *a) { for (unsigned i = 0; i < VIOS_AUDIO_SLOTS; ++i) free(a->tune[i]); }

int main() {
    vios_audio a; vios_audio_init(&a);
    assert(vios_audio_busy(&a) == 0 && vios_audio_sample(&a) == 0);

    // TONE: 100 ms = 1638 samples on the first idle voice, then silence.
    assert(vios_audio_tone(&a, 440, 100) == 0);
    assert(hz(a.v[0].inc) == 440 && a.v[0].left == 1638 && vios_audio_busy(&a) == 1);
    assert(vios_audio_tone(&a, 880, 50) == 1);   // voice 0 busy: next voice
    assert(run(&a, 1638) > 1500 && vios_audio_busy(&a) == 0 && run(&a, 100) == 0);
    for (unsigned i = 0; i < 3; ++i) vios_audio_tone(&a, 100 + i, 1000);
    assert(vios_audio_tone(&a, 999, 10) == 0 && hz(a.v[0].inc) == 999);  // all busy: voice 0
    vios_audio_stop(&a, 255);
    assert(vios_audio_busy(&a) == 0);

    // Validation and error position.
    assert(vios_audio_check((const uint8_t *)"t140 l8 o5 c d e- f# g4. r16 > c < b- *", 39) == -1);
    assert(vios_audio_check((const uint8_t *)"cdx", 3) == 2);
    assert(vios_audio_check((const uint8_t *)"o9", 2) == 0);
    assert(vios_audio_check((const uint8_t *)"c65", 3) == 0);
    assert(vios_audio_check((const uint8_t *)"t20", 3) == 0);
    assert(vios_audio_check((const uint8_t *)"o0 c-", 5) == 3);  // below C0
    assert(vios_audio_check((const uint8_t *)"&8", 2) == 0);
    assert(!store(&a, 2, "cdz") && a.err_slot == 2 && a.err_pos == 2 && !a.tune[2]);
    assert(!store(&a, 8, "c"));

    // Tune timing: t120 quarter = 8192 samples, q7 sounds 7168 of them.
    assert(store(&a, 0, "a b") && a.err_slot == VIOS_AUDIO_NONE);
    assert(vios_audio_play(&a, 0, 1) && !vios_audio_play(&a, 3, 1) && !vios_audio_play(&a, 0, 3));
    vios_audio_sample(&a);
    assert(hz(a.v[1].inc) == 440 && a.v[1].left == 8191 && a.v[1].gate == 7167);
    unsigned loud = run(&a, 8191);
    assert(loud > 7000 && loud <= 7167);
    vios_audio_sample(&a);
    assert(hz(a.v[1].inc) == 494);           // B4
    run(&a, 8191);
    assert(vios_audio_busy(&a) == 2);        // tune ended but not yet fetched
    vios_audio_sample(&a);
    assert(vios_audio_busy(&a) == 0 && a.notes >= 2);

    // Octaves, accidentals, dots, lengths, legato.
    assert(store(&a, 1, "o3 c+8. >b-16 q8 l2 c"));
    vios_audio_play(&a, 1, 0);
    vios_audio_sample(&a); assert(hz(a.v[0].inc) == 139 && a.v[0].left == 4096 + 2048 - 1);
    run(&a, a.v[0].left); vios_audio_sample(&a); assert(hz(a.v[0].inc) == 466 && a.v[0].left == 2047);
    run(&a, a.v[0].left); vios_audio_sample(&a); assert(hz(a.v[0].inc) == 262 && a.v[0].gate == a.v[0].left);
    vios_audio_stop(&a, 0);

    // Loop: `*` repeats forever; `&n` continues in another slot; empty loops stop.
    assert(store(&a, 3, "l64 c *"));
    vios_audio_play(&a, 3, 2);
    uint32_t before = a.notes;
    run(&a, 512 * 10);
    assert(a.notes - before >= 5 && vios_audio_busy(&a) == 4);
    assert(store(&a, 4, "l64 d &5") && store(&a, 5, "l64 e &4"));
    vios_audio_stop(&a, 2); vios_audio_play(&a, 4, 2);
    vios_audio_sample(&a); assert(hz(a.v[2].inc) == 294);
    run(&a, a.v[2].left); vios_audio_sample(&a); assert(hz(a.v[2].inc) == 330 && a.v[2].slot == 5);
    run(&a, a.v[2].left); vios_audio_sample(&a); assert(hz(a.v[2].inc) == 294 && a.v[2].slot == 4);
    assert(store(&a, 6, "*") && store(&a, 7, "&6"));
    vios_audio_play(&a, 7, 2); vios_audio_sample(&a);
    assert(vios_audio_busy(&a) == 0);        // no note in the loop: the voice stops
    // Replacing a slot stops the voices on it; length 0 frees it.
    vios_audio_play(&a, 3, 2); vios_audio_sample(&a);
    assert(store(&a, 3, "") && !a.tune[3] && vios_audio_busy(&a) == 0);

    // Waveforms, master volume and clamping.
    assert(store(&a, 0, "v15 w1 c"));
    vios_audio_play(&a, 0, 0);
    int lo = 0, hi = 0;
    for (unsigned i = 0; i < 4000; ++i) { int s = vios_audio_sample(&a); if (s < lo) lo = s; if (s > hi) hi = s; }
    assert(lo < -100 && hi > 100);
    a.master = 0; assert(vios_audio_sample(&a) == 0); a.master = 127;
    for (unsigned w = 0; w < 4; ++w) {
        char t[16]; snprintf(t, sizeof t, "v15 w%u c", w);
        assert(store(&a, 0, t)); vios_audio_play(&a, 0, 0); vios_audio_play(&a, 0, 1); vios_audio_play(&a, 0, 2);
        for (unsigned i = 0; i < 2000; ++i) { int s = vios_audio_sample(&a); assert(s >= -127 && s <= 127); }
    }
    vios_audio_stop(&a, 255);
    assert(store(&a, 0, "w3 c4"));            // noise is not constant
    vios_audio_play(&a, 0, 0);
    int first = vios_audio_sample(&a), changes = 0;
    for (unsigned i = 0; i < 4000; ++i) if (vios_audio_sample(&a) != first) ++changes;
    assert(changes > 100);
    vios_audio_stop(&a, 255);

    // Indefinite note (VDP 0x85 duration 65535) holds until stopped.
    vios_audio_voice_tone(&a, 1, 750, VIOS_AUDIO_FOREVER, 100);
    run(&a, 100000);
    assert(vios_audio_busy(&a) == 2 && a.v[1].left == VIOS_AUDIO_FOREVER);
    vios_audio_stop(&a, 1);

    char line[400];
    size_t n = vios_audio_status(&a, line, sizeof line);
    assert(n == strlen(line) && strstr(line, "AUDIO busy=0 master=127") && strstr(line, " v1=750Hz/") && strstr(line, " slots=5,21,0,0,8,8,1,2"));
    char tiny[8]; vios_audio_status(&a, tiny, sizeof tiny); assert(strlen(tiny) == 7);
    release(&a);
    printf("audio engine checks passed\n");
    return 0;
}
