/* GUI windows gate G0 probe (docs/superpowers/specs/2026-10-19-vios-gui-windows.md
 * section 14). MIT. Measures, on the VDP task that owns the framebuffer:
 *  - free internal heap / largest block / PSRAM after modes 0, 129, 1, before
 *    and with the compositor's planned pools allocated (floor 24 KB);
 *  - a 4 bpp LUT glyph blitter writing framebuffer rows directly (cells/ms,
 *    aligned and an unaligned free-x proxy);
 *  - the scanline ISR's frame period and line lead (s_scanLine);
 *  - a tear test: beam-ordered composition started at vertical blank, counting
 *    rows written after the ISR already converted them for that frame;
 *  - the longest VDP loop gap, which bounds vblank -> compositor start.
 * Triggered by control command G0<mm> (mode, hex); results read with G0R<n>;
 * G0Z restarts the loop-gap maximum (measure a workload, then run G0).
 * The screen shows probe glyphs while it runs; mode 1 and the console view are
 * restored afterwards. Not used by normal operation.
 */
#pragma once
#include <esp_heap_caps.h>

struct G0Scan : fabgl::VGAPalettedController {
    static int line() { return s_scanLine; }
    static uint8_t *row(int y) { return sgetScanline(y); }
};

static std::atomic<int> g0_request(-1);
static std::atomic<bool> g0_busy(false);
static char g0_result[6][200];
static uint32_t g0_loop_gap_max, g0_loop_last;

static void g0_heap(char *out, size_t size, const char *tag) {
    snprintf(out, size, "%s int_free=%u int_largest=%u psram_free=%u", tag,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void g0_lut(uint32_t *lut, uint8_t fg, uint8_t bg) {
    for (unsigned b = 0; b < 256; ++b) {
        uint32_t v = 0;
        for (unsigned k = 0; k < 4; ++k) {
            uint8_t left = (b >> (7 - 2 * k)) & 1 ? fg : bg, right = (b >> (6 - 2 * k)) & 1 ? fg : bg;
            v |= (uint32_t)((left << 4) | right) << (8 * k);
        }
        lut[b] = v;
    }
}

/* Aligned 4 bpp cell: 8 rows x one 32-bit store (x in cells). */
static inline void g0_cell(const uint32_t *lut, unsigned cx, unsigned py, uint8_t ch) {
    const uint8_t *glyph = &fabgl::FONT_AGON_DATA[ch * 8];
    for (unsigned r = 0; r < 8; ++r) ((uint32_t *)G0Scan::row(py + r))[cx] = lut[glyph[r]];
}
/* Unaligned proxy: two masked read-modify-write stores per glyph row. */
static inline void g0_cell_shifted(const uint32_t *lut, unsigned cx, unsigned py, uint8_t ch, unsigned nibbles) {
    const uint8_t *glyph = &fabgl::FONT_AGON_DATA[ch * 8];
    uint32_t mask = 0xFFFFFFFFu << (4 * nibbles);
    for (unsigned r = 0; r < 8; ++r) {
        uint32_t *p = (uint32_t *)G0Scan::row(py + r) + cx, v = lut[glyph[r]];
        p[0] = (p[0] & ~mask) | (v << (4 * nibbles));
        if (cx + 1 < 80) p[1] = (p[1] & mask) | (v >> (32 - 4 * nibbles));
    }
}

static void g0_run(uint8_t mode) {
    unsigned n = 0;
    for (auto &line : g0_result) line[0] = 0;
    canvas->waitCompletion();
    set_mode(mode);
    canvas->waitCompletion();
    delay(200);
    char a[96], b[96];
    g0_heap(a, sizeof a, "mode_only");
    void *internal = heap_caps_malloc(12 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // 8 LUT pairs, 32 windows, damage lists
    void *surfaces[4];
    for (auto &s : surfaces) s = heap_caps_malloc(9600, MALLOC_CAP_SPIRAM);
    g0_heap(b, sizeof b, "with_pools");
    snprintf(g0_result[n++], sizeof g0_result[0], "G0 mode %u videoMode %u %ux%u colours %u pools %s",
             mode, videoMode, canvasW, canvasH, (unsigned)getVGAColourDepth(), internal && surfaces[3] ? "ok" : "FAILED");
    snprintf(g0_result[n++], sizeof g0_result[0], "%s", a);
    snprintf(g0_result[n++], sizeof g0_result[0], "%s", b);
    if (videoMode == 0 && getVGAColourDepth() == 16 && internal) {
        uint32_t *lut = (uint32_t *)internal;
        g0_lut(lut, 15, 1);
        // Blitter: full screens of aligned cells, then the unaligned proxy.
        uint32_t t = (uint32_t)esp_timer_get_time();
        for (unsigned rep = 0; rep < 5; ++rep)
            for (unsigned cy = 0; cy < 60; ++cy) for (unsigned cx = 0; cx < 80; ++cx) g0_cell(lut, cx, cy * 8, (uint8_t)('A' + (cx + cy + rep) % 26));
        uint32_t aligned_us = (uint32_t)esp_timer_get_time() - t;
        t = (uint32_t)esp_timer_get_time();
        for (unsigned rep = 0; rep < 5; ++rep)
            for (unsigned cy = 0; cy < 60; ++cy) for (unsigned cx = 0; cx < 80; ++cx) g0_cell_shifted(lut, cx, cy * 8, (uint8_t)('a' + (cx + cy) % 26), 1 + rep % 7);
        uint32_t shifted_us = (uint32_t)esp_timer_get_time() - t;
        snprintf(g0_result[n++], sizeof g0_result[0], "blit aligned_cells_per_ms=%u full_screen_us=%u shifted_cells_per_ms=%u full_screen_us=%u",
                 (unsigned)(24000ull * 1000 / aligned_us), aligned_us / 5, (unsigned)(24000ull * 1000 / shifted_us), shifted_us / 5);
        // Beam map: sample the ISR line counter for ~50 ms.
        int line_max = 0, resets = 0, last = G0Scan::line();
        uint32_t first_reset = 0, last_reset = 0;
        t = (uint32_t)esp_timer_get_time();
        while ((uint32_t)esp_timer_get_time() - t < 50000) {
            int l = G0Scan::line();
            if (l < last) { uint32_t now = (uint32_t)esp_timer_get_time(); if (!resets) first_reset = now; last_reset = now; ++resets; }
            if (l > line_max) line_max = l;
            last = l;
        }
        unsigned frame_us = resets > 1 ? (last_reset - first_reset) / (resets - 1) : 0;
        // Tear test: start at vertical blank (counter past the visible rows, or at
        // its reset), composite in beam order, count rows already converted.
        const int lead = 2, visible = 480;
        unsigned results[2][3] = {};  // per damage size: frames, torn frames, late rows
        for (unsigned kind = 0; kind < 2; ++kind) {
            unsigned rows_cells = kind ? 20 : 60, cols = kind ? 40 : 80, top = kind ? 10 : 0, left = kind ? 20 : 0;
            for (unsigned frame = 0; frame < 60; ++frame) {
                uint32_t wait = (uint32_t)esp_timer_get_time();
                int prev = G0Scan::line();
                while ((uint32_t)esp_timer_get_time() - wait < 40000) {  // vblank: past visible, or wrapped
                    int l = G0Scan::line();
                    if (l >= visible || l < prev) break;
                    prev = l;
                }
                unsigned late = 0;
                for (unsigned cy = 0; cy < rows_cells; ++cy) {
                    int py = (int)(top + cy) * 8, l = G0Scan::line();
                    if (l < visible && l + lead >= py) ++late;
                    for (unsigned cx = 0; cx < cols; ++cx) g0_cell(lut, left + cx, py, (uint8_t)('0' + (frame + cx) % 10));
                }
                ++results[kind][0]; if (late) ++results[kind][1]; results[kind][2] += late;
            }
        }
        snprintf(g0_result[n++], sizeof g0_result[0], "beam frame_us=%u line_max=%d resets=%d lead_lines=%d", frame_us, line_max, resets, lead);
        snprintf(g0_result[n++], sizeof g0_result[0], "tear full_4800: frames=%u torn=%u late_cell_rows=%u window_800: frames=%u torn=%u late_cell_rows=%u loop_gap_max_us=%u",
                 results[0][0], results[0][1], results[0][2], results[1][0], results[1][1], results[1][2], g0_loop_gap_max);
    }
    heap_caps_free(internal);
    for (auto s : surfaces) heap_caps_free(s);
    canvas->waitCompletion();
    set_mode(1);
    canvas->waitCompletion();
    vios_glyph_state_valid = false;
    if (vios_console_terminal.visible()) vios_console_terminal.show(millis());
}

/* VDP loop hook: tracks the loop gap and runs a requested probe. */
void vios_g0_service() {
    uint32_t now = (uint32_t)esp_timer_get_time();
    if (g0_loop_last && now - g0_loop_last > g0_loop_gap_max) g0_loop_gap_max = now - g0_loop_last;
    g0_loop_last = now;
    int mode = g0_request.exchange(-1);
    if (mode < 0) return;
    g0_run((uint8_t)mode);
    g0_busy = false;
    g0_loop_last = (uint32_t)esp_timer_get_time();
}
bool vios_g0_request(uint8_t mode) {
    if (g0_busy.exchange(true)) return false;
    g0_request = mode;
    return true;
}
bool vios_g0_busy() { return g0_busy.load(); }
uint32_t vios_g0_gap() { return g0_loop_gap_max; }
void vios_g0_reset_gap() { g0_loop_gap_max = 0; g0_loop_last = 0; }
const char *vios_g0_line(unsigned n) { return n < 6 ? g0_result[n] : ""; }
