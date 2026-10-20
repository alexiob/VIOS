/* VIOS lean audio: VDU 23,0,&E9 and VDP 1.04 &85 handlers (included by vdu_audio.h). MIT. */
#pragma once
#include "vios_audio_vdp.h"

/* VDU 23,0,&E9,op,... (include/vios/wire.h VIOS_VDU_AUDIO). */
void VDUStreamProcessor::vios_audio_vdu() {
    auto op = readByte_t(); if (op < 0) return;
    switch (op) {
        case 0: {	// TONE freq; ms;
            auto freq = readWord_t(); if (freq < 0) return;
            auto ms = readWord_t(); if (ms < 0) return;
            vios_audio_locked([&](struct vios_audio *a) { vios_audio_tone(a, (unsigned)freq, (unsigned)ms); });
        } break;
        case 1: {	// TUNE len slot bytes
            auto length = readByte_t(); if (length < 0) return;
            auto slot = readByte_t(); if (slot < 0) return;
            uint8_t *text = length ? (uint8_t *)VIOS_AUDIO_ALLOC(length) : nullptr;
            for (int i = 0; i < length; ++i) {
                auto value = readByte_t(); if (value < 0) { VIOS_AUDIO_FREE(text); return; }
                if (text) text[i] = (uint8_t)value;
            }
            if (length && !text) {	// no PSRAM: report as an error at offset 0
                vios_audio_locked([&](struct vios_audio *a) { a->err_slot = (uint8_t)slot; a->err_pos = 0; });
                return;
            }
            uint8_t *old = nullptr; int stored = 0;
            vios_audio_locked([&](struct vios_audio *a) { stored = vios_audio_store(a, (unsigned)slot, text, (unsigned)length, &old); });
            if (!stored && text) VIOS_AUDIO_FREE(text);
            if (old) VIOS_AUDIO_FREE(old);
        } break;
        case 2: {	// PLAY slot voice
            auto slot = readByte_t(); if (slot < 0) return;
            auto voice = readByte_t(); if (voice < 0) return;
            vios_audio_locked([&](struct vios_audio *a) { vios_audio_play(a, (unsigned)slot, (unsigned)voice); });
        } break;
        case 3: {	// STOP voice (255 all)
            auto voice = readByte_t(); if (voice < 0) return;
            vios_audio_locked([&](struct vios_audio *a) { vios_audio_stop(a, (unsigned)voice); });
        } break;
        case 4: {	// VOLUME 0..127
            auto volume = readByte_t(); if (volume < 0) return;
            vios_audio_locked([&](struct vios_audio *a) { a->master = (uint8_t)(volume > 127 ? 127 : volume); });
        } break;
        case 5: {	// QUERY -> packet &F9: busy mask, master, error slot, error offset
            uint8_t packet[4];
            vios_audio_locked([&](struct vios_audio *a) {
                packet[0] = (uint8_t)vios_audio_busy(a); packet[1] = a->master; packet[2] = a->err_slot; packet[3] = a->err_pos;
            });
            send_packet(VIOS_PACKET_AUDIO & 0x7F, sizeof packet, packet);
        } break;
    }
}

/* VDU 23,0,&85 for MOS programs and BEEP: channels 0-2 map to the voices.
 * Samples, envelopes, and channels above 2 are not supported; their arguments
 * are still read so the VDU stream stays in step. */
void VDUStreamProcessor::vdu_sys_audio() {
    auto channel = readByte_t(); if (channel == -1) return;
    auto command = readByte_t(); if (command == -1) return;
    bool voice = channel >= 0 && channel < (int)VIOS_AUDIO_VOICES;
    switch (command) {
        case AUDIO_CMD_PLAY: {
            auto volume = readByte_t(); if (volume == -1) return;
            auto frequency = readWord_t(); if (frequency == -1) return;
            auto duration = readWord_t(); if (duration == -1) return;
            sendAudioStatus(channel, play_note(channel, volume, frequency, duration));
        } break;
        case AUDIO_CMD_STATUS: {
            uint8_t status = 0;
            if (voice) vios_audio_locked([&](struct vios_audio *a) {
                const struct vios_audio_voice *c = &a->v[channel];
                if (vios_audio_voice_busy(c)) status = AUDIO_STATUS_ACTIVE | AUDIO_STATUS_PLAYING | (c->left == VIOS_AUDIO_FOREVER ? AUDIO_STATUS_INDEFINITE : 0);
            });
            sendAudioStatus(channel, voice ? status : 255);
        } break;
        case AUDIO_CMD_VOLUME: {
            auto volume = readByte_t(); if (volume == -1) return;
            if (voice) vios_audio_locked([&](struct vios_audio *a) {
                if (volume) a->v[channel].amp = (uint8_t)(volume > 127 ? 127 : volume); else vios_audio_stop(a, (unsigned)channel);
            });
        } break;
        case AUDIO_CMD_FREQUENCY: {
            auto frequency = readWord_t(); if (frequency == -1) return;
            if (voice) vios_audio_locked([&](struct vios_audio *a) { a->v[channel].inc = (uint32_t)(frequency > 8191 ? 8191 : frequency) << 18; });
        } break;
        case AUDIO_CMD_WAVEFORM: {
            auto waveform = readByte_t(); if (waveform == -1) return;
            if (waveform == AUDIO_WAVE_SAMPLE && readWord_t() == -1) return;
            static const uint8_t map[6] = {VIOS_WAVE_SQUARE, VIOS_WAVE_TRIANGLE, VIOS_WAVE_SAWTOOTH, VIOS_WAVE_TRIANGLE, VIOS_WAVE_NOISE, VIOS_WAVE_NOISE};
            if (voice && waveform < 6) vios_audio_locked([&](struct vios_audio *a) { a->v[channel].wave = map[waveform]; });
        } break;
        case AUDIO_CMD_SAMPLE: {
            auto action = readByte_t(); if (action == -1) return;
            if (action == AUDIO_SAMPLE_LOAD) {
                auto length = read24_t(); if (length == -1) return;
                for (int32_t i = 0; i < length; ++i) if (readByte_t() == -1) return;
            } else if (action == AUDIO_SAMPLE_FROM_BUFFER) {
                if (readWord_t() == -1 || readByte_t() == -1) return;
            }
            if (action != AUDIO_SAMPLE_DEBUG_INFO) sendAudioStatus(channel, 0);
        } break;
        case AUDIO_CMD_ENV_VOLUME: {
            auto type = readByte_t(); if (type == -1) return;
            if (type == AUDIO_ENVELOPE_ADSR) for (int i = 0; i < 7; ++i) if (readByte_t() == -1) return;	// attack; decay; sustain release;
        } break;
        case AUDIO_CMD_ENV_FREQUENCY: {
            auto type = readByte_t(); if (type == -1) return;
            if (type == AUDIO_FREQUENCY_ENVELOPE_STEPPED) {
                auto phases = readByte_t(); if (phases == -1) return;
                for (int i = 0; i < 3 + 4 * phases; ++i) if (readByte_t() == -1) return;	// control stepLength; {adjustment; number;}
            }
        } break;
    }
}

void VDUStreamProcessor::sendAudioStatus(uint8_t channel, uint8_t status) {
    uint8_t packet[] = {channel, status};
    send_packet(PACKET_AUDIO, sizeof packet, packet);
}
