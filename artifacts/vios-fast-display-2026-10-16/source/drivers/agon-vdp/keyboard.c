#include "drivers/vdp_keyboard.h"

int vdp_keyboard_feed(struct vdp_keyboard *p, uint8_t byte) {
    if (p->state == 0) {
        if (byte >= 0x80) { p->type = byte; p->state = 1; }
    } else if (p->state == 1) {
        p->length = byte; p->index = 0; p->key = 0; p->down = 0;
        p->state = byte ? 2 : 0;
    } else {
        if (p->index == 0) p->key = byte;
        if (p->index == 3) p->down = byte;
        if (++p->index == p->length) {
            p->state = 0;
            if (p->type == 0x81 && p->length >= 4 && p->down)
                return p->key;
        }
    }
    return -1;
}
