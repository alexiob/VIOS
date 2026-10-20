/* GUI development tools (docs/superpowers/specs/2026-10-20-vios-gui-tools-design.md):
 * System Browser, Workspace, Inspector and Manager windows on the ESP display
 * server. The ESP does navigation, scrolling and editing locally; this engine
 * only fills widgets and acts on the semantic events it claims from the
 * shared event poll (kernel/vios_ui.c). It runs from the REPL idle hook, i.e.
 * with an empty input line and nothing compiling, so evaluating text here is
 * exactly what typing it would do. Reports reuse the existing textual
 * commands, with console output diverted into a read-only TextView. */
#include "vios/tools.h"
#include "vios/ui.h"
#include "vios/runtime.h"
#include "vios/workbench.h"
#include "vios/text_screen.h"
#include "forth/inspect.h"
#include "forth/vm.h"
#include "platform/console.h"
#include "platform/timer.h"
#include "platform/vdp.h"

enum { CMD = 0x7F00, C_BROWSER = CMD, C_WORKSPACE, C_INSPECTOR, C_MANAGER, C_ACCEPT, C_SAVE, C_REVERT, C_REFRESH,
       C_KILL, C_GOTO, C_ROLLBACK, C_LAST = C_ROLLBACK, A_TRIAL, A_CONFIRM, A_REVERT };
enum { BROWSER, WORKSPACE, INSPECTOR, MANAGER, DIALOG, TOOLS = DIALOG };
enum { EV_ACTIVATE = 2, EV_SELECT = 3, EV_TEXT = 5, EV_PAGE = 9, EV_WIN = 0x10, EV_COMMAND = 0x11 };
enum { K_BOX = 1, K_LABEL, K_BUTTON, K_LIST = 7, K_TEXT = 9, K_TABS = 10 };
enum { F_FILL = 4, F_READONLY = 8, F_PAGER = 32 };
/* Widget ids 40..63 belong to the tools. */
enum { B_ROOT = 40, B_ROW, B_MODULES, B_WORDS, B_INFO, B_CODE, B_STATUS,
       W_ROOT, W_TEXT,
       I_ROOT, I_TABS, I_TABLE, I_DETAIL,
       M_ROOT, M_TABS, M_TABLE, M_DETAIL,
       D_ROOT, D_LABEL, D_ROW, D_YES, D_NO, LAST_WIDGET = D_NO };
/* Tool t is window 20 + t (the dialog is 24). */
#define WINDOW_OF(t) ((unsigned char)(20 + (t)))
static const char TITLES[] = "Browser\0Workspace\0Inspector\0Manager\0Confirm";
static unsigned char open_tools, ready, inspector_tab, manager_tab, first_poll;
static unsigned epoch, module_row, word_row, table_row[2], pending, pending_arg;
static uint32_t since_poll;
static unsigned short last_tick;
static char code_module[32];
static unsigned char code_is_word;
static fcell memory_base;
#define QUEUE 6
static unsigned char queue[QUEUE][VIOS_UI_EVENT_MAX], queued;   /* claimed during a poll, handled after it */
static char scratch[256];                                        /* text to evaluate: one REPL line */

/* ------------------------------------------------------------ wire helpers */

static void send(const unsigned char *o, unsigned n) { vios_ui_op(o, n); }
static void op2(unsigned char code, unsigned char a) { unsigned char o[] = {code, a}; send(o, 2); }
static unsigned len(const char *s) { return (unsigned)vios_length(s); }
static void wtext(unsigned char id, const char *s) {
    unsigned char o[3 + 47] = {0x81, id};
    unsigned n = len(s) > 47 ? 47 : len(s);
    o[2] = (unsigned char)n;
    for (unsigned i = 0; i < n; ++i) o[3 + i] = (unsigned char)s[i];
    send(o, 3 + n);
}
static void wvalue(unsigned char id, unsigned v) { unsigned char o[] = {0x82, id, (unsigned char)v, (unsigned char)(v >> 8)}; send(o, 4); }
static void columns(unsigned char id, unsigned char a, unsigned char b, unsigned char c, const char *header) {
    unsigned char o[] = {0x93, id, 3, a, b, c};
    send(o, 6);
    wtext(id, header);
}

/* Rows go out several to an op (92: id k (n text)*k). */
static unsigned char rows_op[3 + 200];
static unsigned rows_used;
static void rows_flush(void) { if (rows_op[2]) send(rows_op, rows_used); rows_op[2] = 0; rows_used = 3; }
static void rows_begin(unsigned char list) {
    unsigned char clear[] = {0x91, list, 0, 0};
    send(clear, 4);
    rows_op[0] = 0x92; rows_op[1] = list; rows_op[2] = 0; rows_used = 3;
}
static void row(const char *s, unsigned n) {
    if (n > 31) n = 31;
    if (rows_used + 1 + n > sizeof rows_op || rows_op[2] == 255) rows_flush();
    rows_op[rows_used++] = (unsigned char)n;
    for (unsigned i = 0; i < n; ++i) rows_op[rows_used++] = (unsigned char)s[i];
    ++rows_op[2];
}

/* A row, label or short text under construction. */
static char line[48];
static unsigned at;
static void put_s(const char *s) { while (s && *s && at < sizeof line - 1) line[at++] = *s++; }
static void put_c(char c) { if (at < sizeof line - 1) line[at++] = c; }
static void put_u(uint32_t v) { char d[10]; unsigned n = 0; do d[n++] = (char)('0' + v % 10); while (v /= 10); while (n) put_c(d[--n]); }
static void put_x(uint32_t v, unsigned digits) { while (digits--) put_c("0123456789ABCDEF"[(v >> (digits * 4)) & 15]); }
static void put_row(void) { row(line, at); at = 0; }
static const char *put_end(void) { line[at] = 0; at = 0; return line; }

/* Console output into a TextView (appended), for reports and SEE. */
static unsigned char sink_view;
static char sink_buf[64];
static unsigned sink_used;
static void (*sink_saved)(unsigned char);
static void sink_flush(void) { if (sink_used) vios_ui_text(sink_view, sink_buf, sink_used, 1); sink_used = 0; }
static void sink(unsigned char c) { sink_buf[sink_used++] = (char)c; if (sink_used == sizeof sink_buf) sink_flush(); }
static void capture(unsigned char view) { sink_used = 0; sink_view = view; sink_saved = platform_console_divert(sink); }
static void release(void) { sink_flush(); platform_console_divert(sink_saved); }
static void view_clear(unsigned char view) { vios_ui_text(view, "", 0, 0); }
static void report(unsigned char view, const char *source) {
    capture(view);
    if (!forth_eval(source)) { vios_out("error: "); vios_out(forth_last_error()); vios_out("\n"); }
    release();
}
static void status(unsigned char id, const char *a, const char *b) { put_s(a); put_s(b); wtext(id, put_end()); }

/* ------------------------------------------------------------ windows, commands */

/* Static op tables: entries are a length and that many bytes of whole ops. */
#define WINDOW(t, s, x, y, cols, rows, flags, menu) 21, 0x01, s, cols, rows, 0x30, 0x50, 20 + t, s, (x) & 255, (x) >> 8, y, 0, cols, rows, flags, \
    0x5A, 20 + t, 1, 0x59, 20 + t, menu
#define WIDGET(id, t, kind, parent, fixed, weight, flags) 14, 0x80, id, 20 + t, kind, parent, 0, 0, 0, 0, flags, 0x84, id, fixed, weight
#define LAYOUT(id, how, gap) 4, 0x83, id, how, gap
#define WTEXT(id, n, ...) 3 + n, 0x81, id, n, __VA_ARGS__
#define DECOR 7                                       /* visible, decorated, shadow */
static const unsigned char BUILD_BROWSER[] = {
    WINDOW(BROWSER, 6, 16, 16, 76, 40, DECOR, 12), WIDGET(B_ROOT, BROWSER, K_BOX, 255, 0, 0, F_FILL), LAYOUT(B_ROOT, 1, 0),
    WIDGET(B_ROW, BROWSER, K_BOX, B_ROOT, 0, 1, 0), LAYOUT(B_ROW, 2, 1), WIDGET(B_MODULES, BROWSER, K_LIST, B_ROW, 16, 0, 0),
    WIDGET(B_WORDS, BROWSER, K_LIST, B_ROW, 20, 0, 0), WIDGET(B_INFO, BROWSER, K_LIST, B_ROW, 0, 1, 0),
    WIDGET(B_CODE, BROWSER, K_TEXT, B_ROOT, 0, 2, 0), WIDGET(B_STATUS, BROWSER, K_LABEL, B_ROOT, 1, 0, 0), 0};
static const unsigned char BUILD_WORKSPACE[] = {
    WINDOW(WORKSPACE, 7, 128, 40, 60, 16, DECOR, 255), WIDGET(W_ROOT, WORKSPACE, K_BOX, 255, 0, 0, F_FILL), LAYOUT(W_ROOT, 1, 0),
    WIDGET(W_TEXT, WORKSPACE, K_TEXT, W_ROOT, 0, 1, 0), 0};
static const unsigned char BUILD_INSPECTOR[] = {
    WINDOW(INSPECTOR, 8, 32, 32, 70, 34, DECOR, 14), WIDGET(I_ROOT, INSPECTOR, K_BOX, 255, 0, 0, F_FILL), LAYOUT(I_ROOT, 1, 0),
    WIDGET(I_TABS, INSPECTOR, K_TABS, I_ROOT, 1, 0, 0), WIDGET(I_TABLE, INSPECTOR, K_LIST, I_ROOT, 0, 1, F_PAGER),
    WIDGET(I_DETAIL, INSPECTOR, K_TEXT, I_ROOT, 0, 1, F_READONLY), 0};
static const unsigned char BUILD_MANAGER[] = {
    WINDOW(MANAGER, 9, 96, 48, 64, 30, DECOR, 15), WIDGET(M_ROOT, MANAGER, K_BOX, 255, 0, 0, F_FILL), LAYOUT(M_ROOT, 1, 0),
    WIDGET(M_TABS, MANAGER, K_TABS, M_ROOT, 1, 0, 0), WIDGET(M_TABLE, MANAGER, K_LIST, M_ROOT, 0, 1, 0),
    WIDGET(M_DETAIL, MANAGER, K_TEXT, M_ROOT, 0, 1, F_READONLY), 0};
static const unsigned char BUILD_DIALOG[] = {
    WINDOW(DIALOG, 5, 176, 200, 36, 6, DECOR | 32, 255), WIDGET(D_ROOT, DIALOG, K_BOX, 255, 0, 0, F_FILL), LAYOUT(D_ROOT, 1, 1),
    WIDGET(D_LABEL, DIALOG, K_LABEL, D_ROOT, 3, 0, 0), WIDGET(D_ROW, DIALOG, K_BOX, D_ROOT, 1, 0, 0), LAYOUT(D_ROW, 2, 2),
    WIDGET(D_YES, DIALOG, K_BUTTON, D_ROW, 0, 1, 0), WTEXT(D_YES, 4, '&', 'Y', 'e', 's'),
    WIDGET(D_NO, DIALOG, K_BUTTON, D_ROW, 0, 1, 0), WTEXT(D_NO, 3, '&', 'N', 'o'), 0};
static const unsigned char *const BUILDS[] = {BUILD_BROWSER, BUILD_WORKSPACE, BUILD_INSPECTOR, BUILD_MANAGER, BUILD_DIALOG};
static void send_table(const unsigned char *t) { for (; *t; t += 1 + *t) send(t + 1, *t); }
static void window(unsigned tool) {
    const char *title = TITLES;
    for (unsigned i = 0; i < tool; ++i) title += len(title) + 1;
    send_table(BUILDS[tool]);
    unsigned char o[3 + 9] = {0x54, WINDOW_OF(tool), (unsigned char)len(title)};
    for (unsigned i = 0; i < o[2]; ++i) o[3 + i] = (unsigned char)title[i];
    send(o, 3 + o[2]);
}
/* Commands: id offset, modifiers (Ctrl 1, Shift 2), key, label. */
static const struct { unsigned char id, mods; char key; const char *label; } COMMANDS[] = {
    {0, 3, 'b', "&Browser"}, {1, 3, 'w', "&Workspace"}, {2, 3, 'i', "&Inspector"}, {3, 3, 'm', "&Manager"},
    {4, 0, 0, "&Accept  Ctrl+S"}, {5, 0, 0, "&Save to SD"}, {6, 0, 0, "Re&vert"}, {7, 1, 'r', "&Refresh"},
    {8, 0, 0, "&Kill task"}, {9, 0, 0, "&Go to address"}, {10, 0, 0, "Roll&back"}};
#define ITEM(c) (c) & 255, (c) >> 8
static const unsigned char MENUS[] = {
    17, 0x61, 0, 5, 'W', 'o', 'r', 'l', 'd', 4, ITEM(C_BROWSER), ITEM(C_WORKSPACE), ITEM(C_INSPECTOR), ITEM(C_MANAGER),
    12, 0x61, 12, 0, 4, ITEM(C_ACCEPT), ITEM(C_SAVE), ITEM(C_REVERT), ITEM(C_REFRESH),
    10, 0x61, 14, 0, 3, ITEM(C_REFRESH), ITEM(C_GOTO), ITEM(C_KILL),
    10, 0x61, 15, 0, 3, ITEM(C_REFRESH), ITEM(C_SAVE), ITEM(C_ROLLBACK),
    4, 0x74, 3, 1, 0, 0};                             /* nudge: poll only when events wait */
static void focus(unsigned tool) { op2(0x58, WINDOW_OF(tool)); }

/* ------------------------------------------------------------ browser */

static int word_in(const struct forth_word_info *w, unsigned module) {
    struct forth_module_info m;
    if (module == 0) return 1;
    if (module == 1) return !w->module;
    return w->module && forth_module_info(module - 2, &m) && vios_same(w->module, m.name);
}
static int word_at(unsigned k) {                       /* word index of row k of the words pane */
    struct forth_word_info w;
    for (unsigned i = 0; i < forth_word_count(); ++i)
        if (forth_inspect_word(i, &w) && word_in(&w, module_row) && !k--) return (int)i;
    return -1;
}
static void browser_modules(void) {
    struct forth_module_info m;
    rows_begin(B_MODULES);
    row("(all)", 5); row("(core)", 6);
    for (unsigned i = 0; forth_module_info(i, &m); ++i) { put_s(m.name); if (m.active) put_s(" *"); put_row(); }
    rows_flush();
    wvalue(B_MODULES, module_row);
}
static void browser_module(void) {                     /* words, facts and source of the selected module */
    struct forth_word_info w;
    struct forth_module_info m, o;
    struct forth_module_dependency d;
    rows_begin(B_WORDS);
    for (unsigned i = 0; i < forth_word_count(); ++i) if (forth_inspect_word(i, &w) && word_in(&w, module_row)) row(w.name, len(w.name));
    rows_flush();
    word_row = 0;
    rows_begin(B_INFO);
    code_module[0] = 0;
    code_is_word = 0;
    view_clear(B_CODE);
    if (module_row >= 2 && forth_module_info(module_row - 2, &m)) {
        put_s("version "); put_s(m.version); put_row();
        put_s("crc "); put_x(m.source_crc32, 8); put_row();
        put_s("words "); put_u(m.words); put_s(" token "); put_u((uint32_t)m.token_bytes); put_row();
        for (unsigned i = 0; forth_module_dependency_at(m.name, 0, i, &d); ++i) {
            int ok = 0;
            for (unsigned j = 0; forth_module_info(j, &o); ++j) if (vios_same(o.name, d.name)) ok = o.source_crc32 == d.source_crc32;
            put_s("needs "); put_s(d.name); put_s(ok ? " ok" : " stale"); put_row();
        }
        for (unsigned j = 0; forth_module_info(j, &o); ++j)
            for (unsigned i = 0; forth_module_dependency_at(o.name, 0, i, &d); ++i)
                if (vios_same(d.name, m.name)) { put_s("used by "); put_s(o.name); put_row(); }
        const char *source = forth_module_source(m.name, 0);
        for (unsigned i = 0; i < 32; ++i) if (!(code_module[i] = m.name[i])) break;
        code_module[31] = 0;
        if (source) vios_ui_text(B_CODE, source, len(source), 0);
        status(B_STATUS, m.name, m.active ? ": active  Ctrl+S accept  Shift+F10 menu" : ": not active");
    } else status(B_STATUS, module_row ? "core words" : "all words", "  Tab: next pane");
    rows_flush();
}
static const char *const KINDS[] = {"primitive", "colon", "constant", "variable", "defer", "native"};
static void browser_word(void) {
    struct forth_word_info w;
    int i = word_at(word_row);
    if (i < 0 || !forth_inspect_word((unsigned)i, &w)) return;
    rows_begin(B_INFO);
    put_s(w.kind < 6 ? KINDS[w.kind] : "?"); put_s(" xt "); put_u(w.xt); put_row();
    put_s("token "); put_u(w.token_bytes); put_s(" direct "); put_u(w.direct_bytes); put_row();
    if (w.module) { put_s("module "); put_s(w.module); put_row(); }
    if (w.value_valid) { put_s("value "); put_u(w.value); put_row(); }
    rows_flush();
    view_clear(B_CODE);
    if (w.source && w.source_length) vios_ui_text(B_CODE, w.source, w.source_length, 0);
    code_is_word = 1;
    status(B_STATUS, w.name, w.module ? ": accept redefines it outside its module" : ": Ctrl+S accept redefines it");
}

/* A TextView range into scratch, NUL-terminated; 0 if it does not fit. */
static void to_scratch(void *ctx, const char *s, unsigned n) { unsigned *k = ctx; for (unsigned i = 0; i < n; ++i) scratch[(*k)++] = s[i]; }
static int read_range(unsigned char view, unsigned lo, unsigned hi) {
    unsigned k = 0;
    if (hi < lo || hi - lo >= sizeof scratch || !vios_ui_text_read(view, lo, hi - lo, to_scratch, &k)) return 0;
    scratch[k] = 0;
    return 1;
}
/* Stream a TextView into the workbench draft chunk by chunk: no CPU buffer. */
static size_t draft_at;
static int draft_ok;
static void to_draft(void *ctx, const char *s, unsigned n) {
    struct vios_workbench_status st;
    (void)ctx;
    vios_workbench_status(&st);
    if (draft_ok && !vios_workbench_patch(st.revision, draft_at, 0, s, n)) draft_ok = 0;
    draft_at += n;
}
/* The draft becomes this module's source; a draft of the same module may be
 * replaced (the browser's text is that edit), another module's dirty draft not. */
static int edit_draft(const char *name) {
    struct vios_workbench_status st;
    vios_workbench_status(&st);
    return vios_workbench_edit(name, st.has_draft && vios_same(st.module, name));
}
static void browser_accept(void) {
    struct vios_ui_text_info info;
    struct vios_workbench_status st;
    if (!vios_ui_text_info(B_CODE, &info)) return;
    if (code_is_word) {                                    /* a definition: evaluate it, like a method accept */
        if (!read_range(B_CODE, 0, info.length)) { status(B_STATUS, "too long to evaluate", ""); return; }
        if (forth_eval(scratch)) { wvalue(B_CODE, 0); status(B_STATUS, "accepted", ""); }
        else status(B_STATUS, "error: ", forth_last_error());
        return;
    }
    if (!code_module[0]) { status(B_STATUS, "select a module or a word first", ""); return; }
    if (!edit_draft(code_module)) { status(B_STATUS, "workbench: ", vios_workbench_error()); return; }
    vios_workbench_status(&st);
    draft_at = 0;
    draft_ok = vios_workbench_patch(st.revision, 0, st.length, 0, 0);
    if (!vios_ui_text_read(B_CODE, 0, info.length, to_draft, 0)) draft_ok = 0;
    vios_workbench_status(&st);
    if (!draft_ok || !vios_workbench_stage(st.revision) || !vios_workbench_activate(st.revision)) {
        status(B_STATUS, "not accepted: ", vios_workbench_error());
        return;
    }
    wvalue(B_CODE, 0);
    browser_modules();
    /* Every accept keeps the retired version's code until restart: show the pools. */
    struct forth_resources r;
    forth_inspect_resources(&r);
    unsigned code = (unsigned)(r.direct_bytes * 100 / r.direct_capacity), words = r.words * 100 / r.words_max;
    put_s(code_module); put_s(" active; words "); put_u(words); put_s("% code "); put_u(code); put_c('%');
    if (code > 80 || words > 80) put_s(": save, restart");
    wtext(B_STATUS, put_end());
}
static int save_module(const char *name) {
    struct vios_workbench_status st;
    if (!edit_draft(name)) return 0;
    vios_workbench_status(&st);
    return vios_workbench_can_save(st.revision) && vios_module_save(name, 0) && vios_workbench_mark_saved(st.revision);
}

/* ------------------------------------------------------------ inspector */

static int task_at(unsigned k) {                       /* task id of row k of the tasks tab */
    struct forth_task_info t;
    for (unsigned i = 1; i <= FORTH_TASKS_MAX; ++i) if (forth_task_info(i, &t) && t.state && !k--) return (int)i;
    return 0;
}
static void server_status(void) {
    unsigned char o[] = {0x31, 1}, r[32];
    unsigned n = 0;
    if (vios_ui_request(o, 2, r, sizeof r, &n) && n >= 9) {
        vios_out("display epoch "); vios_uint((uint32_t)(r[2] | r[3] << 8)); vios_out(" errors "); vios_uint(r[6]);
    }
    o[0] = 0x33; o[1] = 0;
    if (vios_ui_request(o, 2, r, sizeof r, &n) && n >= 10) { vios_out(" frames "); vios_uint(vios_get32(r + 2)); }
    vios_out(" ESP restarts "); vios_uint(platform_vdp_epoch()); vios_out("\n");
}
static void inspector_fill(void) {
    struct forth_task_info t;
    rows_begin(I_TABLE);
    view_clear(I_DETAIL);
    table_row[0] = 0;
    switch (inspector_tab) {
    case 0:                                                /* data stack, top first */
        columns(I_TABLE, 4, 9, 10, "#\thex\tdecimal");
        for (unsigned i = 0; i < fvm_depth; ++i) {
            fcell v = fvm_ds[fvm_depth - 1 - i];
            put_u(i); put_c('\t'); put_x(v, 6); put_c('\t'); put_u(v); put_row();
        }
        break;
    case 1:                                                /* 16 rows of 8 bytes; PAGE events move */
        columns(I_TABLE, 0, 0, 0, "");
        for (unsigned r = 0; r < 16; ++r) {
            fcell a = (memory_base + r * 8) & FVM_MASK;
            put_x(a, 6);
            for (unsigned i = 0; i < 8; ++i) {
                unsigned char c = fvm_read8(a + i);
                put_c(' ');
                if (fvm_error) { fvm_error = 0; put_s("--"); } else put_x(c, 2);
            }
            put_row();
        }
        break;
    case 2:
        columns(I_TABLE, 4, 16, 8, "id\tname\tstate");
        for (unsigned i = 1; i <= FORTH_TASKS_MAX; ++i)
            if (forth_task_info(i, &t) && t.state) { put_u(i); put_c('\t'); put_s(t.name ? t.name : "?"); put_c('\t'); put_u(t.state); put_row(); }
        report(I_DETAIL, "TASKS");
        break;
    case 3: columns(I_TABLE, 0, 0, 0, ""); capture(I_DETAIL); vios_handles_report(); release(); break;
    default: columns(I_TABLE, 0, 0, 0, ""); report(I_DETAIL, "RESOURCES"); capture(I_DETAIL); server_status(); release();
    }
    rows_flush();
}
static void inspector_select(unsigned k) {
    const char *name = 0;
    table_row[0] = k;
    if (inspector_tab == 0 && k < fvm_depth) name = forth_xt_name(fvm_ds[fvm_depth - 1 - k]);
    if (!name) return;
    view_clear(I_DETAIL);
    put_s("SEE "); put_s(name);
    report(I_DETAIL, put_end());
}

/* ------------------------------------------------------------ manager */

static const char *const BOOT_ROWS[] = {"Trial the candidate slot", "Confirm the running slot", "Revert to the confirmed slot"};
static void manager_fill(void) {
    struct forth_module_info m;
    rows_begin(M_TABLE);
    view_clear(M_DETAIL);
    table_row[1] = 0;
    if (manager_tab == 0) {
        columns(M_TABLE, 0, 0, 0, "");
        for (unsigned i = 0; i < 3; ++i) row(BOOT_ROWS[i], len(BOOT_ROWS[i]));
        capture(M_DETAIL); vios_out("Enter: run the selected action\n"); vios_boot_status(); vios_boot_manifest(); release();
    } else {
        columns(M_TABLE, 14, 10, 8, "module\tversion\tstate");
        for (unsigned i = 0; forth_module_info(i, &m); ++i) { put_s(m.name); put_c('\t'); put_s(m.version); put_c('\t'); put_s(m.active ? "active" : "-"); put_row(); }
        capture(M_DETAIL); vios_out("Enter: save the module to SD\nShift+F10: rollback the latest activation\n"); release();
    }
    rows_flush();
}

/* ------------------------------------------------------------ dialog */

static void ask(const char *question, unsigned action, unsigned arg) {
    pending = action;
    pending_arg = arg;
    if (!(open_tools & (1u << DIALOG))) { window(DIALOG); open_tools |= 1u << DIALOG; }
    wtext(D_LABEL, question);
    focus(DIALOG);
    op2(0x88, D_NO);                                       /* Enter on its own cancels */
}
static void close_tool(unsigned tool) {
    op2(0x57, WINDOW_OF(tool));
    open_tools &= (unsigned char)~(1u << tool);
}
static void run(unsigned action, unsigned arg) {
    struct forth_module_info m;
    int ok = 1;
    if (action == C_KILL) { put_u(arg); put_s(" TASK-KILL"); ok = arg && forth_eval(put_end()); if (open_tools & (1u << INSPECTOR)) inspector_fill(); return; }
    if (action == C_SAVE) ok = forth_module_info(arg, &m) && save_module(m.name);
    else if (action == C_ROLLBACK) ok = forth_module_info(arg, &m) && forth_module_rollback(m.name);
    else if (action == A_TRIAL) ok = vios_boot_trial();
    else if (action == A_CONFIRM) ok = vios_boot_confirm();
    else if (action == A_REVERT) ok = vios_boot_revert();
    if (open_tools & (1u << MANAGER)) {
        manager_fill();
        capture(M_DETAIL);
        vios_out(ok ? "done\n" : "failed: "); if (!ok) { vios_out(forth_last_error()); vios_out("\n"); }
        release();
    }
}

/* ------------------------------------------------------------ opening */

static void build(unsigned tool) {
    vios_ui_begin();
    window(tool);
    if (tool == BROWSER) { module_row = 0; browser_modules(); browser_module(); }
    else if (tool == WORKSPACE) {
        static const char hint[] = "\\ Ctrl+D do it, Ctrl+P print it, Ctrl+I inspect it\n3 4 + .\n";
        vios_ui_text(W_TEXT, hint, sizeof hint - 1, 0);
    } else if (tool == INSPECTOR) { wtext(I_TABS, "Stack|Memory|Tasks|Handles|System"); inspector_fill(); }
    else { wtext(M_TABS, "Boot|Modules"); manager_fill(); }
    focus(tool);
    vios_ui_commit();
    open_tools |= (unsigned char)(1u << tool);
}
int vios_tools_open(unsigned tool) {
    if (tool >= TOOLS || !ready) return 0;
    if (open_tools & (1u << tool)) focus(tool); else build(tool);
    return 1;
}
unsigned vios_tools_open_mask(void) { return open_tools; }

/* ------------------------------------------------------------ events */

static unsigned tool_of(unsigned char window) { return window >= 20 && window <= 24 ? window - 20u : 255; }
/* Tool commands, events of tool windows and of tool widgets are the tools'. */
static int claim(const unsigned char *e, unsigned n) {
    if (n < 3) return 0;
    unsigned kind = e[1], cmd = n >= 5 ? (unsigned)(e[3] | e[4] << 8) : 0;
    int mine = (kind == EV_COMMAND && cmd >= CMD && cmd <= C_LAST) || ((kind == EV_WIN || kind == 6) && tool_of(e[2]) != 255) ||
               (kind >= 2 && kind <= 9 && kind != 6 && kind != 7 && e[2] >= B_ROOT && e[2] <= LAST_WIDGET);
    if (!mine) return 0;
    if (queued < QUEUE) { for (unsigned i = 0; i < n; ++i) queue[queued][i] = e[i]; ++queued; }
    return 1;
}

static void insert_after(unsigned hi) {                /* line[0..at) after the selection, selected */
    unsigned char sel[6] = {0x8B, W_TEXT, (unsigned char)hi, (unsigned char)(hi >> 8), (unsigned char)hi, (unsigned char)(hi >> 8)};
    unsigned char o[3 + sizeof line] = {0x8D, W_TEXT, (unsigned char)at};
    for (unsigned i = 0; i < at; ++i) o[3 + i] = (unsigned char)line[i];
    send(sel, 6);
    send(o, 3 + at);
    at = 0;
}
static char printed[40];
static unsigned printed_n;
static void collect(unsigned char c) { if (printed_n < sizeof printed && c >= 32 && c < 127) printed[printed_n++] = (char)c; }
static void workspace_eval(unsigned what, unsigned lo, unsigned hi) {
    if (!read_range(W_TEXT, lo, hi)) { put_s(" error: selection longer than 255 bytes"); insert_after(hi); return; }
    fcell depth = fvm_depth;
    void (*old)(unsigned char) = 0;
    printed_n = 0;
    if (what == 3) old = platform_console_divert(collect);   /* print it: output into the text */
    int ok = forth_eval(scratch);
    if (what == 3) platform_console_divert(old);
    if (!ok) { put_s(" error: "); put_s(forth_last_error()); insert_after(hi); return; }
    if (what == 3) {
        put_c(' ');
        for (unsigned i = 0; i < printed_n; ++i) put_c(printed[i]);
        if (!printed_n) for (fcell i = depth; i < fvm_depth; ++i) { put_u(fvm_ds[i]); if (i + 1 < fvm_depth) put_c(' '); }
        insert_after(hi);
    }
    if (what == 4) {                                       /* inspect it: the stack in the Inspector */
        inspector_tab = 0;
        if (open_tools & (1u << INSPECTOR)) { wvalue(I_TABS, 0); inspector_fill(); focus(INSPECTOR); }
        else vios_tools_open(INSPECTOR);
    }
}

static void command_event(unsigned cmd, unsigned tool) {
    struct forth_module_info m;
    switch (cmd) {
    case C_BROWSER: case C_WORKSPACE: case C_INSPECTOR: case C_MANAGER: vios_tools_open(cmd - C_BROWSER); return;
    case C_ACCEPT: if (tool == BROWSER) browser_accept(); return;
    case C_REVERT: if (tool == BROWSER) browser_module(); return;
    case C_REFRESH:
        if (tool == INSPECTOR) inspector_fill();
        else if (tool == MANAGER) manager_fill();
        else if (tool == BROWSER) { browser_modules(); browser_module(); }
        return;
    case C_SAVE:
        if (tool == BROWSER && code_module[0]) status(B_STATUS, code_module, save_module(code_module) ? ": saved to SD" : ": not saved (accept first)");
        if (tool == MANAGER && manager_tab == 1) ask("Save this module to SD?", C_SAVE, table_row[1]);
        return;
    case C_ROLLBACK: if (tool == MANAGER && manager_tab == 1 && forth_module_info(table_row[1], &m)) ask("Roll back this module?", C_ROLLBACK, table_row[1]); return;
    case C_KILL: if (tool == INSPECTOR && inspector_tab == 2) ask("Kill the selected task?", C_KILL, (unsigned)task_at(table_row[0])); return;
    case C_GOTO:
        if (tool == INSPECTOR && fvm_depth) { memory_base = fvm_ds[fvm_depth - 1]; inspector_tab = 1; wvalue(I_TABS, 1); inspector_fill(); }
        return;
    }
}

static void handle(const unsigned char *e) {
    unsigned kind = e[1], id = e[2], arg = (unsigned)(e[3] | e[4] << 8);
    if (kind == EV_COMMAND) { command_event(arg, tool_of((unsigned char)id)); return; }
    if (kind == EV_WIN) { if (e[3] == 2 && tool_of((unsigned char)id) != 255) close_tool(tool_of((unsigned char)id)); return; }
    switch (id) {
    case B_MODULES: if (kind == EV_SELECT) { module_row = arg; browser_module(); } break;
    case B_WORDS: if (kind == EV_SELECT) { word_row = arg; browser_word(); } break;
    case B_CODE: if (kind == EV_TEXT && e[3] == 1) browser_accept(); break;
    case W_TEXT: if (kind == EV_TEXT && e[3] >= 2) workspace_eval(e[3], arg = (unsigned)(e[4] | e[5] << 8), (unsigned)(e[6] | e[7] << 8)); break;
    case I_TABS: if (kind == EV_SELECT) { inspector_tab = (unsigned char)arg; inspector_fill(); } break;
    case M_TABS: if (kind == EV_SELECT) { manager_tab = (unsigned char)arg; manager_fill(); } break;
    case I_TABLE:
        if (kind == EV_SELECT) inspector_select(arg);
        if (kind == EV_PAGE && inspector_tab == 1) {
            int dir = (signed char)e[3];
            memory_base = (memory_base + (fcell)(dir == 1 || dir == -1 ? dir * 8 : dir * 64)) & FVM_MASK;
            inspector_fill();
            wvalue(I_TABLE, dir > 0 ? 15 : 0);
        }
        if (kind == EV_ACTIVATE && inspector_tab == 2) command_event(C_KILL, INSPECTOR);
        break;
    case M_TABLE:
        if (kind == EV_SELECT) table_row[1] = arg;
        if (kind == EV_ACTIVATE && manager_tab == 0) ask(BOOT_ROWS[table_row[1] < 3 ? table_row[1] : 0], A_TRIAL + (table_row[1] < 3 ? table_row[1] : 0), 0);
        if (kind == EV_ACTIVATE && manager_tab == 1) command_event(C_SAVE, MANAGER);
        break;
    case D_YES: if (kind == EV_ACTIVATE) { close_tool(DIALOG); run(pending, pending_arg); } break;
    case D_NO: if (kind == EV_ACTIVATE) close_tool(DIALOG); break;
    }
}

/* ------------------------------------------------------------ setup and idle */

static void setup(void) {
    vios_ui_begin();
    for (unsigned i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; ++i) {
        unsigned id = CMD + COMMANDS[i].id, n = len(COMMANDS[i].label);
        unsigned char o[8 + 23] = {0x60, (unsigned char)id, (unsigned char)(id >> 8), 2, 0, COMMANDS[i].mods, (unsigned char)COMMANDS[i].key, (unsigned char)n};
        for (unsigned k = 0; k < n; ++k) o[8 + k] = (unsigned char)COMMANDS[i].label[k];
        send(o, 8 + n);
    }
    send_table(MENUS);
    vios_ui_commit();
    ready = first_poll = 1;
}

void vios_tools_idle(void) {
    if (!vios_text_owns_display()) return;
    unsigned e = platform_vdp_epoch();
    if (!ready || e != epoch) { epoch = e; open_tools = 0; setup(); }
    unsigned short now = platform_timer_read();
    since_poll += (unsigned short)(last_tick - now);       /* the timer counts down */
    last_tick = now;
    if (!platform_vdp_nudged() && !first_poll && !queued && !(open_tools && since_poll >= PLATFORM_TIMER_HZ)) return;
    since_poll = 0;
    first_poll = 0;
    if (!vios_ui_poll() && !queued) return;
    for (unsigned i = 0; i < queued; ++i) handle(queue[i]);   /* handlers may poll again: they only append */
    queued = 0;
}

static int tool_word(void) {                            /* TOOL ( n -- ) open or focus tool n */
    fcell n = fvm_pop();
    if (fvm_error) return 0;
    if (!ready) vios_tools_idle();
    return n < TOOLS && vios_tools_open((unsigned)n) ? 1 : forth_error("TOOL: 0 Browser, 1 Workspace, 2 Inspector, 3 Manager (GUI only)");
}

int vios_tools_init(void) {
    open_tools = ready = inspector_tab = manager_tab = queued = 0;
    module_row = word_row = 0;
    memory_base = 0;
    vios_ui_claim(claim);
    return forth_register_native("TOOL", tool_word, 0);
}
