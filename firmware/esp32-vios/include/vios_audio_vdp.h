/* VIOS lean audio on the VDP (docs/superpowers/specs/2026-10-20-vios-audio.md). MIT.
 *
 * Replaces the VDP 1.04 channel engine (one 4 KB-stack task per channel) with
 * vios_audio.h run inside fabgl's sound ISR: one static WaveformGenerator whose
 * getSample() is the engine's sampler. It disables itself when every voice is
 * idle, so an idle engine costs the mixer one flag test per sample. Commands
 * from the VDU parser and the sampler share one spinlock (the ISR may run on
 * the other core). Tune texts live in PSRAM.
 */
#pragma once
#include <fabgl.h>
#define VIOS_AUDIO_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)
#define VIOS_AUDIO_FREE(p) heap_caps_free(p)
#include "vios_audio.h"

fabgl::SoundGenerator SoundGenerator;
static struct vios_audio vios_audio_engine;
static portMUX_TYPE vios_audio_mux = portMUX_INITIALIZER_UNLOCKED;

class ViosAudioGenerator : public fabgl::WaveformGenerator {
public:
    void setFrequency(int) {}
    int getSample() {
        portENTER_CRITICAL_ISR(&vios_audio_mux);
        int sample = vios_audio_sample(&vios_audio_engine);
        if (!vios_audio_busy(&vios_audio_engine)) enable(false);
        portEXIT_CRITICAL_ISR(&vios_audio_mux);
        return sample;
    }
};
static ViosAudioGenerator vios_audio_generator;

/* Run f on the engine under the lock; wake the generator if a voice is busy. */
template <class F> static void vios_audio_locked(F f) {
    portENTER_CRITICAL(&vios_audio_mux);
    f(&vios_audio_engine);
    if (vios_audio_busy(&vios_audio_engine)) vios_audio_generator.enable(true);
    portEXIT_CRITICAL(&vios_audio_mux);
}

void init_audio() {
    vios_audio_init(&vios_audio_engine);
    vios_audio_generator.setVolume(127);  // the engine applies its own master volume
    SoundGenerator.setVolume(127);
    SoundGenerator.attach(&vios_audio_generator);
    SoundGenerator.play(true);
}

/* VDU 7 and VDU 23,0,&85 PLAY: like VDP 1.04, a busy channel refuses the note. */
uint8_t play_note(uint8_t channel, uint8_t volume, uint16_t frequency, uint16_t duration) {
    uint8_t ok = 0;
    if (channel >= VIOS_AUDIO_VOICES) return 0;
    vios_audio_locked([&](struct vios_audio *a) {
        if (vios_audio_voice_busy(&a->v[channel])) return;
        vios_audio_voice_tone(a, channel, frequency, duration == 65535 ? VIOS_AUDIO_FOREVER : vios_audio_ms(duration), volume);
        ok = 1;
    });
    return ok;
}

/* Buffer commands (vdu_buffered.h) drop VDP 1.04 samples made from buffers; there are none. */
uint8_t clearSample(uint16_t) { return 0; }
void resetSamples() {}

unsigned vios_audio_status_line(char *out, unsigned capacity) {
    char line[360];
    portENTER_CRITICAL(&vios_audio_mux);
    vios_audio_status(&vios_audio_engine, line, sizeof line);
    portEXIT_CRITICAL(&vios_audio_mux);
    return (unsigned)snprintf(out, capacity, "%s internal_free=%u internal_min=%u psram_free=%u",
        line, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

