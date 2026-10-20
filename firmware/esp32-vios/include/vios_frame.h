/* VIOS frame task (GUI G1 integration). MIT.
 *
 * The VGA16 line ISR calls vios_vblank_hook once per frame at vertical blank
 * (vendor/vdp-gl vga16controller.cpp). The hook stamps the time and notifies
 * this task, which runs on the VDP core at a higher priority than the Arduino
 * loop: it preempts the VDU parser even while that parser sleeps waiting for
 * the CPU's next bytes, so frame start no longer depends on the VDP loop gap
 * (G0 finding). Each frame draws pending console cells within a 4 ms budget,
 * in beam order (rows top to bottom), and updates the cursor blink. Without
 * a vblank hook (modes other than 0) it wakes every 20 ms instead.
 *
 * vios_display_lock() is recursive: the terminal, the framebuffer and every
 * VDP mode change are serialized through it.
 */
#pragma once

extern void (* volatile vios_vblank_hook)();

static SemaphoreHandle_t vios_display_mutex;
static TaskHandle_t vios_frame_handle;
static volatile uint32_t vios_vblank_stamp;
static volatile bool vios_vblank_pending;
static std::atomic<uint32_t> vios_frames(0), vios_frame_start_max(0), vios_frame_work_max(0);
static std::atomic<uint32_t> vios_frames_skipped(0);   // kicked frames left to the vblank frame (too close to it)
/* Where a long frame goes: waiting for the display lock, console service, GUI step. */
static std::atomic<uint32_t> vios_frame_lock_max(0), vios_frame_service_max(0), vios_frame_gui_max(0);
/* Frames that copied an off-screen repaint to the screen (one long copy each,
 * vios_offscreen.h) are counted apart from the steady-state frame budget. */
static std::atomic<uint32_t> vios_frame_present_max(0), vios_frame_present_count(0);
/* Black-bar probe: after each GUI frame, 40 framebuffer points in the top 32
 * rows (the desktop there is never colour 0); count frames where any is 0. */
static std::atomic<uint32_t> vios_top_black_frames(0);
static void vios_top_black_probe() {
    if (!vios_gui_owns_display()) return;
    static const uint16_t xs[5] = {8, 160, 320, 480, 632};
    for (unsigned y = 0; y < 32; y += 4)
        for (unsigned i = 0; i < 5; ++i) {
            uint8_t b = VdpRows::row(y)[xs[i] / 2];
            if (((xs[i] & 1) ? b & 15 : b >> 4) == 0) { ++vios_top_black_frames; return; }
        }
}
static const unsigned VIOS_FRAME_CELLS = 2800; // ~4 ms at the measured 719 cells/ms
static const uint32_t VIOS_GUI_KICK_US = 700;    // a frame kicked between vblanks (a key): small damage at once, large
                                                 // damage carries to the vblank frame instead of delaying its start
/* A kicked frame must end before the next vblank (else that frame starts
 * late: 1.2-2.9 ms seen in gui_tools, 2026-10-20). It gets the time left in
 * this video frame minus this margin, half for the console and half for the
 * GUI; with too little left it does nothing and the vblank frame catches up. */
static const uint32_t VIOS_KICK_MARGIN_US = 1500, VIOS_KICK_MIN_US = 400;
/* Console cells per ms assumed for a kicked frame: a third of the text
 * console's measured 719, since cells into the GUI console window cost more
 * and a cell budget is not a time budget (kicked frames overran by 1-2 ms). */
static const unsigned VIOS_KICK_CELLS_PER_MS = 240;
static const uint32_t VIOS_GUI_FRAME_US = 3000;  // compositor time per frame (stops after the row that passes it); with the console service a flood frame stays within 4 ms

/* The outermost lock call of the current display-lock owner (return address;
 * tools: addr2line on the ELF): the frame task names who made it wait. Only
 * the owner writes it, so a plain variable suffices. */
static unsigned vios_lock_depth;
static void *vios_lock_site;
__attribute__((noinline)) void vios_display_lock() {   // noinline: the return address names the caller
    if (!vios_display_mutex) vios_display_mutex = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(vios_display_mutex, portMAX_DELAY);
    if (!vios_lock_depth++) vios_lock_site = __builtin_return_address(0);
}
void vios_display_unlock() {
    if (vios_lock_depth) --vios_lock_depth;
    xSemaphoreGiveRecursive(vios_display_mutex);
}
static volatile uint32_t vios_late_us, vios_late_cause, vios_late_prev_kind, vios_late_prev_us;
static uint32_t vios_wait_us;static char vios_wait_holder[16] = "-";static void *vios_wait_site;
uintptr_t vios_frame_wait_site() { return (uintptr_t)vios_wait_site; }
const char *vios_frame_wait_holder(uint32_t *us, bool reset) { *us = vios_wait_us; if (reset) vios_wait_us = 0; return vios_wait_holder; }
void vios_frame_late(uint32_t *out) { out[0] = vios_late_us; out[1] = vios_late_cause; out[2] = vios_late_prev_kind; out[3] = vios_late_prev_us; out[4] = vios_frames_skipped.load(); }

static void IRAM_ATTR vios_vblank_isr() {
    if (!vios_frame_handle) return;
    if (!vios_vblank_pending) { vios_vblank_pending = true; vios_vblank_stamp = (uint32_t)esp_timer_get_time(); }
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(vios_frame_handle, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void vios_frame_raise(std::atomic<uint32_t> &slot, uint32_t value) {
    uint32_t old = slot.load();
    while (value > old && !slot.compare_exchange_weak(old, value)) {}
}

/* The latest vblank frame start: was the previous frame still running when
 * vblank came (cause 1, with that frame's kind: 0 kicked, 1 vblank, 2
 * present, and its length), or did the task just start late (cause 2)? */
static void vios_frame_task(void *) {
    uint32_t last_blink = 0, prev_start = 0, prev_end = 0, prev_kind = 0;
    uint32_t vblank_last = 0, vblank_period = 16683;   // measured from consecutive vblanks
    for (;;) {
        bool vblank = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20)) && vios_vblank_pending;
        uint32_t start = (uint32_t)esp_timer_get_time();
        if (vblank) {
            uint32_t late = start - vios_vblank_stamp;
            vios_frame_raise(vios_frame_start_max, late);
            if (late > vios_late_us) {
                vios_late_us = late;vios_late_cause = (int32_t)(prev_end - vios_vblank_stamp) > 0 ? 1 : 2;
                vios_late_prev_kind = prev_kind;vios_late_prev_us = prev_end - prev_start;
            }
            uint32_t period = vios_vblank_stamp - vblank_last;
            if (vblank_last && period > 10000 && period < 25000) vblank_period = period;
            vblank_last = vios_vblank_stamp;
            vios_vblank_pending = false;
        }
        TaskHandle_t holder = vios_display_mutex ? xSemaphoreGetMutexHolder(vios_display_mutex) : nullptr;
        void *holder_site = holder ? vios_lock_site : nullptr;   // the holder's outermost lock call
        vios_display_lock();
        uint32_t locked = (uint32_t)esp_timer_get_time();
        if (locked - start > vios_wait_us) {   // who made the longest wait (nobody: the wait was not the lock)
            vios_wait_us = locked - start;vios_wait_site = holder_site;
            const char *name = holder ? pcTaskGetName(holder) : "-";
            unsigned i = 0;for (; i + 1 < sizeof vios_wait_holder && name[i]; ++i) vios_wait_holder[i] = name[i];vios_wait_holder[i] = 0;
        }
        vios_frame_raise(vios_frame_lock_max, locked - start);
        unsigned cells = VIOS_FRAME_CELLS;uint32_t gui_us = vblank ? VIOS_GUI_FRAME_US : VIOS_GUI_KICK_US;
        uint32_t since = locked - vblank_last;
        if (!vblank && vblank_last && since < 2 * vblank_period) {   // stale stamps (no vblank hook): no limit
            int32_t room = (int32_t)vblank_period - (int32_t)since - (int32_t)VIOS_KICK_MARGIN_US;
            if (room < (int32_t)VIOS_KICK_MIN_US) { vios_display_unlock(); ++vios_frames_skipped; continue; }
            uint32_t half = (uint32_t)room / 2;
            if (cells > half * VIOS_KICK_CELLS_PER_MS / 1000) cells = half * VIOS_KICK_CELLS_PER_MS / 1000;
            if (gui_us > half) gui_us = half;
        }
        if (!vios_gui_hold_console()) vios_console_terminal.service(vios_console_renderer.budget() > 40 ? cells : 40);
        uint32_t now_ms = millis();
        if (now_ms - last_blink >= 50) { last_blink = now_ms; vios_console_terminal.tick(now_ms); }
        uint32_t served = (uint32_t)esp_timer_get_time();
        vios_frame_raise(vios_frame_service_max, served - locked);
        uint32_t presents = vios_gui_presents();
        vios_gui_frame(UINT32_MAX, gui_us, vblank);
        bool presented = vios_gui_presents() != presents;
        if (!presented) vios_frame_raise(vios_frame_gui_max, (uint32_t)esp_timer_get_time() - served);
        vios_top_black_probe();   // the display server, when it owns the screen: by time, robust to rate changes
        vios_display_unlock();
        if (presented) { vios_frame_raise(vios_frame_present_max, (uint32_t)esp_timer_get_time() - start); ++vios_frame_present_count; }
        else vios_frame_raise(vios_frame_work_max, (uint32_t)esp_timer_get_time() - start);
        prev_start = start;prev_end = (uint32_t)esp_timer_get_time();prev_kind = presented ? 2 : vblank ? 1 : 0;
        ++vios_frames;
    }
}

bool vios_frame_waiting() { return vios_vblank_pending; }
/* Wake the frame task outside vertical blank (small, tear-free updates). */
void vios_frame_kick() { if (vios_frame_handle) xTaskNotifyGive(vios_frame_handle); }

void vios_frame_start() {
    if (vios_frame_handle) return;
    vios_display_lock(); vios_display_unlock(); // create the mutex before the task can race for it
    /* Above vdp-gl's priority-5 tasks (primitive exec, keyboard) that share this
     * core: at 2 they preempted the frame task and, through it, a display-lock
     * holder that inherited only 2, so vblank frames started up to 3 ms late
     * (gui_tools, 2026-10-20). A frame is bounded work (~4 ms of 16.7). */
    xTaskCreatePinnedToCore(vios_frame_task, "vios-frame", 4096, nullptr, 6, &vios_frame_handle, ARDUINO_RUNNING_CORE);
    vios_vblank_hook = vios_vblank_isr;
}
void vios_frame_stats(uint32_t *frames, uint32_t *start_max_us, uint32_t *work_max_us) {
    *frames = vios_frames.load(); *start_max_us = vios_frame_start_max.load(); *work_max_us = vios_frame_work_max.load();
}
void vios_frame_stats_reset() { vios_gui_slow_reset(); vios_frames = 0; vios_frame_start_max = 0; vios_frame_work_max = 0; vios_top_black_frames = 0; vios_frame_lock_max = 0; vios_frame_service_max = 0; vios_frame_gui_max = 0; vios_frame_present_max = 0; vios_frame_present_count = 0; vios_late_us = vios_late_cause = vios_late_prev_kind = vios_late_prev_us = 0; vios_frames_skipped = 0; vios_wait_us = 0; vios_wait_holder[0] = '-'; vios_wait_holder[1] = 0; }
void vios_frame_presents(uint32_t *count, uint32_t *max_us) { *count = vios_frame_present_count.load(); *max_us = vios_frame_present_max.load(); }
void vios_frame_parts(uint32_t *lock_us, uint32_t *service_us, uint32_t *gui_us) { *lock_us = vios_frame_lock_max.load(); *service_us = vios_frame_service_max.load(); *gui_us = vios_frame_gui_max.load(); }
uint32_t vios_frame_top_black() { return vios_top_black_frames.load(); }
