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
    uint32_t threshold = 640 * 480 / 2;   // near full-screen repaints only (a console window redraw stays direct)
    unsigned max_frames = 30;             // present anyway after this many frames (0.5 s)
    uint32_t sync_us = 3000;              // screen -> shadow copy time per frame (needs vds clock_us; else sync_rows)
    int sync_rows = 120;
    uint32_t presents = 0, forced = 0, abandoned = 0, deferred = 0, last_rows = 0, present_us_max = 0, sync_frames_max = 0;

    /* Shadow memory of height * bytes; nullptr disables (old behaviour). */
    void attach(uint8_t *memory, int height, int bytes_per_row) {
        mem = memory; rows_n = height > MAX_ROWS ? MAX_ROWS : height; bytes = bytes_per_row;
        for (int y = 0; mem && y < rows_n; ++y) shadow[y] = mem + (size_t)y * (size_t)bytes;
    }
    bool active() const { return state != IDLE; }
    uint8_t phase() const { return (uint8_t)state; }   // 0 idle, 1 copying the screen, 2 composing off-screen
    bool composing_offscreen() const { return state == ON; }
    uint8_t **screen() const { return scr; }

    /* The screen's row table (new framebuffer or GUI hidden): drop any
     * off-screen pass; the caller re-damages what it needs. */
    void reset(vds *v, uint8_t **screen_rows) {
        if (state == ON) unhook(v);
        state = IDLE;
        v->no_blit = 0;
        scr = screen_rows;
        v->rows = screen_rows;
    }

    /* One frame of composition, as vds_frame_within. Returns 1 when no damage is left. */
    int frame(vds *v, uint32_t budget_px, uint32_t budget_us) {
        return frame(v, [&]() { return vds_frame_within(v, budget_px, budget_us); });
    }
    /* The same around the adapter's own compose step (vds_proto_frame_within,
     * which may also hold or skip composition: only an empty damage list
     * ends a pass).
     *
     * A pass first copies the screen into the shadow over a few frames
     * (composition waits meanwhile, so the screen does not change; a scroll
     * blit on the screen restarts the copy), then composes in the shadow and
     * presents the rows it wrote in one copy. */
    /* present_ok: the frame started at vertical blank, so a long present
     * copy cannot run into the next one; otherwise a finished pass waits
     * (at most one frame) for a frame that did. */
    template <class Compose> int frame(vds *v, Compose compose, bool present_ok = true) {
        /* A finished pass presents at the start of a frame, with no compositing
         * in that frame: the copy (9-14 ms) then fits before the next vblank. */
        if (state == ON && ready) {
            if (!present_ok) { ++deferred; return v->ndamage == 0; }
            ready = false; present(v);
            return v->ndamage == 0;
        }
        if (state == IDLE && !cooling && mem && scr && v->ndamage && damage_pixels(v) >= threshold) {
            state = SYNC; synced = 0; frames = 0; blits_at = v->stats.blits;
            v->no_blit = 1;   // scrolls recompose during the pass: no screen moves under the copy, no row copies in PSRAM
        }
        if (state == SYNC) {   // composition starts the frame after the copy: one or the other per frame
            if (sync(v)) begin(v);
            else if (++frames >= max_frames) { state = IDLE; v->no_blit = 0; ++abandoned; cooling = true; }
            return v->ndamage == 0;
        }
        int result = compose();
        bool clear = v->ndamage == 0;
        if (state == ON) {
            if (v->stats.blits != blits_at) all_dirty = true;     // a scroll blit moved shadow rows
            if (clear) ready = true;   // present in the next frame that starts at vblank
            else if (++frames >= max_frames) { ++forced; cooling = true; present(v); }
        }
        if (clear) cooling = false;
        return result;
    }

private:
    enum { IDLE, SYNC, ON } state = IDLE;
    uint8_t *mem = nullptr, **scr = nullptr;
    uint8_t *shadow[MAX_ROWS];
    uint8_t dirty[MAX_ROWS];
    int rows_n = 0, bytes = 0, synced = 0;
    bool all_dirty = false, cooling = false, ready = false;   // cooling: after a forced present, until the damage is drawn; ready: composed, awaiting its present
    unsigned frames = 0;
    uint32_t blits_at = 0;
    void (*saved_hook)(void *, int16_t) = nullptr;
    void *saved_ctx = nullptr;

    static void row_written(void *ctx, int16_t y) {
        Presenter *p = (Presenter *)ctx;
        if (y >= 0 && y < p->rows_n) p->dirty[y] = 1;
        if (p->saved_hook) p->saved_hook(p->saved_ctx, y);
    }
    /* Copy the next rows of the screen; true once the shadow is complete. */
    bool sync(vds *v) {
        if (v->stats.blits != blits_at) { synced = 0; blits_at = v->stats.blits; }   // the screen moved under the copy
        uint32_t started = v->clock_us ? v->clock_us(v->clock_ctx) : 0;
        int limit = v->clock_us ? rows_n : synced + sync_rows;
        while (synced < rows_n && synced < limit) {
            memcpy(shadow[synced], scr[synced], (size_t)bytes);
            ++synced;
            if (v->clock_us && (uint32_t)(v->clock_us(v->clock_ctx) - started) >= sync_us) break;
        }
        if (frames + 1 > sync_frames_max) sync_frames_max = frames + 1;
        return synced == rows_n;
    }
    void begin(vds *v) {
        memset(dirty, 0, sizeof dirty);
        all_dirty = false; frames = 0; blits_at = v->stats.blits;
        saved_hook = v->row_written; saved_ctx = v->row_ctx;
        v->row_written = row_written; v->row_ctx = this;
        v->rows = shadow;
        state = ON;
    }
    void unhook(vds *v) {
        ready = false;
        v->row_written = saved_hook; v->row_ctx = saved_ctx;
        v->rows = scr;
        v->no_blit = 0;
        state = IDLE;
    }
    void present(vds *v) {
        uint32_t started = v->clock_us ? v->clock_us(v->clock_ctx) : 0, n = 0;
        for (int y = 0; y < rows_n; ++y)
            if (all_dirty || dirty[y]) { memcpy(scr[y], shadow[y], (size_t)bytes); ++n; }
        if (v->clock_us) { uint32_t us = v->clock_us(v->clock_ctx) - started; if (us > present_us_max) present_us_max = us; }
        last_rows = n; ++presents;
        unhook(v);
    }
};

} // namespace vios_offscreen
