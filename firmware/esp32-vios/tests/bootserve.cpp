/* Host test of the untethered-boot server (include/vios_bootserve.h): a model
 * of the MOS-loaded stub pulls an image through manifest and block bursts over
 * a packet stream mixed with unrelated VDP packets, and reassembles it exactly;
 * corruption is caught by the block CRC; the MOS-phase VDU filter keeps text
 * and swallows arguments. */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "vios_bootserve.h"

static std::vector<uint8_t> wire;   // ESP -> CPU bytes: packets 0x80|code, len, data
static void send_packet(uint8_t code, uint8_t n, const uint8_t *data) {
    wire.push_back((uint8_t)(0x80 | code)); wire.push_back(n); wire.insert(wire.end(), data, data + n);
}
/* The server side of VDU 23,0,E8, as vdu_sys.h runs it. */
static std::vector<uint8_t> image;
static void serve_manifest() { uint8_t r[16]; send_packet(VIOS_BOOT_PACKET, (uint8_t)vios_boot_manifest(r, (unsigned)image.size(), vios_boot_crc32(0, image.data(), image.size()), 1, 1152000), r); }
static void serve_blocks(unsigned index, unsigned count) {
    uint8_t r[3 + VIOS_BOOT_BLOCK + 4];
    for (unsigned i = 0; i < count; ++i) {
        unsigned n = vios_boot_block(r, image.data(), (unsigned)image.size(), index + i);
        assert(n <= 255); send_packet(VIOS_BOOT_PACKET, (uint8_t)n, r);
        if (r[0] != 'B') break;
    }
    uint8_t key[4] = {'a', 0, 0, 1}; send_packet(0x01, 4, key);   // a keyboard packet interleaves
}
static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* The stub's reader: skip unrelated packets, return the next boot packet. */
static size_t at;
static std::vector<uint8_t> next_boot_packet() {
    for (;;) {
        assert(at + 2 <= wire.size());
        uint8_t code = wire[at] & 0x7F, n = wire[at + 1];
        std::vector<uint8_t> p(wire.begin() + at + 2, wire.begin() + at + 2 + n); at += 2 + n;
        if (code == VIOS_BOOT_PACKET) return p;
    }
}
/* Returns 1 loaded, 0 refused (a CRC failed). corrupt_at flips one wire byte. */
static int stub_load(std::vector<uint8_t> &ram, long corrupt_at) {
    wire.clear(); at = 0;
    serve_manifest();
    auto m = next_boot_packet();
    assert(m.size() == 16 && m[0] == 'M' && m[1] == VIOS_BOOT_VERSION && m[2] == VIOS_BOOT_BLOCK && m[11] == 1 && get32(m.data() + 12) == 1152000);
    uint32_t size = get32(m.data() + 3), crc = get32(m.data() + 7);
    ram.assign(size, 0);
    unsigned blocks = vios_boot_blocks(size);
    for (unsigned first = 0; first < blocks; first += VIOS_BOOT_BURST) {
        size_t mark = wire.size();
        serve_blocks(first, VIOS_BOOT_BURST);
        if (corrupt_at >= 0 && (size_t)corrupt_at >= mark && (size_t)corrupt_at < wire.size()) wire[corrupt_at] ^= 0x40;
        for (unsigned i = first; i < first + VIOS_BOOT_BURST && i < blocks; ++i) {
            auto b = next_boot_packet();
            if (b[0] != 'B' || (b[1] | b[2] << 8) != (int)i) return 0;
            unsigned n = (unsigned)b.size() - 7;
            if (vios_boot_crc32(0, b.data() + 3, n) != get32(b.data() + 3 + n)) return 0;
            memcpy(ram.data() + i * VIOS_BOOT_BLOCK, b.data() + 3, n);
        }
    }
    return vios_boot_crc32(0, ram.data(), ram.size()) == crc;
}

int main() {
    assert(vios_boot_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926UL);   /* zlib CRC-32 */
    srand(7);
    for (unsigned size : {1u, 191u, 192u, 193u, 6144u, 158186u}) {
        image.resize(size); for (auto &b : image) b = (uint8_t)rand();
        std::vector<uint8_t> ram;
        assert(stub_load(ram, -1) == 1 && ram == image);
        uint8_t r[3 + VIOS_BOOT_BLOCK + 4];
        assert(vios_boot_block(r, image.data(), size, vios_boot_blocks(size)) == 3 && r[0] == 'E');   /* past the end */
    }
    /* One flipped data byte anywhere in a burst is refused. */
    image.resize(5000); for (auto &b : image) b = (uint8_t)rand();
    unsigned refused = 0;
    for (long k = 40; k < 4000; k += 397) { std::vector<uint8_t> ram; int ok = stub_load(ram, k + 18); assert(!ok || ram == image); refused += !ok; }
    assert(refused >= 8);   /* never a wrong image; almost every flip is caught at its block */
    /* MOS-phase filter: text kept, VDU arguments swallowed, VDU 23 left to the parser. */
    const uint8_t mos[] = {12, 'A', 'g', 'o', 'n', 17, 'X', 31, 'Y', 'Z', 13, 10, 22, 3, 'o', 'k', 19, 1, 2, 3, 4, 5, '!'};
    std::vector<uint8_t> text;
    for (size_t i = 0; i < sizeof mos; ++i) {
        uint8_t c = mos[i];
        assert(c != 23 && vios_mos_vdu_args(23) == 0);
        i += vios_mos_vdu_args(c);
        if (vios_mos_text(c)) text.push_back(c);
    }
    assert(std::string(text.begin(), text.end()) == "Agon\r\nok!");
    printf("boot server checks passed\n");
    return 0;
}
