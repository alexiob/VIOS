/* VIOS USB transport v1. MIT; upstream firmware notices remain separate. */
#ifndef VIOS_WIRE_H
#define VIOS_WIRE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define VIOS_WIRE_VERSION 1
#define VIOS_WIRE_PAYLOAD_MAX 240
#define VIOS_WIRE_RAW_MAX 250
#define VIOS_WIRE_FRAME_MAX 252
#define VIOS_VDU_CONSOLE 0xE0
#define VIOS_VDU_CLOCK 0xE1
#define VIOS_VDU_BOOT_HEALTH 0xE2
/* Host service: CPU request (VDU 23,0,E3,len,payload) -> USB channel 4;
 * USB channel 4 reply -> CPU packet FC,len,payload. One request in flight. */
#define VIOS_VDU_HOST 0xE3
/* Console view on the ESP terminal: op 0 hide, 1 show (redraw), 2 clear. */
#define VIOS_VDU_CONSOLE_VIEW 0xE5
/* VIOS display setup: op 0 selects 640x480 16 colours (VDP mode 0) with logical
 * colours 0-3 pinned to mode 1's black/red/yellow/white, then reports the mode. */
#define VIOS_VDU_DISPLAY 0xE6
#define VIOS_PACKET_CONSOLE 0xFE
#define VIOS_PACKET_CLOCK 0xFD
#define VIOS_PACKET_HOST 0xFC
#define VIOS_CHANNEL_CONSOLE 1
#define VIOS_CHANNEL_CONTROL 2
#define VIOS_CHANNEL_EVENT 3
#define VIOS_CHANNEL_HOST 4
struct vios_wire_message {
    uint8_t channel;
    uint16_t sequence;
    uint16_t length;
    uint8_t payload[VIOS_WIRE_PAYLOAD_MAX];
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
