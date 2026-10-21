/* VIOS GUI on the VDP: the display server (lib/vios-vds) wired into the
 * board firmware. MIT.
 *
 * At boot the ESP sets the VIOS display mode (640x480, 16 colours) and starts
 * the compositor at once, before the eZ80 has loaded: the desktop and a
 * Console window appear within the ESP's own start-up. The CPU then drives
 * windows, menus and widgets with VDU 23,0,E4 batches (vios_gui_batch) and
 * polls events; the frame task (vios_frame.h) composes at every vertical
 * blank under the display lock; physical keys pass the window manager first
 * (vios_gui_key) and reach the CPU only when unused.
 *
 * Ctrl+Alt+F1 switches between the GUI and the full-screen text console
 * drawn by the 4 bpp console blitter (vios_console_vdp.h); the display
 * server keeps its state while hidden and repaints everything on return.
 *
 * Near full-screen repaints (the workbench opening or closing, the GUI
 * coming back) are composed off-screen and shown at once (vios_offscreen.h).
 *
 * Memory: the compositor (blitter tables, line buffer) is internal RAM; the
 * protocol, window manager, surface cells and the 150 KB off-screen shadow
 * framebuffer are in PSRAM.
 */
#pragma once
extern "C" {
#include "vds_ui.h"
}
#include "vios_console_vds.h"
#include "vios_offscreen.h"

void vios_display_lock();
bool vios_frame_waiting();
const char *vios_frame_wait_holder(uint32_t *us, bool reset);
uintptr_t vios_frame_wait_site();   // a vblank frame waits for the display lock (vios_frame.h)
void vios_display_unlock();
void vios_frame_kick();

static const uint8_t VIOS_GUI_SCREEN_WINDOW = 26;   // text_screen_vds.c: the workbench
static vds vios_gui_vds;
static vds_proto *vios_gui_proto;
static vds_ui *vios_gui_ui;
static vds_cell *vios_gui_cells[VDS_MAX_SURFACES];
static uint16_t vios_gui_cell_count[VDS_MAX_SURFACES];
static VdsConsoleRenderer vios_gui_console(&vios_gui_vds, VDS_CONSOLE_SURFACE, VDS_CONSOLE_WINDOW);
static volatile bool vios_gui_on;          // the compositor owns the screen
static bool vios_gui_ready;
static uint8_t vios_gui_style = 255;       // style whose palette is programmed
static uint32_t vios_gui_last_frame;
static vios_offscreen::Presenter vios_gui_presenter;

bool vios_gui_active() { return vios_gui_on; }
bool vios_gui_owns_display() { return vios_gui_ready && !ttxtMode && videoMode == 0 && getVGAColourDepth() == 16; }

static vds_cell *vios_gui_cell_memory(void *, uint8_t id, uint16_t count) {
    if (id >= VDS_MAX_SURFACES || !count) return nullptr;
    if (vios_gui_cell_count[id] < count) {          // grow only: surfaces are reused
        vds_cell *cells = (vds_cell *)heap_caps_malloc(sizeof(vds_cell) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cells) return nullptr;
        if (vios_gui_cells[id]) heap_caps_free(vios_gui_cells[id]);
        vios_gui_cells[id] = cells;
        vios_gui_cell_count[id] = count;
    }
    return vios_gui_cells[id];
}

/* G5 canvas memory (display list and raster), PSRAM, grown only: canvases
 * are recreated with the same ids. The library caps all rasters at 256 KB. */
static void *vios_gui_canvases[VDS_MAX_CANVASES];
static uint32_t vios_gui_canvas_bytes[VDS_MAX_CANVASES];
static void *vios_gui_canvas_memory(void *, uint8_t id, uint32_t bytes) {
    if (id >= VDS_MAX_CANVASES || !bytes) return nullptr;
    if (vios_gui_canvas_bytes[id] < bytes) {
        void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!memory) return nullptr;
        if (vios_gui_canvases[id]) heap_caps_free(vios_gui_canvases[id]);
        vios_gui_canvases[id] = memory;
        vios_gui_canvas_bytes[id] = bytes;
    }
    return vios_gui_canvases[id];
}

static void vios_gui_send(void *, uint8_t code, const uint8_t *data, uint8_t n) {
    if (processor) processor->send_packet(code, n, (uint8_t *)data);
}

/* Program palette entries 4..15 from the style (0-3 stay pinned) and give the
 * console window the style's text colours. Display lock held. */
static void vios_gui_apply_style() {
    const vds_style *st = &vios_gui_vds.style;
    if (st->kind == vios_gui_style) return;
    vios_gui_style = st->kind;
    for (uint8_t i = 4; i < 16; ++i) { palette[i] = st->palette[i]; setPaletteItem(i, colourLookup[st->palette[i]]); }
    vios_gui_console.colours((uint8_t)(st->title_fg | st->body << 4), (uint8_t)(st->body | st->title_fg << 4));
    if (vios_gui_on) vios_console_terminal.show(millis());   // redraw the console in the new colours
}

static bool vios_gui_mode_ok() { return !ttxtMode && videoMode == 0 && getVGAColourDepth() == 16 && VdpRows::table(); }

/* The server-owned console surface and window (display lock held). */
static bool vios_gui_console_window() {
    vds_cell *console = vios_gui_cell_memory(nullptr, VDS_CONSOLE_SURFACE, vios_console::COLS * (vios_console::ROWS + 1));
    if (!console) return false;
    vds_surface_init(&vios_gui_vds, VDS_CONSOLE_SURFACE, vios_console::COLS, vios_console::ROWS + 1, console, 0x30);
    return vds_window_open(&vios_gui_vds, VDS_CONSOLE_WINDOW, VDS_CONSOLE_SURFACE, 16, 228, 74, 28,
                           VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "Console");
}

/* The CPU restarted into MOS: drop the windows, surfaces, menus and sprites
 * the previous resident created (protocol reset, op 34) and bring back the
 * console window, which the reset closes too. */
void vios_gui_cpu_reset() {
    if (!vios_gui_ready) return;
    vios_display_lock();
    static const uint8_t reset_op[] = {0x34};
    vds_proto_batch(vios_gui_proto, reset_op, sizeof reset_op);
    vios_gui_console_window();
    vios_gui_style = 255;
    vios_gui_apply_style();
    if (vios_gui_on) { vios_console_terminal.show(millis()); vds_damage_all(&vios_gui_vds); }
    vios_display_unlock();
}

/* Where a slow batch spends its time under the lock: the ops, the widget
 * redraw after them (ui_after), the style update (max us since the last read). */
static uint32_t vios_gui_ops_max, vios_gui_after_max, vios_gui_style_max, vios_gui_hold_max, vios_gui_wait_max, vios_gui_key_max;
static uint32_t vios_gui_batch_hold_stat;   // like hold_max, but reset only with the ESP stats
static void (*vios_gui_ui_after)(void *);
static uint32_t vios_gui_after_us;
static void vios_gui_timed_after(void *ui) {
    uint32_t t = (uint32_t)esp_timer_get_time(); vios_gui_ui_after(ui); vios_gui_after_us = (uint32_t)esp_timer_get_time() - t;
}
/* Start the compositor in the VIOS display mode with a Console window. */
bool vios_gui_start() {
    vios_display_lock();
    if (!vios_gui_ready) {
        vios_gui_proto = (vds_proto *)heap_caps_calloc(1, sizeof(vds_proto), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        vios_gui_ui = (vds_ui *)heap_caps_calloc(1, sizeof(vds_ui), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!vios_gui_proto || !vios_gui_ui || !vios_gui_mode_ok() ||
            !vds_init(&vios_gui_vds, (uint8_t **)VdpRows::table(), 640, 480, fabgl::FONT_AGON_DATA)) {
            vios_display_unlock();
            return false;
        }
        vios_gui_vds.clock_us = [](void *) -> uint32_t { return (uint32_t)esp_timer_get_time(); };   // frame time budgets
        vds_proto_init(vios_gui_proto, &vios_gui_vds, vios_gui_cell_memory, vios_gui_send, nullptr);
        vios_gui_proto->canvas_mem = vios_gui_canvas_memory;
        /* A batch of the CPU's GUI ops held the display lock up to 4.9 ms
         * (gui_tools, 2026-10-20), delaying vblank frames: between ops, hand
         * the lock to a waiting vblank frame (it has the higher priority). */
        vios_gui_proto->yield = [](void *) { if (vios_frame_waiting()) { vios_display_unlock(); vios_display_lock(); } };
        vds_ui_init(vios_gui_ui, vios_gui_proto);
        if (vios_gui_proto->ui_after) { vios_gui_ui_after = vios_gui_proto->ui_after; vios_gui_proto->ui_after = vios_gui_timed_after; }
        if (!vios_gui_console_window()) { vios_display_unlock(); return false; }
        /* Without the shadow (no PSRAM left) large repaints sweep as before. */
        vios_gui_presenter.attach((uint8_t *)heap_caps_malloc(480 * 320, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), 480, 320);
        vios_gui_ready = true;
    }
    vios_gui_on = true;
    enableCursor(false);                  // the VDP text cursor must not draw into the GUI's framebuffer
    vios_gui_style = 255;
    vios_gui_apply_style();
    vios_console_terminal.attach(&vios_gui_console);
    vios_console_terminal.show(millis());
    vds_damage_all(&vios_gui_vds);
    vios_gui_last_frame = millis();
    vios_display_unlock();
    return true;
}

/* Back to the full-screen text console (the 4 bpp console blitter). */
void vios_gui_stop() {
    vios_display_lock();
    vios_gui_on = false;
    vios_gui_presenter.reset(&vios_gui_vds, (uint8_t **)VdpRows::table());   // an off-screen pass must not outlive the GUI
    vios_console_terminal.attach(&vios_console_renderer);
    vios_console_terminal.show(millis());
    vios_display_unlock();
}

/* Slowest E4 batch and frame step since the last status read (us), with the
 * first op of that batch: finds what holds the display lock too long. */
static uint32_t vios_gui_batch_max, vios_gui_frame_max;
/* The slowest GUI frame step since the last stats reset, and what it was doing. */
static struct { uint32_t us, damage_px, rows; uint8_t phase, ndamage, phase_after; } vios_gui_slow;
void vios_gui_slow_reset() { vios_gui_slow = {}; }
uint32_t vios_gui_presents() { return vios_gui_presenter.presents; }
static uint8_t vios_gui_batch_op;

uint32_t vios_gui_hold_max_us(bool reset) { uint32_t v = vios_gui_batch_hold_stat; if (reset) vios_gui_batch_hold_stat = 0; return v; }   // STATS batch_hold_max
uint32_t vios_gui_key_max_us(bool reset) { uint32_t v = vios_gui_key_max; if (reset) vios_gui_key_max = 0; return v; }   // STATS key_max
/* One E4 payload from the CPU (VDP task). */
void vios_gui_batch(const uint8_t *payload, size_t length) {
    if (!vios_gui_ready) return;          // no display server: drop, the CPU sees no replies
    uint32_t entered = (uint32_t)esp_timer_get_time();
    vios_display_lock();
    uint32_t started = (uint32_t)esp_timer_get_time();
    if (started - entered > vios_gui_wait_max) vios_gui_wait_max = started - entered;
    vios_gui_after_us = 0;
    vds_proto_batch(vios_gui_proto, payload, length);
    uint32_t spent = (uint32_t)esp_timer_get_time() - started;
    if (spent > vios_gui_batch_max) { vios_gui_batch_max = spent; vios_gui_batch_op = length ? payload[0] : 0; }
    if (spent - vios_gui_after_us > vios_gui_ops_max) vios_gui_ops_max = spent - vios_gui_after_us;
    if (vios_gui_after_us > vios_gui_after_max) vios_gui_after_max = vios_gui_after_us;
    uint32_t styled = (uint32_t)esp_timer_get_time();
    vios_gui_apply_style();
    styled = (uint32_t)esp_timer_get_time() - styled;if (styled > vios_gui_style_max) vios_gui_style_max = styled;
    /* UI-RESET (op 34) closes every window, the Console too: bring it back,
     * as after a CPU restart (vios_gui_cpu_reset). */
    if (vios_gui_on && !vios_gui_vds.windows[VDS_CONSOLE_WINDOW].used && vios_gui_console_window()) {
        vios_console_terminal.show(millis()); vds_damage_all(&vios_gui_vds);
    }
    uint32_t held = (uint32_t)esp_timer_get_time() - started;if (held > vios_gui_hold_max) vios_gui_hold_max = held;if (held > vios_gui_batch_hold_stat) vios_gui_batch_hold_stat = held;
    vios_display_unlock();
}

/* A physical key: the GUI hotkey, then the window manager. True if used. */
bool vios_gui_key(uint8_t ascii, uint8_t modifiers, uint8_t vk, uint8_t down) {
    bool ctrl = modifiers & VDS_MOD_CTRL, alt = modifiers & (VDS_MOD_LALT | VDS_MOD_RALT);
    if (vk == VDS_VK_F1 && ctrl && alt) {   // Ctrl+Alt+F1: GUI <-> text console
        if (down && vios_gui_ready) { if (vios_gui_on) vios_gui_stop(); else vios_gui_start(); }
        return true;
    }
    if (!vios_gui_on) return false;
    /* The CPU's full-screen workbench window owns the keyboard while open: its
     * F5/F6/F10 are stage/activate/close, not zoom/focus/menu. */
    const vds_window *screen = &vios_gui_vds.windows[VIOS_GUI_SCREEN_WINDOW];
    if (screen->used && (screen->flags & VDS_W_VISIBLE)) return false;
    vios_display_lock();
    uint32_t keyed = (uint32_t)esp_timer_get_time();
    bool used = vds_ui_key(vios_gui_ui, ascii, modifiers, vk, down);
    keyed = (uint32_t)esp_timer_get_time() - keyed;if (keyed > vios_gui_key_max) vios_gui_key_max = keyed;
    vios_display_unlock();
    if (used) vios_frame_kick();
    return used;
}

/* The CPU's console view requests (VDU 23,0,E5) while the GUI is on: the
 * console stays a live window; "show" brings it to the front. */
bool vios_gui_console_view(uint8_t op) {
    if (!vios_gui_on) return false;
    vios_display_lock();
    if (op == 1) vds_ui_focus(vios_gui_ui, VDS_CONSOLE_WINDOW);
    else if (op == 2) vios_console_terminal.reset();
    vios_display_unlock();
    return true;
}

/* Console backpressure (frame task, display lock held): while the compositor
 * still carries damage from earlier frames, new console output waits in the
 * terminal's shadow (scrolls coalesce) instead of churning a half-drawn
 * window; at most VIOS_GUI_HOLD_FRAMES in a row, so animations cannot starve
 * the console. */
static const unsigned VIOS_GUI_HOLD_FRAMES = 4;
static unsigned vios_gui_held;
bool vios_gui_hold_console() {
    if (!vios_gui_on || !vios_gui_vds.ndamage || vios_gui_held >= VIOS_GUI_HOLD_FRAMES) { vios_gui_held = 0; return false; }
    ++vios_gui_held;
    return true;
}

/* Each frame (frame task, display lock held): compose within the budget. */
void vios_gui_frame(uint32_t budget_pixels, uint32_t budget_us, bool at_vblank) {
    if (!vios_gui_on) return;
    if (!vios_gui_mode_ok()) return;      // a raw MODE change: wait for the VIOS display mode
    uint8_t **rows = (uint8_t **)VdpRows::table();
    if (rows != vios_gui_presenter.screen()) { vios_gui_presenter.reset(&vios_gui_vds, rows); vds_damage_all(&vios_gui_vds); }   // a new framebuffer

    uint32_t now = millis();
    uint32_t started = (uint32_t)esp_timer_get_time();
    uint32_t elapsed = now - vios_gui_last_frame;
    uint8_t phase = vios_gui_presenter.phase(), ndamage = vios_gui_vds.ndamage;
    uint32_t pixels = vios_offscreen::damage_pixels(&vios_gui_vds), rows_before = vios_gui_vds.stats.rows;
    vios_gui_presenter.frame(&vios_gui_vds, [&]() { return vds_proto_frame_within(vios_gui_proto, elapsed, budget_pixels, budget_us); }, at_vblank, budget_us);
    uint32_t spent = (uint32_t)esp_timer_get_time() - started;
    if (spent > vios_gui_frame_max) vios_gui_frame_max = spent;
    if (spent > vios_gui_slow.us) vios_gui_slow = {spent, pixels, vios_gui_vds.stats.rows - rows_before, phase, ndamage, vios_gui_presenter.phase()};
    vios_gui_last_frame = now;
}

/* One framebuffer pixel's colour index, -1 outside mode 0 (control task: a racy read is fine). */
int vios_gui_pixel(unsigned x, unsigned y) {
    if (!vios_gui_mode_ok() || x >= 640 || y >= 480) return -1;
    uint8_t b = ((volatile uint8_t **)VdpRows::table())[y][x >> 1];
    return (x & 1) ? b & 15 : b >> 4;
}

/* Control-channel summary for the harness: "GUI on windows frames rows carried errors". */
unsigned vios_gui_status(char *out, unsigned capacity) {
    if (!vios_gui_ready) return (unsigned)snprintf(out, capacity, "GUI off");
    uint32_t frame_wait;const char *frame_holder = vios_frame_wait_holder(&frame_wait, false);   // reset with the ESP stats
    unsigned n = (unsigned)snprintf(out, capacity, "GUI %s windows %u frames %u rows %u carried %u errors %u style %u batch_max_us %u batch_op %02X frame_max_us %u presents %u deferred %u forced %u abandoned %u present_us_max %u sync_frames_max %u slow_us %u slow_px %u slow_rows %u slow_nd %u slow_phase %u>%u ops_us %u after_us %u style_us %u hold_us %u wait_us %u frame_wait %u by %s at %x",
                              vios_gui_on ? "on" : "text", vios_gui_vds.count, (unsigned)vios_gui_vds.stats.frames,
                              (unsigned)vios_gui_vds.stats.rows, (unsigned)vios_gui_vds.stats.carried,
                              vios_gui_proto->errors, vios_gui_vds.style.kind,
                              (unsigned)vios_gui_batch_max, vios_gui_batch_op, (unsigned)vios_gui_frame_max,
                              (unsigned)vios_gui_presenter.presents, (unsigned)vios_gui_presenter.deferred, (unsigned)vios_gui_presenter.forced,
                              (unsigned)vios_gui_presenter.abandoned, (unsigned)vios_gui_presenter.present_us_max,
                              (unsigned)vios_gui_presenter.sync_frames_max,
                              (unsigned)vios_gui_slow.us, (unsigned)vios_gui_slow.damage_px, (unsigned)vios_gui_slow.rows,
                              vios_gui_slow.ndamage, vios_gui_slow.phase, vios_gui_slow.phase_after,
                              (unsigned)vios_gui_ops_max, (unsigned)vios_gui_after_max, (unsigned)vios_gui_style_max,
                              (unsigned)vios_gui_hold_max, (unsigned)vios_gui_wait_max, (unsigned)frame_wait, frame_holder, (unsigned)vios_frame_wait_site());
    vios_gui_batch_max = vios_gui_frame_max = vios_gui_ops_max = vios_gui_after_max = vios_gui_style_max = vios_gui_hold_max = vios_gui_wait_max = 0;
    return n;
}
