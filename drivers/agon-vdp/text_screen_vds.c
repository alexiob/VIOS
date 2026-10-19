/* The workbench text screen on the ESP display server (built with VIOS_GUI=1).
 * Same contract as text_screen.c, but the screen lives in display-server
 * surface VIOS_GUI_SCREEN_SURFACE: puts, clears and scrolls become E4 ops
 * (lib/vios-vds/src/vds_proto.h) queued here and sent by vios_text_render()
 * within its byte budget, so idle painting stays bounded (a full queue is
 * sent at once, so a full-screen repaint streams out). The ESP compares
 * cells itself and redraws only what changed, so the CPU keeps no screen
 * planes: this replaces text_screen.c's 19,200 bytes with one small queue.
 * Reads (vios_text_cell) ask the server and cache 14 cells at a time. */
#include "vios/text_screen.h"
#include "vios/ui.h"
#include "platform/vdp.h"
#include <string.h>

#define QUEUE 512                       /* ops waiting for vios_text_render */
#define BLANK 0x03                      /* fg 3, bg 0: the text screen's clear cell */
static unsigned char queue[QUEUE];
static unsigned queued;
static uint32_t generation;
static struct vios_text_stats stats;
static unsigned char cache[14 * 2], cache_x = 255, cache_y = 255, cache_n;

static unsigned send(unsigned budget);

/* Queue one op (at most 6 + 80 bytes); a full queue is sent first. */
static void op(const unsigned char *bytes, unsigned n) {
    cache_y = 255;
    while (queued + n > QUEUE && send(255 + 4)) {}
    memcpy(queue + queued, bytes, n);
    queued += n;
    ++stats.spans;
}

static void fill(unsigned x, unsigned y, unsigned w, unsigned h, unsigned char ch, unsigned char attr) {
    unsigned char o[8] = {0x13, VIOS_GUI_SCREEN_SURFACE, (unsigned char)x, (unsigned char)y, (unsigned char)w, (unsigned char)h, ch, attr};
    op(o, sizeof o);
}

void vios_text_stats_reset(void) { stats.cells = stats.spans = stats.batches = stats.bytes = 0; }
int vios_text_owns_display(void) { return 1; }

/* The workbench is a full-screen window over the desktop showing the screen
 * surface; F6 and the window manager reach the other windows. */
#define WINDOW (VIOS_UI_WINDOWS - 1)
static int window_open;
static void window(int open) {
    unsigned char close[2] = {0x57, WINDOW};
    unsigned char show[10] = {0x50, WINDOW, VIOS_GUI_SCREEN_SURFACE, 0, 0, 0, 0, VIOS_TEXT_COLUMNS, VIOS_TEXT_ROWS, 1};
    /* Close only an open window: closing a closed one is a protocol error the
     * server counts and queues as an event (seen on the board as op 57). */
    if (window_open) op(close, sizeof close);
    if (open) op(show, sizeof show);
    window_open = open;
}
void vios_text_show(void) {
    window(1);
    while (queued) if (!send(255 + 4)) break;
}
void vios_text_hide(void) {
    window(0);
    while (queued) if (!send(255 + 4)) break;
}
void vios_text_forget(void) { window_open = 0; queued = 0; }
void vios_text_stats(struct vios_text_stats *out) { *out = stats; }
uint32_t vios_text_generation(void) { return generation; }
int vios_text_pending(void) { return queued != 0; }

void vios_text_reset(void) {
    ++generation;
    queued = 0;                             /* the surface is recreated blank */
    cache_y = 255;
    unsigned char o[5] = {0x01, VIOS_GUI_SCREEN_SURFACE, VIOS_TEXT_COLUMNS, VIOS_TEXT_ROWS, BLANK};
    op(o, sizeof o);
    vios_text_stats_reset();
}

void vios_text_begin_frame(void) {
    ++generation;
    fill(0, 0, VIOS_TEXT_COLUMNS, VIOS_TEXT_ROWS, ' ', BLANK);
}

void vios_text_clear(unsigned x, unsigned y, unsigned width, unsigned height) {
    if (x >= VIOS_TEXT_COLUMNS || y >= VIOS_TEXT_ROWS || !width || !height) return;
    if (width > VIOS_TEXT_COLUMNS - x) width = VIOS_TEXT_COLUMNS - x;
    if (height > VIOS_TEXT_ROWS - y) height = VIOS_TEXT_ROWS - y;
    fill(x, y, width, height, ' ', BLANK);
}

void vios_text_put(unsigned x, unsigned y, const char *text, size_t length, unsigned fg, unsigned bg) {
    if (x >= VIOS_TEXT_COLUMNS || y >= VIOS_TEXT_ROWS || !text) return;
    if (length > VIOS_TEXT_COLUMNS - x) length = VIOS_TEXT_COLUMNS - x;
    unsigned char o[6 + VIOS_TEXT_COLUMNS] = {0x10, VIOS_GUI_SCREEN_SURFACE, (unsigned char)x, (unsigned char)y,
                                             (unsigned char)((fg & 15) | (bg & 15) << 4), (unsigned char)length};
    memcpy(o + 6, text, length);
    op(o, 6 + (unsigned)length);
    stats.cells += (uint32_t)length;
}

int vios_text_scroll(unsigned x, unsigned y, unsigned width, unsigned height, int rows) {
    int distance = rows < 0 ? -rows : rows;
    if (x >= VIOS_TEXT_COLUMNS || y >= VIOS_TEXT_ROWS || !width || !height || width > VIOS_TEXT_COLUMNS - x ||
        height > VIOS_TEXT_ROWS - y || !rows || distance > 31 || distance >= (int)height) return 0;
    unsigned char o[8] = {0x16, VIOS_GUI_SCREEN_SURFACE, (unsigned char)x, (unsigned char)y, (unsigned char)width,
                          (unsigned char)height, (unsigned char)(rows & 0xFF), BLANK};
    op(o, sizeof o);
    return 1;
}

/* Send whole ops (never a partial one) up to `budget` bytes as one E4 payload. */
unsigned vios_text_render(unsigned budget) { return send(budget); }

static unsigned send(unsigned budget) {
    if (!queued || budget <= 4) return 0;
    unsigned room = budget - 4 > 255 ? 255 : budget - 4, take = 0;
    while (take < queued) {                 /* op boundaries: sizes as in vds_proto.c */
        unsigned char c = queue[take];
        unsigned n = c == 0x10 ? 6u + queue[take + 5] : c == 0x13 || c == 0x16 ? 8u : c == 0x01 ? 5u :
                     c == 0x50 ? 10u : c == 0x57 ? 2u : 1u;
        if (take + n > room) break;
        take += n;
    }
    if (!take) return 0;                    /* a single op larger than the budget waits */
    unsigned char header[4] = {23, 0, 0xE4, (unsigned char)take};
    platform_vdp_send(header, sizeof header);
    platform_vdp_send(queue, take);
    memmove(queue, queue + take, queued - take);
    queued -= take;
    ++stats.batches;
    stats.bytes += take + 4;
    return take + 4;
}

int vios_text_cell(unsigned x, unsigned y, struct vios_text_cell *out) {
    if (x >= VIOS_TEXT_COLUMNS || y >= VIOS_TEXT_ROWS || !out) return 0;
    if (y != cache_y || x < cache_x || x >= cache_x + cache_n) {
        while (queued) if (!send(255 + 4)) return 0;
        unsigned n = VIOS_TEXT_COLUMNS - x < 14 ? VIOS_TEXT_COLUMNS - x : 14, got = 0;
        unsigned char command[9] = {23, 0, 0xE4, 5, 0x32, VIOS_GUI_SCREEN_SURFACE, (unsigned char)x, (unsigned char)y, (unsigned char)n};
        unsigned char reply[32];
        if (!platform_vdp_request(command, sizeof command, 0xF1, reply, sizeof reply, &got) || got < 2 + 2 * n || reply[0] != 0x32)
            return 0;
        memcpy(cache, reply + 2, 2 * n);
        cache_x = (unsigned char)x; cache_y = (unsigned char)y; cache_n = (unsigned char)n;
    }
    out->ch = cache[2 * (x - cache_x)];
    out->attr = cache[2 * (x - cache_x) + 1];
    return 1;
}
