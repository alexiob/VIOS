#ifndef DRIVERS_VDP_KEYBOARD_H
#define DRIVERS_VDP_KEYBOARD_H
#include <stdint.h>
struct vdp_keyboard {
    uint8_t state, type, length, index, key, down;
};
int vdp_keyboard_feed(struct vdp_keyboard *p, uint8_t byte);
#endif
