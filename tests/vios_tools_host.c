/* End-to-end host test of the GUI tools (kernel/vios_tools.c) without a board:
 * the real Forth system, UI words, tool engine and workbench model on the
 * CPU side; the real ESP display server (vds, vds_proto, vds_ui, widgets,
 * TextView) on the other side of a stubbed VDP link. Keys are pressed on
 * the ESP as the keyboard would; the CPU acts only from its idle hook after
 * a nudge, as on the board. Usage: vios_tools_host FONT.bin */
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

/* Helpers. */
static void eval(const char *s) { if (!forth_eval(s)) { fprintf(stderr, "eval failed: %s: %s\n", s, forth_last_error()); abort(); } }
static void idle(void) { for (int i = 0; i < 4; ++i) { vios_tools_idle(); vds_proto_frame(&p, 20, UINT32_MAX); } }
static int key(uint8_t ascii, uint8_t mods, uint8_t vk) {
    int a = vds_ui_key(&u, ascii, mods, vk, 1);
    vds_ui_key(&u, ascii, mods, vk, 0);
    idle();
    return a;
}
static int ctrl(char c) { return key((uint8_t)(c - 'a' + 1), VDS_MOD_CTRL, (uint8_t)(22 + c - 'a')); }
static int ctrl_shift(char c) { return key((uint8_t)(c - 'a' + 1), VDS_MOD_CTRL | VDS_MOD_SHIFT, (uint8_t)(22 + c - 'a')); }
static void type(const char *s) { for (; *s; ++s) { vds_ui_key(&u, (uint8_t)*s, 0, *s == '\n' ? VDS_VK_RETURN : 0, 1); vds_ui_key(&u, (uint8_t)*s, 0, 0, 0); } idle(); }
static const vds_widget *wd(unsigned id) { return &u.widgets[id]; }
static const char *item(unsigned id, unsigned k) { return k < wd(id)->count ? u.items[wd(id)->first + k] : ""; }
static int has_item(unsigned id, const char *s) { for (unsigned k = 0; k < wd(id)->count; ++k) if (strstr(item(id, k), s)) return 1; return 0; }
static const char *text_of(unsigned id) {
    static char t[VDS_TEXT_MAX + 1];
    const vds_text *x = vds_ui_text(&u, (uint8_t)id);
    assert(x);
    memcpy(t, x->buf, x->len);
    t[x->len] = 0;
    return t;
}
static int open(unsigned window) { return v.windows[window].used && (v.windows[window].flags & VDS_W_VISIBLE); }

enum { B_MODULES = 42, B_WORDS, B_INFO, B_CODE, B_STATUS, W_TEXT = 48, I_TABS = 50, I_TABLE, I_DETAIL, M_TABS = 54, M_TABLE, M_DETAIL, D_YES = 60, D_NO };

int main(int argc, char **argv) {
    assert(argc >= 2);
    FILE *f = fopen(argv[1], "rb");
    assert(f && fread(font, 1, sizeof font, f) == sizeof font);
    fclose(f);
    for (int y = 0; y < H; ++y) rows[y] = fb[y];
    assert(vds_init(&v, rows, W, H, font));
    vds_proto_init(&p, &v, cell_mem, esp_send, 0);
    vds_ui_init(&u, &p);
    forth_init();
    assert(vios_ui_init() && vios_tools_init());
    vios_workbench_init();
    static const char demo[] = ": SQUARE DUP * ;\n: CUBE DUP SQUARE * ;\n";
    assert(forth_module_stage("DEMO", "1", 0, 0, demo, "") && forth_module_activate("DEMO"));

    /* Setup: commands, the world menu, the nudge; nothing polled while idle. */
    idle();
    assert(p.nudge && u.menus[0].used && u.menus[0].count == 4 && (u.commands[0].flags & VDS_CMD_USED));
    unsigned before = requests;
    idle(); idle();
    assert(requests == before);                                        /* no events, no traffic */

    /* Ctrl+Shift+B opens the Browser; the modules pane lists DEMO. */
    assert(ctrl_shift('b'));
    assert(open(20) && u.focus == 20 && u.key_policy[20] == VDS_KEYS_DROP && nudges >= 1);
    assert(!strcmp(item(B_MODULES, 0), "(all)") && !strcmp(item(B_MODULES, 2), "DEMO *"));
    assert(wd(B_WORDS)->count == forth_word_count());
    assert(u.widget_focus[20] == B_MODULES);
    assert(key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_DOWN));            /* DEMO */
    assert(wd(B_WORDS)->count == 2 && !strcmp(item(B_WORDS, 0), "SQUARE") && !strcmp(item(B_WORDS, 1), "CUBE"));
    assert(!strcmp(text_of(B_CODE), demo) && has_item(B_INFO, "version 1"));
    assert(strstr(u.widgets[B_STATUS].text, "DEMO: active"));

    /* Accept: a new word in the module's source becomes live (stage + activate). */
    assert(key(0, 0, VDS_VK_TAB) && key(0, 0, VDS_VK_TAB) && key(0, 0, VDS_VK_TAB) && u.widget_focus[20] == B_CODE);
    assert(key(0, VDS_MOD_CTRL, VDS_VK_END));
    type(": QUAD SQUARE SQUARE ;\n");
    assert(vds_ui_text(&u, B_CODE)->dirty);
    assert(ctrl('s'));
    assert(forth_find_xt("QUAD") && strstr(forth_module_source("DEMO", 0), ": QUAD SQUARE SQUARE ;"));
    assert(strstr(u.widgets[B_STATUS].text, "DEMO active; words ") && !vds_ui_text(&u, B_CODE)->dirty);
    eval("3 QUAD");
    assert(fvm_pop() == 81);
    /* A broken accept leaves the live module alone and says why. */
    type(": BROKEN UNDEFINED-WORD ;\n");
    assert(ctrl('s'));
    assert(!forth_find_xt("BROKEN") && strstr(u.widgets[B_STATUS].text, "not accepted"));
    /* Accept again and again (older versions are retired). */
    for (int i = 0; i < 2; ++i) {
        assert(key(0, VDS_MOD_CTRL, VDS_VK_END));
        while (strstr(text_of(B_CODE), "BROKEN")) {                                   /* drop the BROKEN line */
            key(0, 0, VDS_VK_BACKSPACE);
            const char *t = text_of(B_CODE);
            if (strlen(t) >= 2 && !strcmp(t + strlen(t) - 2, ";\n") && !strstr(t, "BROKEN")) break;
        }
        while (text_of(B_CODE)[strlen(text_of(B_CODE)) - 1] != '\n') key(0, 0, VDS_VK_BACKSPACE);
        type(i ? ": FIVE 5 ;\n" : ": FOUR 4 ;\n");
        assert(ctrl('s') && strstr(u.widgets[B_STATUS].text, "DEMO active"));
    }
    eval("FOUR FIVE +");
    assert(fvm_pop() == 9);
    /* Save to SD from the pane menu (command), after a revert to the live source. */
    unsigned char revert[] = {0x7F, 0x06};
    (void)revert;
    assert(vds_ui_focus(&u, 20) && key(0, VDS_MOD_SHIFT, VDS_VK_F10) && u.depth == 1);   /* pane menu */
    assert(key('v', 0, 0) && !strstr(text_of(B_CODE), "BROKEN"));        /* Re&vert */
    assert(key(0, VDS_MOD_SHIFT, VDS_VK_F10) && key('s', 0, 0) && saves == 1 && !strcmp(saved_name, "DEMO"));
    /* A word: its retained source; accept redefines it. */
    assert(vds_ui_widget_focus(&u, B_WORDS) && key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_UP));
    assert(strstr(text_of(B_CODE), "SQUARE DUP *") && has_item(B_INFO, "colon"));

    /* Workspace: print it, do it, errors in place, inspect it. */
    unsigned words = forth_word_count();
    assert(ctrl_shift('w') && open(21) && u.focus == 21 && u.widget_focus[21] == W_TEXT);
    assert(key(0, VDS_MOD_CTRL, VDS_VK_END));
    type("2 3 *");
    assert(ctrl('p'));
    assert(strstr(text_of(W_TEXT), "2 3 * 6") && fvm_depth == 1 && fvm_pop() == 6);
    size_t seen = console_n;
    type("\n1 2 + .");
    assert(ctrl('d') && strstr(console + seen, "3 "));                  /* do it: output in the transcript */
    type("\nNO-SUCH-WORD");
    assert(ctrl('d') && strstr(text_of(W_TEXT), "NO-SUCH-WORD error:"));
    type("\n: TWICE 2 * ; 21 TWICE");
    assert(ctrl('i') && forth_word_count() == words + 1 && open(22) && u.focus == 22);
    assert(!strcmp(item(I_TABLE, 0), "0\t00002A\t42") && wd(I_TABS)->value == 0);

    /* Inspector: tabs, words with SEE, memory paging, tasks, system. */
    size_t quiet = console_n;
    assert(vds_ui_widget_focus(&u, I_TABLE));
    assert(key(9, VDS_MOD_CTRL, VDS_VK_TAB) && wd(I_TABS)->value == 1 && wd(I_TABLE)->count == 16);
    assert(!strncmp(item(I_TABLE, 0), "000000", 6));
    assert(key(0, 0, VDS_VK_HOME) && key(0, 0, VDS_VK_UP));                  /* past the top: one row up */
    assert(!strncmp(item(I_TABLE, 0), "FFFFF8", 6));
    assert(key(0, 0, VDS_VK_END) && key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_DOWN));
    assert(!strncmp(item(I_TABLE, 0), "000008", 6) && wd(I_TABLE)->value == 15);
    assert(key(9, VDS_MOD_CTRL, VDS_VK_TAB) && wd(I_TABS)->value == 2 && strstr(text_of(I_DETAIL), "task-slice"));
    assert(key(9, VDS_MOD_CTRL, VDS_VK_TAB) && strstr(text_of(I_DETAIL), "handles: console"));
    assert(key(9, VDS_MOD_CTRL, VDS_VK_TAB) && wd(I_TABS)->value == 4);
    assert(strstr(text_of(I_DETAIL), "display epoch") && strstr(text_of(I_DETAIL), "words"));
    assert(key(9, VDS_MOD_CTRL, VDS_VK_TAB) && wd(I_TABS)->value == 0 && !strcmp(item(I_TABLE, 0), "0\t00002A\t42"));
    eval("' SQUARE");                                                       /* an xt on the stack: SEE in the detail */
    assert(ctrl('r') && wd(I_TABLE)->count == 2 && vds_ui_widget_focus(&u, I_TABLE) && key(0, 0, VDS_VK_DOWN) && key(0, 0, VDS_VK_UP));
    assert(strstr(text_of(I_DETAIL), "SQUARE"));
    fvm_pop(); fvm_pop();
    assert(console_n == quiet);                                         /* reports never reach the console */

    /* Manager: boot report; an action asks first, Esc or No cancels. */
    assert(ctrl_shift('m') && open(23) && strstr(text_of(M_DETAIL), "boot A confirmed") && wd(M_TABLE)->count == 3);
    assert(vds_ui_widget_focus(&u, M_TABLE) && key(0, 0, VDS_VK_RETURN) && open(24) && u.focus == 24);
    assert(u.widget_focus[24] == D_NO && key(0, 0, VDS_VK_RETURN) && !open(24) && !trials);
    assert(vds_ui_focus(&u, 23) && vds_ui_widget_focus(&u, M_TABLE) && key(0, 0, VDS_VK_RETURN) && open(24));
    assert(key(0, 0, VDS_VK_ESCAPE) && !open(24) && !trials);
    assert(vds_ui_focus(&u, 23) && vds_ui_widget_focus(&u, M_TABLE) && key(0, 0, VDS_VK_RETURN));
    assert(key(9, VDS_MOD_SHIFT, VDS_VK_TAB) && u.widget_focus[24] == D_YES && key(0, 0, VDS_VK_RETURN));
    assert(trials == 1 && !open(24) && strstr(text_of(M_DETAIL), "done"));
    assert(vds_ui_focus(&u, 23) && vds_ui_widget_focus(&u, M_TABS) && key(0, 0, VDS_VK_RIGHT) && wd(M_TABS)->value == 1 && has_item(M_TABLE, "DEMO\t1\tactive"));

    /* Applications keep their own events: a button in window 1 reaches UI-NEXT. */
    eval("0 30 4 48 UI-SURFACE 1 0 0 400 30 4 7 UI-WINDOW 5 1 UI-BUTTON UI-NONE 0 0 10 1 0 UI-WIDGET S\" Go\" 5 UI-WTEXT 1 UI-FOCUS");
    idle();
    assert(u.focus == 1 && vds_ui_key(&u, ' ', 0, VDS_VK_SPACE, 1));
    idle();
    int activated = 0;
    for (int i = 0; i < 6 && !activated; ++i) {                             /* past FOCUS events */
        eval("UI-NEXT");
        if (!fvm_pop()) break;
        fcell arg = fvm_pop(), id = fvm_pop(), kind = fvm_pop();
        activated = kind == 2 && id == 5 && arg == 1;                       /* ACTIVATE 5 in window 1 */
        assert(id < 40);                                                    /* never a tool widget's event */
    }
    assert(activated);

    /* Ctrl+W closes a tool (a close request the CPU grants); TOOL reopens it. */
    assert(vds_ui_focus(&u, 22) && ctrl('w') && !open(22) && open(23));
    eval("2 TOOL");
    idle();
    assert(open(22) && u.focus == 22);
    assert(!forth_eval("7 TOOL"));
    fvm_reset();
    assert(!p.errors && !fvm_depth);
    printf("vios tools checks passed: %u nudges, %u requests, %u bytes to the ESP, %u words\n", nudges, requests, bytes_to_esp, forth_word_count());
    return 0;
}
