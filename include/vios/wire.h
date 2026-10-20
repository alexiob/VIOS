/* VIOS USB transport v1/v2. MIT; upstream firmware notices remain separate.
 * v2 (ESP to host only) adds a link sequence after the length: the ESP numbers
 * every frame it sends and keeps recent frames, so the host can ask for frames
 * the USB serial link lost (channel 5, VIOS_LINK_*). */
#ifndef VIOS_WIRE_H
#define VIOS_WIRE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VIOS_WIRE_VERSION 1
#define VIOS_WIRE_VERSION_LINK 2
#define VIOS_WIRE_PAYLOAD_MAX 240
#define VIOS_WIRE_RAW_MAX 252
#define VIOS_WIRE_FRAME_MAX 254
#define VIOS_VDU_CONSOLE 0xE0
#define VIOS_VDU_CLOCK 0xE1
#define VIOS_VDU_BOOT_HEALTH 0xE2
/* Host service: CPU request (VDU 23,0,E3,len,payload) -> USB channel 4;
 * USB channel 4 reply -> CPU packet FC,len,payload. One request in flight. */
#define VIOS_VDU_HOST 0xE3
/* GUI display-server batch: VDU 23,0,E4,len,ops (firmware/esp32-vios/lib/vios-vds). */
#define VIOS_VDU_GUI 0xE4
/* Console view on the ESP terminal: op 0 hide, 1 show (redraw), 2 clear. */
#define VIOS_VDU_CONSOLE_VIEW 0xE5
/* VIOS display setup: op 0 selects 640x480 16 colours (VDP mode 0) with logical
 * colours 0-3 pinned to mode 1's black/red/yellow/white, then reports the mode. */
#define VIOS_VDU_DISPLAY 0xE6
/* Physical line editing on the ESP (Stage 2a): flags bit 0 = the CPU awaits a
 * line (REPL line mode), bit 1 = keys are routed raw to a CPU application
 * (workbench). The ESP edits only when bit 0 is set and bit 1 is clear. */
#define VIOS_VDU_LINE_MODE 0xE7
/* Untethered boot (firmware/esp32-vios/include/vios_bootserve.h): the MOS-loaded
 * stub asks for the embedded resident: op 0 manifest, op 1 index:u16 count blocks.
 * Replies are packets FB ('M' manifest, 'B' block, 'E' no such block). */
#define VIOS_VDU_BOOT 0xE8
#define VIOS_PACKET_BOOT 0xFB
/* Lean audio (firmware/esp32-vios/include/vios_audio_vdu.h), arguments in Forth
 * stack order: op 0 TONE freq:u16
 * ms:u16, 1 TUNE len slot text, 2 PLAY slot voice, 3 STOP voice|255, 4 VOLUME v,
 * 5 QUERY -> packet F9 {busy mask, master, error slot, error offset}. */
#define VIOS_VDU_AUDIO 0xE9
#define VIOS_PACKET_AUDIO 0xF9
/* The stub hands HL = VIOS_BOOT_MAGIC to the resident: booted through MOS and
 * autoexec, so a restart is a real CPU reset (the stub reloads it). */
#define VIOS_BOOT_MAGIC 0x56420BUL
/* The ESP restarted while the CPU kept running (user mode: it cannot reload
 * the CPU): packet FA, reason; the resident resends its ESP-side state. */
#define VIOS_PACKET_ESP_RESTART 0xFA
#define VIOS_PACKET_CONSOLE 0xFE
#define VIOS_PACKET_CLOCK 0xFD
#define VIOS_PACKET_HOST 0xFC
#define VIOS_CHANNEL_CONSOLE 1
#define VIOS_CHANNEL_CONTROL 2
#define VIOS_CHANNEL_EVENT 3
#define VIOS_CHANNEL_HOST 4
/* Link recovery. Host to ESP: 'N' first:u16 = resend from link `first`.
 * ESP to host (v1, unnumbered): 'L' next:u16 = all frames before `next` sent;
 * 'G' next:u16 = some requested frames are no longer kept. */
#define VIOS_CHANNEL_LINK 5
struct vios_wire_message {
    uint8_t channel;
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[VIOS_WIRE_PAYLOAD_MAX];
    uint8_t version;   /* encode: VIOS_WIRE_VERSION_LINK adds `link`; else v1 */
    uint16_t link;
};
uint32_t vios_wire_crc32(const void *data, size_t length);
/* Encode appends the zero delimiter; zero return indicates invalid/capacity. */
size_t vios_wire_encode(const struct vios_wire_message *message, uint8_t *frame, size_t capacity);
/* Decode one COBS frame WITHOUT its delimiter. Returns 1 valid, 0 invalid. */
int vios_wire_decode(const uint8_t *frame, size_t length, struct vios_wire_message *message);
#ifdef __cplusplus
}
#endif
#endif
