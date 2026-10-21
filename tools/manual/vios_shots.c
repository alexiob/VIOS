/* Manual screenshots from the real software, on the host: the resident Forth
 * system, UI words and GUI tools (kernel/vios_ui.c, kernel/vios_tools.c) on the
 * CPU side; the real ESP display server (lib/vios-vds) on the other, joined by
 * a stubbed VDP link exactly as in tests/vios_tools_host.c (whose stubs are
 * copied below). The board's Console window (include/vios_gui.h) is recreated
 * with the same surface, window, place and colours.
 *
 *   vios_shots FONT.bin OUTDIR < SCENE
 *
 * SCENE lines (\n and \\ escapes in text):
 *   eval FORTH        evaluate on the CPU side (fails the run on error)
 *   con FORTH         as typed at the console: "vios> FORTH", output, "ok"
 *   conrun FORTH      Enter after FORTH was typed (conraw): output, "ok", prompt
 *   conraw TEXT       print TEXT on the console
 *   module NAME SRC   stage and activate a module
 *   type TEXT         type on the keyboard (ESP side)
 *   key K [MODS]      press K: a VDS_VK name (DOWN, TAB, F10...) or one char;
 *                     MODS letters c (Ctrl) s (Shift) a (Alt)
 *   ctrl X / ctrlshift X
 *   focus WINDOW      give a window the keyboard
 *   place WINDOW X Y  move a window (pixels)
 *   idle [N]          let the CPU and the frame loop run
 *   shot NAME         write OUTDIR/NAME.ppm
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "forth/system.h"
#include "forth/modules.h"
#include "forth/inspect.h"
#include "vios/ui.h"
#include "vios/tools.h"
#include "vios/workbench.h"
#include "vds_ui.h"

#define W 640
#define H 480
static uint8_t fb[H][W / 2], *rows[H];
static uint8_t font[256 * 8];
static vds_cell cells[VDS_MAX_SURFACES][80 * 60];
static vds v;
static vds_proto p;
static vds_ui u;

/* ESP side. */
static vds_cell *cell_mem(void *ctx, uint8_t id, uint16_t n) { (void)ctx; return id < VDS_MAX_SURFACES && n <= 80 * 60 ? cells[id] : 0; }
static uint8_t reply[32], reply_n, reply_valid;
static int nudged;
static unsigned nudges, bytes_to_esp, requests;
static void esp_send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    assert(n <= 32);
    if (code == VDS_PACKET_EVENT) { assert(n == 1); nudged = 1; ++nudges; return; }
    memcpy(reply, d, n); reply_n = n; reply_valid = 1;
}

/* CPU platform: the VDU stream parsed for 23,0,E4,len,payload. */
static uint8_t vdu[300];
static unsigned vdu_n;
void platform_vdp_send(const void *data, size_t length) {
    const uint8_t *b = data;
    bytes_to_esp += (unsigned)length;
    for (size_t i = 0; i < length; ++i) {
        vdu[vdu_n++] = b[i];
        if (vdu_n >= 4 && vdu[0] == 23 && vdu[1] == 0 && vdu[2] == 0xE4 && vdu_n == 4u + vdu[3]) {
            assert(vds_proto_batch(&p, vdu + 4, vdu[3]));
            vdu_n = 0;
        }
        assert(vdu_n < sizeof vdu);
    }
}
int platform_vdp_request(const void *command, size_t length, unsigned char type, void *data, size_t capacity, unsigned *actual) {
    reply_valid = 0;
    ++requests;
    platform_vdp_send(command, length);
    if (type != 0xF1 || !reply_valid || reply_n > capacity) { *actual = 0; return 0; }
    memcpy(data, reply, reply_n);
    *actual = reply_n;
    return 1;
}
int platform_vdp_nudged(void) { int n = nudged; nudged = 0; return n; }
unsigned platform_vdp_epoch(void) { return 0; }
unsigned short platform_timer_read(void) { static unsigned short t; return t -= 97; }
static char console[8192];
static size_t console_n;
static void (*divert)(unsigned char);
void (*platform_console_divert(void (*sink)(unsigned char)))(unsigned char) { void (*old)(unsigned char) = divert; divert = sink; return old; }
void platform_console_putc(unsigned char c) { if (divert) { divert(c); return; } if (console_n + 1 < sizeof console) console[console_n++] = (char)c; console[console_n] = 0; }
int platform_console_getc(void) { return -1; }
void platform_console_flush(void) {}

/* Runtime services the tools use. */
void vios_out(const char *s) { while (*s) platform_console_putc((unsigned char)*s++); }
void vios_uint(uint32_t n) { char b[12]; snprintf(b, sizeof b, "%u", (unsigned)n); vios_out(b); }
int vios_same(const char *a, const char *b) { return !strcmp(a, b); }
size_t vios_length(const char *s) { return strlen(s); }
uint32_t vios_get32(const unsigned char *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }
static unsigned trials, saves;
static char saved_name[32];
int vios_boot_trial(void) { ++trials; return 1; }
int vios_boot_confirm(void) { return 1; }
int vios_boot_revert(void) { return 1; }
void vios_boot_status(void) { vios_out("boot A confirmed, candidate B\n"); }
void vios_boot_manifest(void) { vios_out("manifest A confirmed modules 1\n"); }
void vios_handles_report(void) { vios_out("handles: console 1\n"); }
int vios_module_save(const char *name, const char *path) { (void)path; ++saves; snprintf(saved_name, sizeof saved_name, "%s", name); return 1; }
/* The workbench screen is not used by the tools. */
int vios_text_owns_display(void) { return 1; }
void vios_text_begin_frame(void) {}
void vios_text_clear(unsigned c, unsigned r, unsigned w, unsigned h) { (void)c; (void)r; (void)w; (void)h; }
uint32_t vios_text_generation(void) { return 0; }
void vios_text_put(unsigned c, unsigned r, const char *s, unsigned n, unsigned char a) { (void)c; (void)r; (void)s; (void)n; (void)a; }
void vios_text_scroll(unsigned top, unsigned bottom, int dy) { (void)top; (void)bottom; (void)dy; }

#include "vios/runtime.h"

/* Helpers (as in tests/vios_tools_host.c). */
static void fail(const char *what, const char *arg) { fprintf(stderr, "vios_shots: %s: %s\n", what, arg); exit(1); }
static void idle(void) { for (int i = 0; i < 4; ++i) { vios_tools_idle(); vds_proto_frame(&p, 20, UINT32_MAX); } }
static void press(uint8_t ascii, uint8_t mods, uint8_t vk) { vds_ui_key(&u, ascii, mods, vk, 1); vds_ui_key(&u, ascii, mods, vk, 0); idle(); }
#ifdef VDS_MAX_CANVASES
/* G5 canvases: memory for the items and backing rasters, as the board's PSRAM. */
static uint8_t canvas_store[VDS_MAX_CANVASES][VDS_CANVAS_ITEMS * sizeof(vds_item) + W / 2 * H];
static void *canvas_mem(void *ctx, uint8_t id, uint32_t n) { (void)ctx; return id < VDS_MAX_CANVASES && n <= sizeof canvas_store[0] ? canvas_store[id] : 0; }
static void finish(void) { for (int i = 0; i < 1000; ++i) { vds_canvas_prepare(&v, UINT32_MAX); if (vds_frame(&v, UINT32_MAX) && !vds_canvas_pending(&v)) return; } }
#else
static void finish(void) { for (int i = 0; i < 1000; ++i) if (vds_frame(&v, UINT32_MAX)) return; }
#endif

/* The Console window: the server-owned surface 16 in window 32. */
#define CON_COLS 80
#define CON_ROWS 58
static vds_cell con_cells[CON_COLS * (CON_ROWS + 1)];
static char con_text[CON_ROWS][CON_COLS];
static unsigned con_row, con_col;
static uint8_t con_attr(int inverse) { return inverse ? (uint8_t)(v.style.body | v.style.title_fg << 4) : (uint8_t)(v.style.title_fg | v.style.body << 4); }
static void con_paint(void) {
    for (unsigned y = 0; y < CON_ROWS; ++y) vds_put(&v, VDS_CONSOLE_SURFACE, 0, (uint8_t)y, con_attr(0), con_text[y], CON_COLS);
    static const char status[] = " VIOS console   F1: editor   Up/Down: history";
    char line[CON_COLS];
    memset(line, ' ', sizeof line);
    memcpy(line, status, sizeof status - 1);
    vds_put(&v, VDS_CONSOLE_SURFACE, 0, CON_ROWS, con_attr(1), line, CON_COLS);
    vds_put(&v, VDS_CONSOLE_SURFACE, (uint8_t)con_col, (uint8_t)con_row, con_attr(1), " ", 1);   /* the cursor */
    const vds_window *w = &v.windows[VDS_CONSOLE_WINDOW];
    unsigned top = con_row >= w->rows ? con_row - w->rows + 1 : 0;
    if (top != w->scroll_row) vds_window_scroll(&v, VDS_CONSOLE_WINDOW, 0, (uint8_t)top);
}
static void con_putc(char c) {
    if (c == '\r') { con_col = 0; return; }
    if (c == '\n' || con_col == CON_COLS) {
        con_col = 0;
        if (++con_row == CON_ROWS) { memmove(con_text[0], con_text[1], sizeof con_text - CON_COLS); memset(con_text[CON_ROWS - 1], ' ', CON_COLS); --con_row; }
        if (c == '\n') return;
    }
    con_text[con_row][con_col++] = c;
}
static void con_puts(const char *s) { while (*s) con_putc(*s++); }
/* Console output of the CPU side (do-it, tasks) goes to the Console window. */
static void con_drain(void) { con_puts(console); console_n = 0; console[0] = 0; }
static void con_window(void) {
    memset(con_text, ' ', sizeof con_text);
    vds_surface_init(&v, VDS_CONSOLE_SURFACE, CON_COLS, CON_ROWS + 1, con_cells, 0x30);
    if (!vds_window_open(&v, VDS_CONSOLE_WINDOW, VDS_CONSOLE_SURFACE, 16, 228, 74, 28,
                         VDS_W_VISIBLE | VDS_W_DECOR | VDS_W_SHADOW | VDS_W_ACTIVE, "Console")) fail("console", "window");
}

static void dump(const char *dir, const char *name) {
    char path[1024];
    con_paint();
    finish();
    snprintf(path, sizeof path, "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) fail("cannot write", path);
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t i = (x & 1) ? fb[y][x >> 1] & 15 : fb[y][x >> 1] >> 4, c = v.style.palette[i];
            uint8_t rgb[3] = {(uint8_t)((c >> 4 & 3) * 85), (uint8_t)((c >> 2 & 3) * 85), (uint8_t)((c & 3) * 85)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void unescape(char *s) {
    char *o = s;
    for (; *s; ++s) {
        if (*s == '\\' && s[1] == 'n') { *o++ = '\n'; ++s; }
        else if (*s == '\\' && s[1] == '\\') { *o++ = '\\'; ++s; }
        else *o++ = *s;
    }
    *o = 0;
}
static const struct { const char *name; uint8_t vk, ascii; } keys[] = {
    {"SPACE", VDS_VK_SPACE, ' '}, {"DELETE", VDS_VK_DELETE, 127}, {"HOME", VDS_VK_HOME, 0}, {"END", VDS_VK_END, 0},
    {"PAGEUP", VDS_VK_PAGEUP, 0}, {"PAGEDOWN", VDS_VK_PAGEDOWN, 0}, {"ESCAPE", VDS_VK_ESCAPE, 27},
    {"BACKSPACE", VDS_VK_BACKSPACE, 8}, {"TAB", VDS_VK_TAB, 9}, {"RETURN", VDS_VK_RETURN, 13}, {"UP", VDS_VK_UP, 0},
    {"DOWN", VDS_VK_DOWN, 0}, {"LEFT", VDS_VK_LEFT, 0}, {"RIGHT", VDS_VK_RIGHT, 0}, {"F1", VDS_VK_F1, 0},
    {"F3", VDS_VK_F3, 0}, {"F5", VDS_VK_F5, 0}, {"F6", VDS_VK_F6, 0}, {"F10", VDS_VK_F10, 0}, {"F12", VDS_VK_F12, 0}};
static uint8_t mods_of(const char *m) {
    uint8_t r = 0;
    for (; m && *m; ++m) r |= *m == 'c' ? VDS_MOD_CTRL : *m == 's' ? VDS_MOD_SHIFT : *m == 'a' ? VDS_MOD_LALT : 0;
    return r;
}
static void letter(char c, uint8_t mods) { press((uint8_t)(c - 'a' + 1), mods, (uint8_t)(22 + c - 'a')); }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: vios_shots FONT.bin OUTDIR < SCENE\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f || fread(font, 1, sizeof font, f) != sizeof font) fail("font", argv[1]);
    fclose(f);
    for (int y = 0; y < H; ++y) rows[y] = fb[y];
    if (!vds_init(&v, rows, W, H, font)) fail("vds_init", "");
    vds_proto_init(&p, &v, cell_mem, esp_send, 0);
#ifdef VDS_MAX_CANVASES
    p.canvas_mem = canvas_mem;
    vds_canvas_icons(&v);
#endif
    vds_ui_init(&u, &p);
    con_window();
    forth_init();
    if (!vios_prelude_init() || !vios_ui_init() || !vios_tools_init()) fail("init", forth_last_error());
    vios_workbench_init();
    idle();
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0; else arg = line + n;
        unescape(arg);
        if (!line[0] || line[0] == '#') continue;
        if (!strcmp(line, "eval")) { if (!forth_eval(arg)) fail(forth_last_error(), arg); idle(); }
        else if (!strcmp(line, "con")) {
            con_drain();
            con_puts("vios> "); con_puts(arg); con_puts("\n");
            int ok = forth_eval(arg);
            con_drain();
            if (ok) con_puts("ok\n"); else { con_puts("error: "); con_puts(forth_last_error()); con_puts("\n"); fvm_reset(); }
            idle();
        }
        else if (!strcmp(line, "conrun")) {
            con_drain();
            con_puts("\n");
            int ok = forth_eval(arg);
            con_drain();
            if (ok) con_puts("ok\n"); else { con_puts("error: "); con_puts(forth_last_error()); con_puts("\n"); fvm_reset(); }
            con_puts("vios> ");
            idle();
        }
        else if (!strcmp(line, "conraw")) con_puts(arg);
        else if (!strcmp(line, "prompt")) con_puts("vios> ");
        else if (!strcmp(line, "module")) {
            char *src = strchr(arg, ' ');
            if (!src) fail("module", arg);
            *src++ = 0;
            if (!forth_module_stage(arg, "1", 0, 0, src, "") || !forth_module_activate(arg)) fail(forth_last_error(), arg);
            idle();
        }
        else if (!strcmp(line, "type")) {
            for (char *s = arg; *s; ++s) { vds_ui_key(&u, (uint8_t)*s, 0, *s == '\n' ? VDS_VK_RETURN : 0, 1); vds_ui_key(&u, (uint8_t)*s, 0, 0, 0); }
            idle();
        }
        else if (!strcmp(line, "key")) {
            char *m = strchr(arg, ' ');
            if (m) *m++ = 0;
            unsigned k = 0;
            for (; k < sizeof keys / sizeof keys[0]; ++k) if (!strcmp(keys[k].name, arg)) break;
            if (k < sizeof keys / sizeof keys[0]) press(keys[k].ascii, mods_of(m), keys[k].vk);
            else if (strlen(arg) == 1) press((uint8_t)arg[0], mods_of(m), 0);
            else fail("unknown key", arg);
        }
        else if (!strcmp(line, "ctrl")) letter(arg[0], VDS_MOD_CTRL);
        else if (!strcmp(line, "ctrlshift")) letter(arg[0], VDS_MOD_CTRL | VDS_MOD_SHIFT);
        else if (!strcmp(line, "focus")) { vds_ui_focus(&u, (uint8_t)atoi(arg)); idle(); }
        else if (!strcmp(line, "place")) {
            unsigned w, x, y;
            if (sscanf(arg, "%u %u %u", &w, &x, &y) != 3 || w >= VDS_MAX_WINDOWS) fail("place", arg);
            vds_window_move(&v, (uint8_t)w, (int16_t)x, (int16_t)y);
            idle();
        }
        else if (!strcmp(line, "idle")) { int k = atoi(arg); if (k < 1) k = 1; while (k--) idle(); }
        else if (!strcmp(line, "shot")) dump(argv[2], arg);
        else fail("unknown scene command", line);
        con_drain();
    }
    return 0;
}
