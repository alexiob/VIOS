# VIOS audio: a lean ESP synth driven from Forth

Date 2026-10-20. Owner goal: "play and control simple sounds from Forth; fast,
low memory and CPU; offload to the ESP; low memory is the most important thing."

## 1. What the board has

- One audio path: ESP32 GPIO25 (internal 8-bit DAC, fed by I2S0 DMA) → amplifier →
  on-board buzzer **and** the 3.5 mm jack. The buzzer is not a separate device:
  jumper `BUZ_EN1` closed connects it to the same signal (docs/research/hardware.md).
  So "buzzer" and "audio" are one output; everything is ESP-generated.
- fabgl `SoundGenerator` (vendor/vdp-gl soundgen.cpp): 16384 Hz, two 64-sample DMA
  buffers (256 B), refilled in the I2S EOF ISR by mixing the attached
  `WaveformGenerator`s. Started once at boot (`play(true)`).

## 2. What VDP 1.04 audio costs (and why we replace it)

`init_audio()` (agon_audio.h) creates **one FreeRTOS task per channel with a
4096-byte stack**, 3 channels at boot: ≈ 12 KB of *internal* RAM for stacks plus
3 TCBs (~360 B), the `unordered_map`/`shared_ptr` nodes, and each idle task wakes
every 1 ms tick (`loop(); vTaskDelay(1)`) forever. fabgl keeps a 24000-byte
internal reserve; 12 KB is half of that again. Notes on a busy channel are
*rejected*, not queued, so tunes need the CPU to poll; every `PLAY` (0x85) sends a
reply the CPU must wait for.

**Decision:** compile the VDP 1.04 channel engine out (`VIOS_VDP_AUDIO=0`,
default) and add `vios_audio`: one static `WaveformGenerator` that synthesizes
3 voices inside the existing sound ISR. No task, no timer, no heap in internal
RAM. Note timing is counted in samples inside the ISR, so it is sample-accurate
and costs nothing while idle beyond the existing mixer call.

Compatibility kept: VDU 7 (bell) and `VDU 23,0,&85` PLAY / STATUS / VOLUME /
FREQUENCY / WAVEFORM map onto the lean voices (channels 0–2), so `BEEP` and MOS
programs still get their reply. Lost: VDP 1.04 samples, ADSR/frequency
envelopes, channels > 2 (their arguments are still parsed so the VDU stream
stays in sync). **Open decision 1.** `-DVIOS_VDP_AUDIO=1` restores the old engine.

## 3. Engine (firmware/esp32-vios/include/vios_audio.h)

Plain C, host-tested (no fabgl): `struct vios_audio` = 3 voices + 8 tune slots +
master volume + counters. Per voice: phase, phase increment, samples until the next
event, samples of sound left (gate), volume, waveform, and the tune cursor
(slot, position, octave, length, tempo, gate eighths). Tune text lives in PSRAM,
`malloc`ed to its exact length (≤ 255 B) on load.

`vios_audio_sample()` (called per sample by the generator, under a spinlock shared
with the VDU handler) mixes voices: square, triangle, sawtooth or pitched noise
(16-bit LFSR clocked at the note frequency), sums, applies master volume and
clamps to ±127. When a voice's event counter reaches 0 it parses its next tune
token. Parsing happens once per note, never per sample.

### Tune notation (stored as text, parsed on the ESP)

Text costs flash for the parser (plentiful: app ~0.66 of 1.3 MB) and is as small
as a byte code for typical tunes (one byte per note with default lengths), so it is
stored verbatim; no compile step, no second buffer. Case-insensitive; spaces ignored.

| Token | Meaning (default) |
| --- | --- |
| `c d e f g a b` then `#`/`+` or `-`, length, `.` | note in the current octave; `a` of `o4` = 440 Hz |
| `r` or `p` + length, `.` | rest |
| `o`n / `<` / `>` | octave 0–8 (4) / down / up |
| `l`n | default length: 1 whole … 64 (4) |
| `t`n | tempo, quarter notes per minute 32–255 (120) |
| `v`n | voice volume 0–15 (12) |
| `w`n | waveform 0 square, 1 triangle, 2 sawtooth, 3 noise (0) |
| `q`n | sounding eighths of each note 1–8 (7; 8 = legato) |
| `*` | repeat this slot from its start (background music, zero CPU) |
| `&`n | continue in slot n (tunes longer than one 255-byte frame; loops across slots) |

Loading validates the whole text (dry-run parse); an invalid tune is not stored and
the error slot/offset is reported by the status query. Loop guard: a fetch that
crosses more than 4 `*`/`&` without a note or rest stops the voice.

## 4. Wire protocol (VDU 23,0,&E9,op,…; reply packet &F9)

Arguments travel in Forth stack order (one shared frame builder on the CPU).
All but QUERY are fire-and-forget: the CPU never waits.

| op | Arguments | Effect |
| --- | --- | --- |
| 0 TONE | freq:u16 ms:u16 | first idle voice (else voice 0) plays a square tone |
| 1 TUNE | len slot bytes… | store tune text in slot 0–7 (len 0 frees it) |
| 2 PLAY | slot voice | start slot on voice 0–2 from its start |
| 3 STOP | voice (255 = all) | silence and end any tune |
| 4 VOLUME | 0–127 | master volume (default 127) |
| 5 QUERY | — | reply &F9: busy mask, master volume, error slot (255 none), error offset |

Host verification without CPU cost: USB control command `AS` returns one text
line with each voice's state (frequency, wave, volume, slot, position, samples to
next event), notes played, slot sizes, the last load error, and the internal and
PSRAM free heap. The board check and the host test read the same fields.

## 5. Forth words (kernel/vios_audio.c, 6 natives)

| Word | Stack | Notes |
| --- | --- | --- |
| `TONE` | ( freq ms -- ) | non-blocking; picks a free voice so it does not cut music |
| `TUNE` | ( addr u slot -- ) | `S" t140 l8 cdefg>c" 0 TUNE` stores once on the ESP |
| `PLAY` | ( slot voice -- ) | `0 0 PLAY`; one 6-byte command for a whole tune |
| `STOP` | ( voice -- ) | `-1 STOP` silences all |
| `VOLUME` | ( 0..127 -- ) | master |
| `SOUND?` | ( -- mask ) | the only round trip; bit n = voice n busy |

`BEEP ( freq ms -- flag )` stays for compatibility (blocking 0x85 reply).
Arguments are masked to bytes/words on the CPU; the ESP range-checks. Natives
are at 92 of `NATIVES_MAX` 96, so it rises to 104 (+48 B).

## 6. Budget

eZ80: ~6 dictionary entries (dictionary capacity, not new static RAM) + C code +
48 B native slots. ESP internal RAM: −12 KB+ (channel tasks gone) + ~100 B engine.
PSRAM: tune texts only. ESP CPU: one generator call per sample, 3 voice updates.
Measured numbers are in §8.

## 7. Open decisions

1. Dropping VDP 1.04 samples/envelopes/extra channels by default (flag restores).
2. `BEEP` kept (blocking); retiring it saves its code and one word.
3. No envelopes: a per-voice linear decay (`s`n) would make plucked sounds nicer
   for ~20 lines; not added until asked.
4. Tunes are ESP RAM state: lost on an ESP restart (reload them, like GUI state).

## 8. Measurements

(filled in after the board run)
