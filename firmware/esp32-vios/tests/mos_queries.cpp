/* MOS 3.0.2's real boot stream (captured with MX on the board, 2026-10-19)
 * through the argument table: every VDU 23,0 command consumes exactly its
 * bytes, so the banner text lands in sync; reply packets match VDP 2.16.0. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include "vios_mos_queries.h"

int main() {
    const uint8_t boot[] = {0x17,0x00,0x80,0x01, 0x17,0x00,0xF8,0x01,0x01,0x01,0x00, 0x17,0x00,0x99,0x75,
                            0x17,0x00,0x86, 0x17,0x00,0x94,0x80, 0x1E,0x0A,0x0D,'A','g','o','n'};
    std::string seen, text;
    size_t i = 0;
    while (i < sizeof boot) {
        if (boot[i] == 0x17) {
            uint8_t cmd = boot[i + 2];
            int n = vios_mos_query_args(cmd);
            if (n < 0) n = cmd == 0x80 ? 1 : 0;          /* &80 echo byte; &86 none (existing handlers) */
            seen += std::to_string(cmd) + ",";
            i += 3 + (size_t)n;
        } else {
            if (boot[i] >= 32) text += (char)boot[i];
            ++i;
        }
    }
    assert(seen == "128,248,153,134,148," && text == "Agon");
    uint8_t p[4];
    assert(vios_colour_reply(p, 0xFF, 0xAA, 0x55, 7) == 4 && !memcmp(p, "\xFF\xAA\x55\x07", 4));
    assert(vios_colour_source(0) == 0 && vios_colour_source(63) == 0 && vios_colour_source(128) == 1 &&
           vios_colour_source(131) == 4 && vios_colour_source(64) < 0 && vios_colour_source(132) < 0);
    assert(vios_key_state_reply(p, 0x75, true) == 4 && !memcmp(p, "\x00\x00\x75\x01", 4));
    assert(vios_mos_query_args(0xF9) == 2 && vios_mos_query_args(0x86) < 0);
    printf("mos query checks passed\n");
    return 0;
}
