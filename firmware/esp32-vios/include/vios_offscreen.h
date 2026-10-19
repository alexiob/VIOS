/* Off-screen composition for large repaints (black-bar fix). MIT.
 *
 * The compositor has about 3 ms per frame, so a full-screen repaint (the
 * workbench window opening or closing, the GUI coming back) used to sweep
 * down the screen over ~13 frames, showing a band of half-drawn rows. When
 * the pending damage is large, the presenter instead copies the screen into
 * a shadow framebuffer (PSRAM on the device), points the compositor at it,
 * lets the repaint run there under the usual per-frame budget, and copies
 * the rows it wrote back to the screen in one go when the damage is done.
 * The screen keeps the old picture until then. A cap on the frames spent
 * off-screen keeps continuous damage (a console flood) from freezing it.
 *
 * Partial presents are always consistent: rows composed in the shadow are
 * final, and damage left over is composed straight into the screen later.
 * Portable: the adapter supplies the shadow memory; no Arduino or fabgl.
 */
#pragma once
#include <stdint.h>
#include <string.h>
extern "C" {
#include "vds.h"
}

namespace vios_offscreen {

static const int MAX_ROWS = 480;

/* Pending damage in pixels (overlaps count twice: an estimate). */
static inline uint32_t damage_pixels(const vds *v) {
    uint32_t n = 0;
    for (unsigned i = 0; i < v->ndamage; ++i) n += (uint32_t)v->damage[i].w * (uint32_t)v->damage[i].h;
    return n;
}

class Presenter {
public:
    uint32_t threshold = 640 * 480 / 4;   // about three frames of compositor work
    unsigned max_frames = 30;             // present anyway after this many frames (0.5 s)
    uint32_t presents = 0, forced = 0, last_rows = 0;

    /* Shadow memory of height * bytes; nullptr disables (old behaviour). */
    void attach(uint8_t *memory, int height, int bytes_per_row) {
        mem = memory; rows_n = height > MAX_ROWS ? MAX_ROWS : height; bytes = bytes_per_row;
        for (int y = 0; mem && y < rows_n; ++y) shadow[y] = mem + (size_t)y * (size_t)bytes;
    }
    bool active() const { return on; }
    uint8_t **screen() const { return scr; }

    /* The screen's row table (new framebuffer or GUI hidden): drop any
     * off-screen pass; the caller re-damages what it needs. */
    void reset(vds *v, uint8_t **screen_rows) {
        if (on) unhook(v);
        scr = screen_rows;
        v->rows = screen_rows;
    }

    /* One frame of composition, as vds_frame_within. Returns 1 when no damage is left. */
    int frame(vds *v, uint32_t budget_px, uint32_t budget_us) {
        return frame(v, [&]() { return vds_frame_within(v, budget_px, budget_us); });
    }
    /* The same around the adapter's own compose step (vds_proto_frame_within,
     * which may also hold or skip composition: only an empty damage list
     * ends a pass). */
    template <class Compose> int frame(vds *v, Compose compose) {
        if (!on && !cooling && mem && scr && v->ndamage && damage_pixels(v) >= threshold) begin(v);
        int result = compose();
        bool clear = v->ndamage == 0;
        if (on) {
            if (v->stats.blits != blits_at) all_dirty = true;     // a scroll blit moved shadow rows
            if (clear || ++frames >= max_frames) { if (!clear) { ++forced; cooling = true; } present(v); }
        }
        if (clear) cooling = false;
        return result;
    }

private:
    uint8_t *mem = nullptr, **scr = nullptr;
    uint8_t *shadow[MAX_ROWS];
    uint8_t dirty[MAX_ROWS];
    int rows_n = 0, bytes = 0;
    bool on = false, all_dirty = false, cooling = false;   // cooling: after a forced present, until the damage is drawn
    unsigned frames = 0;
    uint32_t blits_at = 0;
    void (*saved_hook)(void *, int16_t) = nullptr;
    void *saved_ctx = nullptr;

    static void row_written(void *ctx, int16_t y) {
        Presenter *p = (Presenter *)ctx;
        if (y >= 0 && y < p->rows_n) p->dirty[y] = 1;
        if (p->saved_hook) p->saved_hook(p->saved_ctx, y);
    }
    void begin(vds *v) {
        for (int y = 0; y < rows_n; ++y) memcpy(shadow[y], scr[y], (size_t)bytes);
        memset(dirty, 0, sizeof dirty);
        all_dirty = false; frames = 0; blits_at = v->stats.blits;
        saved_hook = v->row_written; saved_ctx = v->row_ctx;
        v->row_written = row_written; v->row_ctx = this;
        v->rows = shadow;
        on = true;
    }
    void unhook(vds *v) {
        v->row_written = saved_hook; v->row_ctx = saved_ctx;
        v->rows = scr;
        on = false;
    }
    void present(vds *v) {
        uint32_t n = 0;
        for (int y = 0; y < rows_n; ++y)
            if (all_dirty || dirty[y]) { memcpy(scr[y], shadow[y], (size_t)bytes); ++n; }
        last_rows = n; ++presents;
        unhook(v);
    }
};

} // namespace vios_offscreen
