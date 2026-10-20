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
static const uint32_t VIOS_GUI_FRAME_US = 3000;  // compositor time per frame (stops after the row that passes it); with the console service a flood frame stays within 4 ms

void vios_display_lock() {
    if (!vios_display_mutex) vios_display_mutex = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(vios_display_mutex, portMAX_DELAY);
}
void vios_display_unlock() { xSemaphoreGiveRecursive(vios_display_mutex); }

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

static void vios_frame_task(void *) {
    uint32_t last_blink = 0;
    for (;;) {
        bool vblank = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20)) && vios_vblank_pending;
        uint32_t start = (uint32_t)esp_timer_get_time();
        if (vblank) { vios_frame_raise(vios_frame_start_max, start - vios_vblank_stamp); vios_vblank_pending = false; }
        vios_display_lock();
        uint32_t locked = (uint32_t)esp_timer_get_time();
        vios_frame_raise(vios_frame_lock_max, locked - start);
        if (!vios_gui_hold_console()) vios_console_terminal.service(vios_console_renderer.budget() > 40 ? VIOS_FRAME_CELLS : 40);
        uint32_t now_ms = millis();
        if (now_ms - last_blink >= 50) { last_blink = now_ms; vios_console_terminal.tick(now_ms); }
        uint32_t served = (uint32_t)esp_timer_get_time();
        vios_frame_raise(vios_frame_service_max, served - locked);
        uint32_t presents = vios_gui_presents();
        vios_gui_frame(UINT32_MAX, VIOS_GUI_FRAME_US, vblank);
        bool presented = vios_gui_presents() != presents;
        if (!presented) vios_frame_raise(vios_frame_gui_max, (uint32_t)esp_timer_get_time() - served);
        vios_top_black_probe();   // the display server, when it owns the screen: by time, robust to rate changes
        vios_display_unlock();
        if (presented) { vios_frame_raise(vios_frame_present_max, (uint32_t)esp_timer_get_time() - start); ++vios_frame_present_count; }
        else vios_frame_raise(vios_frame_work_max, (uint32_t)esp_timer_get_time() - start);
        ++vios_frames;
    }
}

/* Wake the frame task outside vertical blank (small, tear-free updates). */
void vios_frame_kick() { if (vios_frame_handle) xTaskNotifyGive(vios_frame_handle); }

void vios_frame_start() {
    if (vios_frame_handle) return;
    vios_display_lock(); vios_display_unlock(); // create the mutex before the task can race for it
    xTaskCreatePinnedToCore(vios_frame_task, "vios-frame", 4096, nullptr, 2, &vios_frame_handle, ARDUINO_RUNNING_CORE);
    vios_vblank_hook = vios_vblank_isr;
}
void vios_frame_stats(uint32_t *frames, uint32_t *start_max_us, uint32_t *work_max_us) {
    *frames = vios_frames.load(); *start_max_us = vios_frame_start_max.load(); *work_max_us = vios_frame_work_max.load();
}
void vios_frame_stats_reset() { vios_gui_slow_reset(); vios_frames = 0; vios_frame_start_max = 0; vios_frame_work_max = 0; vios_top_black_frames = 0; vios_frame_lock_max = 0; vios_frame_service_max = 0; vios_frame_gui_max = 0; vios_frame_present_max = 0; vios_frame_present_count = 0; }
void vios_frame_presents(uint32_t *count, uint32_t *max_us) { *count = vios_frame_present_count.load(); *max_us = vios_frame_present_max.load(); }
void vios_frame_parts(uint32_t *lock_us, uint32_t *service_us, uint32_t *gui_us) { *lock_us = vios_frame_lock_max.load(); *service_us = vios_frame_service_max.load(); *gui_us = vios_frame_gui_max.load(); }
uint32_t vios_frame_top_black() { return vios_top_black_frames.load(); }
