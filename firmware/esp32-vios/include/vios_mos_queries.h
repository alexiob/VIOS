/* The VDP queries Agon MOS 3.0.2 makes while booting, answered in AgonPlatform
 * VDP 2.16.0's formats (vdu_sys.h there; untethered-boot spec §16.7). MIT.
 * Portable: the vendored parser (vdu_sys.h) reads the arguments and sends the
 * packets these build; tests/mos_queries.cpp replays MOS 3's real boot stream. */
#pragma once
#include <stdint.h>

/* Argument bytes after VDU 23,0,cmd for the commands handled here (-1: other). */
static inline int vios_mos_query_args(uint8_t cmd) {
    switch (cmd) {
    case 0x94: case 0x99: return 1;   /* colour index; virtual key */
    case 0xF8: return 4;              /* flag; value; (words) */
    case 0xF9: return 2;              /* flag; */
    default: return -1;
    }
}
/* &94 reply, packet 0x04 (PACKET_SCRPIXEL): R, G, B, palette index. */
static inline unsigned vios_colour_reply(uint8_t *out, uint8_t r, uint8_t g, uint8_t b, uint8_t index) {
    out[0] = r; out[1] = g; out[2] = b; out[3] = index; return 4;
}
/* &94 active colours: 128 text fg, 129 text bg, 130 graphics fg, 131 graphics bg;
 * 0..63 palette entries; anything else gets no reply (2.16.0 behaviour). */
static inline int vios_colour_source(uint8_t index) {
    if (index < 64) return 0;
    return index >= 128 && index <= 131 ? index - 127 : -1;
}
/* Modifier byte of keyboard packets (agon_ps2.h): ctrl, shift, left alt,
 * right alt, caps lock, num lock, scroll lock, GUI from bit 0 up. */
static inline uint8_t vios_key_modifiers(bool ctrl, bool shift, bool lalt, bool ralt, bool caps, bool num, bool scroll, bool gui) {
    return (uint8_t)(ctrl << 0 | shift << 1 | lalt << 2 | ralt << 3 | caps << 4 | num << 5 | scroll << 6 | gui << 7);
}
/* &99 reply, packet 0x01 (PACKET_KEYCODE): ascii, modifiers, vk, down, as the
 * keyboard driver reports an injected key in 2.16.0. */
static inline unsigned vios_key_state_reply(uint8_t *out, uint8_t ascii, uint8_t modifiers, uint8_t vk, bool down) {
    out[0] = ascii; out[1] = modifiers; out[2] = vk; out[3] = down ? 1 : 0; return 4;
}
