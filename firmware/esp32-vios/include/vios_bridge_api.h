/* VIOS USB multiplex integration declarations; original VDP API remains intact. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <vios_wire.h>
void vios_bridge_poll();
void vios_bridge_boot();
void vios_usb_console_write(const uint8_t *data,size_t length);
void vios_usb_host_write(const uint8_t *data,size_t length);

void vios_legacy_hexload_notice();
void vios_boot_health(uint8_t flag);

bool vios_cpu_is_paused();
void vios_unsupported(const char *feature);

void vios_bridge_probe();

bool vios_bridge_init();
bool vios_bridge_start();
void vios_bridge_service_vdp();
unsigned long vios_vdp_wait_running();
void vios_vdp_parser_busy(bool busy);
void vios_vdp_tx_begin();
void vios_vdp_tx_end();

void vios_bridge_handoff();

/* GUI Stage 0 instrumentation (no behaviour change): key injection at the
 * PS/2 boundary, key-to-glyph latency probe, parser and USB-blocking time. */
bool vios_injected_key(uint8_t *ascii);
void vios_probe_key(uint8_t ascii);
void vios_probe_glyph(char c);
void vios_probe_glyphs(const char *text,size_t count); // one counter update per run
void vios_parser_time(uint32_t us);

/* GUI Stage 1c: console terminal rendered on the ESP (vios_console_vdp.h). */
void vios_console_feed(const uint8_t *data,size_t length);
void vios_console_view(uint8_t op);
void vios_console_tick();
/* Text of ESP console row y (0..57) without trailing spaces; status row 58. */
unsigned vios_console_row(unsigned y,char *out,unsigned capacity);

/* GUI windows gate G0 probe (vios_g0_probe.h). */
void vios_g0_service();
bool vios_g0_request(uint8_t mode);
bool vios_g0_busy();
void vios_g0_reset_gap();
const char *vios_g0_line(unsigned n);
uint32_t vios_g0_gap();
/* VIOS display: mode 0 with palette 0-3 pinned (vios_console_vdp.h). */
void vios_display_mode0();
/* Max time per VDP loop section (0 host->CPU service, 1 console, 2 keyboard). */
void vios_loop_section(unsigned index,uint32_t started_us);

/* GUI G1 frame task (vios_frame.h): woken at vertical blank, it runs the
 * budgeted console drawing at higher priority than the VDP parser. The
 * recursive display lock covers the terminal and the framebuffer; the E0
 * feed, view ops and every VDP mode change take it. */
void vios_display_lock();
void vios_display_unlock();
void vios_frame_start();
void vios_frame_stats(uint32_t *frames,uint32_t *start_max_us,uint32_t *work_max_us);
void vios_frame_stats_reset();
/* GUI display server (vios_gui.h): status line, a key with modifiers for the
 * window manager (true if used), and one framebuffer pixel's colour index. */
unsigned vios_gui_status(char *out,unsigned capacity);
bool vios_gui_key(uint8_t ascii,uint8_t modifiers,uint8_t vk,uint8_t down);
int vios_gui_pixel(unsigned x,unsigned y);
void vios_gui_batch(const uint8_t *payload,size_t length);

/* Stage 2a cooked line editing on the ESP (vios_line.h, glue in vios_bridge.h). */
void vios_line_mode(uint8_t flags);                       // VDU 23,0,E7,flags
bool vios_line_key(uint8_t ascii,uint8_t vk,uint8_t down); // true: consumed, do not send 0x81
