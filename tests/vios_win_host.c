/* Host test of /dev/win and /dev/ui/tree (kernel/vios_win.c through the
 * handles of kernel/vios_handles.c) against the real ESP display server
 * (vds_proto, vds_ui, vds) in process: the files make, list, change and
 * stream events of windows and widgets whose state only the server holds,
 * and the tree source rebuilds the same server state after a reset.
 * Usage: vios_win_host FONT.bin */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "forth/system.h"
#include "vios/runtime.h"
#include "vios/ui.h"
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
static unsigned requests;
static void esp_send(void *ctx, uint8_t code, const uint8_t *d, uint8_t n) {
    (void)ctx;
    assert(code == VDS_PACKET_REPLY && n <= 30);
    memcpy(reply, d, n); reply_n = n; reply_valid = 1;
}

/* CPU platform stubs: VDU bytes parsed for 23,0,E4,len,payload. */
static uint8_t vdu[300];
static unsigned vdu_n;
void platform_vdp_send(const void *data, size_t length) {
    const uint8_t *b = data;
    for (size_t i = 0; i < length; ++i) {
        vdu[vdu_n++] = b[i];
        if (vdu_n >= 4 && vdu[0] == 23 && vdu[1] == 0 && vdu[2] == 0xE4 && vdu_n == 4u + vdu[3]) {
            assert(vds_proto_batch(&p, vdu + 4, vdu[3]));
            vdu_n = 0;
        }
        assert(vdu_n < sizeof vdu && (vdu_n < 3 || (vdu[0] == 23 && vdu[1] == 0 && vdu[2] == 0xE4)));
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
int platform_host_request(const void *r, size_t n, void *reply_, size_t capacity, unsigned *actual) { (void)r; (void)n; (void)reply_; (void)capacity; *actual = 0; return 0; }
static char console[8192];
static unsigned console_n;
void platform_console_putc(unsigned char c) { if (console_n + 1 < sizeof console) console[console_n++] = (char)c; console[console_n] = 0; }
int platform_console_getc(void) { return -1; }
void platform_console_flush(void) {}
const char *storage_error(void) { return "no storage"; }
int storage_list(const char *path, void (*each)(const char *, unsigned long, int, void *), void *ctx) { (void)path; (void)each; (void)ctx; return 0; }
int storage_stream_open(unsigned slot, const char *path, int write) { (void)slot; (void)path; (void)write; return 0; }
int storage_stream_read(unsigned slot, void *b, size_t size, size_t *got) { (void)slot; (void)b; (void)size; *got = 0; return 0; }
int storage_stream_write(unsigned slot, const void *b, size_t size, size_t *put) { (void)slot; (void)b; (void)size; *put = 0; return 0; }
int storage_stream_close(unsigned slot) { (void)slot; return 1; }
/* The few vios_runtime.c helpers the handles use. */
void vios_out(const char *t) { while (*t) platform_console_putc((unsigned char)*t++); }
void vios_uint(uint32_t n) { char b[12]; snprintf(b, sizeof b, "%u", (unsigned)n); vios_out(b); }
void vios_hex(uint32_t n, unsigned d) { char b[12]; snprintf(b, sizeof b, "%0*X", (int)d, (unsigned)n); vios_out(b); }
int vios_same(const char *a, const char *b) { return !strcmp(a, b); }
size_t vios_length(const char *s) { return strlen(s); }
int vios_arg(const char **rest, char *out, size_t capacity) {
    while (**rest == ' ') ++*rest;
    size_t n = 0;
    while (**rest && **rest != ' ' && n + 1 < capacity) out[n++] = *(*rest)++;
    out[n] = 0;
    return n > 0;
}

static void eval(const char *s) {
    if (!forth_eval(s)) { fprintf(stderr, "eval failed: %s: %s\n", s, forth_last_error()); abort(); }
}
static fcell value(const char *s) { eval(s); return fvm_pop(); }
static int32_t signed_value(const char *s) { fcell x = value(s) & FVM_MASK; return x & 0x800000 ? (int32_t)x - 0x1000000 : (int32_t)x; }
static char got[2048];
/* A whole file through OPEN/READ/CLOSE, as text. */
static const char *slurp(const char *path) {
    char cmd[160];
    snprintf(cmd, sizeof cmd, "S\" %s\" SLURP", path);
    fcell n = value(cmd);
    assert(n < sizeof got);
    for (fcell i = 0; i < n; ++i) got[i] = (char)fvm_read8(value("BUF") + i);
    got[n] = 0;
    return got;
}
/* One WRITE of text to path (\n as newline): the WRITE result. */
static int32_t spit(const char *path, const char *text) {
    char cmd[400];
    size_t n = strlen(text);
    fcell buf = value("BUF");
    for (size_t i = 0; i < n; ++i) fvm_write8(buf + i, (unsigned char)text[i]);
    snprintf(cmd, sizeof cmd, "BUF %u S\" %s\" SPIT", (unsigned)n, path);
    return signed_value(cmd);
}
static void same(const char *a, const char *b) { if (strcmp(a, b)) { fprintf(stderr, "got:\n%s\nexpected:\n%s\n", a, b); abort(); } }
static void drain(void) { for (;;) { eval("UI-NEXT"); if (!fvm_pop()) return; fvm_pop(); fvm_pop(); fvm_pop(); } }
static void event(uint8_t kind, uint8_t id, const uint8_t *args, uint8_t n) { vds_proto_event(&p, kind, id, args, n); assert(vios_ui_poll()); }

/* The server's node state, to compare before and after a rebuild. */
static void state(char *out, size_t capacity) {
    size_t n = 0;
    for (unsigned w = 0; w < VDS_PROTO_WINDOWS; ++w) {
        const vds_window *x = &v.windows[w];
        if (x->used) n += (size_t)snprintf(out + n, capacity - n, "w%u %u %d %d %u %u %u %s\n", w, x->surface, x->x, x->y, x->cols, x->rows, x->flags & ~8, x->title);
    }
    for (unsigned i = 0; i < VDS_MAX_WIDGETS; ++i) {
        const vds_widget *x = &u.widgets[i];
        if (!x->used) continue;
        n += (size_t)snprintf(out + n, capacity - n, "%u %u %u %u %u %u %u %u %u %d %u %u %u %u [%s] [%s]", i, x->window, x->kind, x->parent, x->col, x->row,
                              x->w, x->h, x->flags, x->value, x->layout, x->gap, x->fixed, x->weight, x->text, x->name);
        for (unsigned r = 0; r < x->count; ++r) n += (size_t)snprintf(out + n, capacity - n, " <%s>", u.items[x->first + r]);
        n += (size_t)snprintf(out + n, capacity - n, "\n");
    }
    assert(n < capacity);
}

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
    assert(vios_ui_init() && vios_handles_init());
    eval("CREATE BUF 1024 ALLOT  VARIABLE HV");
    eval(": SLURP ( addr u -- n ) 1 OPEN DUP 0 < IF EXIT THEN >R BUF 1024 R@ READ R> CLOSE DROP ;");
    eval(": SPIT ( addr u paddr pu -- r ) 2 OPEN DUP 0 < IF >R 2DROP R> EXIT THEN >R R@ WRITE R> CLOSE DROP ;");

    /* Nothing yet: an empty list; no such nodes; nothing made. */
    same(slurp("/dev/win"), "");
    assert(signed_value("S\" /dev/win/3/ctl\" 1 OPEN") == -5 && signed_value("S\" /dev/win/w30/ctl\" 1 OPEN") == -5);
    assert(signed_value("S\" /dev/win/0/bogus\" 1 OPEN") == -5 && signed_value("S\" /dev/win\" 2 OPEN") == -7);
    assert(signed_value("S\" /dev/win/new\" SLURP") == -6);

    /* /dev/win/new makes windows and widgets the server holds. */
    assert(spit("/dev/win/new", "window - VIOS greeter") == 21);
    same(slurp("/dev/win/new"), "w0\n");
    assert(v.windows[0].used && v.windows[0].surface == 0 && !strcmp(v.windows[0].title, "VIOS greeter") && v.surfaces[0].cols == 32);
    assert(spit("/dev/win/new", "button w0 hello\nlabel w0 msg\nlist w0 words\nbox w0 pane\n") > 0);
    same(slurp("/dev/win/new"), "3\n");
    assert(spit("/dev/win/new", "check 3 opt") > 0 && spit("/dev/win/new", "text w0 notes") > 0);
    /* A handle used both ways reads back the id it made. */
    eval("S\" /dev/win/new\" 2 OPEN HV !  S\" tabs w0 one|two\" HV @ WRITE DROP  BUF 8 HV @ READ HV @ CLOSE DROP");
    assert(fvm_pop() == 2 && fvm_read8(value("BUF")) == '6');
    assert(u.widgets[0].kind == VDS_WK_BUTTON && !strcmp(u.widgets[0].text, "hello") && !strcmp(u.widgets[0].name, "hello"));
    assert(u.widgets[4].parent == 3 && u.widgets[4].window == 0 && u.widgets[5].kind == VDS_WK_TEXT);
    assert(spit("/dev/win/new", "check 0 x") < 0 && spit("/dev/win/new", "check w9 x") < 0 && spit("/dev/win/new", "gizmo w0 x") < 0);
    assert(spit("/dev/win/new", "window w0 x") < 0 && spit("/dev/win/new", "button") < 0);
    assert(!p.errors);

    /* The list: windows, then widgets in creation order (parents first). */
    same(slurp("/dev/win"),
         "w0 window - VIOS_greeter 64 64 32 12\n"
         "0 button w0 hello 0 0 9 1\n1 label w0 msg 0 0 8 1\n2 list w0 words 0 0 20 4\n3 box w0 pane 0 0 20 4\n"
         "4 check 3 opt 0 0 8 1\n5 text w0 notes 0 0 20 4\n6 tabs w0 one|two 0 0 11 1\n");

    /* ctl: commands in, properties out. */
    assert(spit("/dev/win/2/ctl", "rect 2 5 20 4\nvalue 0\n") > 0 && spit("/dev/win/0/ctl", "rect 2 1 10 1\ntext Say &hello\n") > 0);
    assert(spit("/dev/win/3/ctl", "rect 0 9 30 3\nlayout 2 1\nsize 3 2\nname Pane box") > 0);
    assert(spit("/dev/win/w0/ctl", "rect 80 48 40 16\nname VIOS greeter 2") > 0);
    assert(v.windows[0].x == 80 && v.windows[0].cols == 40 && v.surfaces[0].cols >= 40 && v.surfaces[0].rows >= 16);
    same(slurp("/dev/win/0/ctl"),
         "kind button\nparent w0\nname hello\nrect 2 1 10 1\nshow 1\nflags 0\nvalue 0\ncount 0\nlayout 0 0\nsize 0 1\nfocus 1\ntext Say &hello\n");
    same(slurp("/dev/win/w0/ctl"), "kind window\nname VIOS greeter 2\nrect 80 48 40 16\nshow 1\nflags 7\nsurface 0 40 16\nfocus 0\n");
    same(slurp("/dev/win/3/ctl"),
         "kind box\nparent w0\nname Pane box\nrect 0 9 30 3\nshow 1\nflags 0\nvalue 0\ncount 0\nlayout 2 1\nsize 3 2\nfocus 0\ntext \n");
    assert(spit("/dev/win/1/ctl", "show 0") > 0 && (u.widgets[1].flags & VDS_WF_HIDDEN));
    assert(spit("/dev/win/1/ctl", "show 1") > 0 && !(u.widgets[1].flags & VDS_WF_HIDDEN) && spit("/dev/win/1/ctl", "focus") < 0);
    assert(spit("/dev/win/4/ctl", "focus") > 0 && u.widget_focus[0] == 4);
    assert(spit("/dev/win/0/ctl", "bogus 1") < 0 && spit("/dev/win/0/ctl", "rect 1 2") < 0 && spit("/dev/win/0/ctl", "raise") < 0);
    assert(spit("/dev/win/w0/ctl", "value 3") < 0 && spit("/dev/win/0/ctl", "style 2") < 0 && spit("/dev/win/1/ctl", "layout 1 0") < 0);
    assert(spit("/dev/win/w0/ctl", "show 0") > 0 && !(v.windows[0].flags & 1) && spit("/dev/win/w0/ctl", "show 1\nraise\nfocus") > 0);
    assert(spit("/dev/win/w0/ctl", "style 1") > 0 && v.style.kind == VDS_STYLE_TURBO && spit("/dev/win/w0/ctl", "style 0") > 0);
    assert(!p.errors);

    /* text: the content; a handle's first write replaces, later ones append. */
    eval("S\" /dev/win/1/text\" 2 OPEN HV !  S\" Hello\" HV @ WRITE DROP  S\"  world\" HV @ WRITE DROP  HV @ CLOSE DROP");
    assert(!strcmp(u.widgets[1].text, "Hello world"));
    same(slurp("/dev/win/1/text"), "Hello world\n");
    assert(spit("/dev/win/1/text", "Bye\n") == 4 && !strcmp(u.widgets[1].text, "Bye"));
    assert(spit("/dev/win/2/text", "one\ntwo\nthe third row, thirty-one chars\n") > 0 && u.widgets[2].count == 3);
    same(slurp("/dev/win/2/text"), "one\ntwo\nthe third row, thirty-one chars\n");
    eval("S\" /dev/win/2/text\" 2 OPEN HV !  S\" a\" HV @ WRITE DROP  S\" b\" HV @ WRITE DROP  HV @ CLOSE DROP");
    same(slurp("/dev/win/2/text"), "a\nb\n");
    assert(spit("/dev/win/w0/text", "Greeter") > 0 && !strcmp(v.windows[0].title, "Greeter"));
    same(slurp("/dev/win/w0/text"), "Greeter\n");
    char doc[700];                                           /* a TextView, longer than a line or a reply */
    for (unsigned i = 0; i < sizeof doc - 1; ++i) doc[i] = i % 50 == 49 ? '\n' : (char)('a' + i % 26);
    doc[sizeof doc - 1] = 0;
    assert(spit("/dev/win/5/text", doc) == (int32_t)strlen(doc));
    same(slurp("/dev/win/5/text"), doc);
    eval("S\" /dev/win/5/text\" 1 OPEN HV !  BUF 7 HV @ READ BUF 7 + 1000 HV @ READ + HV @ CLOSE DROP");   /* short reads resume */
    assert(fvm_pop() == strlen(doc));
    for (unsigned i = 0; i < strlen(doc); ++i) assert(fvm_read8(value("BUF") + i) == (unsigned char)doc[i]);
    assert(!p.errors);

    /* event: a node's events become lines on its stream, not UI-NEXT events. */
    drain();
    eval("S\" /dev/win/0/event\" 1 OPEN HV !");
    assert(signed_value("BUF 64 HV @ READ") == -1);                       /* nothing yet: E_AGAIN */
    uint8_t w0 = 0, sel[3] = {7, 0, 0}, chg[3] = {1, 0, 0}, page[2] = {0xFF, 0};
    event(VDS_EVENT_ACTIVATE, 0, &w0, 1);
    event(VDS_EVENT_ACTIVATE, 1, &w0, 1);                                  /* label 1 has no stream */
    eval("UI-NEXT"); assert(fvm_pop() == FVM_MASK && fvm_pop() == 0 && fvm_pop() == 1 && fvm_pop() == VDS_EVENT_ACTIVATE);
    event(VDS_EVENT_CHANGE, 0, chg, 3);
    vds_ui_widget_focus(&u, 0);
    vds_ui_key(&u, 13, 0, VDS_VK_RETURN, 1); assert(vios_ui_poll());         /* a real Enter on the button */
    eval("BUF 64 HV @ READ"); { fcell n = fvm_pop(); char s[65] = {0}; for (fcell i = 0; i < n; ++i) s[i] = (char)fvm_read8(value("BUF") + i); same(s, "activate\nchange 1\nactivate\n"); }
    eval("S\" /dev/win/w0/event\" 1 OPEN  S\" /dev/win/2/event\" 1 OPEN");
    fcell list_events = fvm_pop(), window_events = fvm_pop();
    event(VDS_EVENT_SELECT, 2, sel, 3);
    event(VDS_EVENT_PAGE, 2, page, 2);
    uint8_t moved[7] = {1, 80, 0, 48, 0, 40, 16}, close_[7] = {2, 80, 0, 48, 0, 40, 16}, cmd[3] = {12, 0, 1};
    event(VDS_EVENT_WIN, 0, moved, 7);
    event(VDS_EVENT_WIN, 0, close_, 7);
    event(VDS_EVENT_COMMAND, 0, cmd, 3);
    char line[200];
    snprintf(line, sizeof line, "BUF 64 %u READ", (unsigned)list_events); eval(line);
    { fcell n = fvm_pop(); char s[65] = {0}; for (fcell i = 0; i < n; ++i) s[i] = (char)fvm_read8(value("BUF") + i); same(s, "select 7\npage -1\n"); }
    snprintf(line, sizeof line, "BUF 200 %u READ", (unsigned)window_events); eval(line);
    { fcell n = fvm_pop(); char s[201] = {0}; for (fcell i = 0; i < n; ++i) s[i] = (char)fvm_read8(value("BUF") + i); same(s, "moved 80 48 40 16\nclose\ncommand 12 1\n"); }
    snprintf(line, sizeof line, "%u CLOSE DROP %u CLOSE DROP HV @ CLOSE DROP", (unsigned)list_events, (unsigned)window_events); eval(line);
    event(VDS_EVENT_ACTIVATE, 0, &w0, 1);                                  /* closed: back to UI-NEXT */
    eval("UI-NEXT"); assert(fvm_pop() == FVM_MASK && fvm_pop() == 0 && fvm_pop() == 0 && fvm_pop() == VDS_EVENT_ACTIVATE);
    /* A background task waits on a stream with READ-WAIT and reads the list. */
    eval("VARIABLE TN  CREATE TB 64 ALLOT  VARIABLE TL "
         ": WAITER S\" /dev/win/0/event\" 1 OPEN >R TB 64 R@ READ-WAIT TN ! S\" /dev/win\" 1 OPEN >R TB 64 R@ READ TL ! R> CLOSE DROP R> CLOSE DROP ; "
         "' WAITER SPAWN DROP");
    for (int i = 0; i < 50; ++i) forth_tasks_run();
    assert(value("TN @") == 0);
    event(VDS_EVENT_ACTIVATE, 0, &w0, 1);
    for (int i = 0; i < 50; ++i) forth_tasks_run();
    assert(value("TN @") == 9 && value("TL @") == 64 && fvm_read8(value("TB")) == 'w');   /* "activate\n", then the list */
    assert(signed_value("S\" /dev/win/0/event\" 1 OPEN DUP CLOSE DROP") > 0);    /* the task's handles were released */

    /* /dev/ui/tree (and UI-SNAPSHOT) is source that rebuilds the same state. */
    assert(spit("/dev/win/4/ctl", "value 1") > 0 && spit("/dev/win/new", "progress w0 bar") > 0 && spit("/dev/win/7/ctl", "value 40") > 0);
    char before[4096], after[4096], tree[2048];
    state(before, sizeof before);
    strcpy(tree, slurp("/dev/ui/tree"));
    assert(strstr(tree, "0 32 12 48 UI-SURFACE") || strstr(tree, "0 40 16 48 UI-SURFACE"));
    assert(strstr(tree, "UI-WINDOW S\" Greeter\" 0 UI-TITLE\n") && strstr(tree, "S\" one\"") == 0 && strstr(tree, " 2 2 UI-ITEMS\nS\" a\" 2 0 UI-ITEM\nS\" b\" 2 1 UI-ITEM\n"));
    assert(strstr(tree, "3 2 1 UI-LAYOUT 3 3 2 UI-WSIZE\n") && strstr(tree, "\\ TextView: "));
    console_n = 0;
    eval("UI-SNAPSHOT");
    same(console, tree);
    eval("UI-RESET");
    assert(!v.windows[0].used && !u.widgets[0].used);
    for (char *at = tree, *nl; (nl = strchr(at, '\n')); at = nl + 1) { *nl = 0; eval(at); }
    state(after, sizeof after);
    same(after, before);
    assert(!p.errors);

    /* delete and close through ctl; HANDLES shows the files. */
    assert(spit("/dev/win/3/ctl", "delete") > 0 && !u.widgets[3].used && !u.widgets[4].used);
    assert(strstr(slurp("/dev/win"), "4 check") == 0);
    eval("S\" /dev/win/w0/ctl\" 1 OPEN HV !");
    console_n = 0; vios_handles_report(); assert(strstr(console, "kind win read") && strstr(console, "path /dev/win/w0/ctl"));
    eval("HV @ CLOSE DROP");
    assert(spit("/dev/win/w0/ctl", "close") > 0 && !v.windows[0].used && v.surfaces[0].cells);    /* UI-SURFACE made that one */
    for (int i = 0; i < 12; ++i) {                          /* a surface /dev/win/new made is freed with its window */
        assert(spit("/dev/win/new", "window - again") > 0 && v.windows[0].used && v.windows[0].surface == 1);
        assert(spit("/dev/win/w0/ctl", "close") > 0 && !v.surfaces[1].cells);
    }
    same(slurp("/dev/win"), "");
    assert(!p.errors && !fvm_depth);
    printf("vios win checks passed: %u requests\n", requests);
    return 0;
}
