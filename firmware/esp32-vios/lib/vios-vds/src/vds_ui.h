/* VIOS Display Server intrinsics (GUI windows G2/G3): window focus, keyboard
 * move/resize, zoom, close requests, commands, menus, accelerators and the
 * command palette, handled on the ESP so they react within a frame whatever
 * the CPU is doing. Only semantic events reach the CPU; keys no intrinsic
 * uses are left for the existing 0x81 path.
 *
 * Keys are the pinned VDP 1.04 / FabGL virtual keys and modifier bits
 * (include/platform/input.h). Bindings (section 8 of the windows design), in
 * routing order:
 *   open menu / palette  arrows, Enter, Esc, mnemonic letters (palette: typing filters)
 *   move/resize mode     arrows move (8 px), Shift+arrows resize a cell, Enter keeps, Esc reverts
 *   F10                  Turbo Vision: first menu-bar pulldown; Smalltalk: world menu (menu 0)
 *   Alt+letter           Turbo Vision: the menu-bar menu with that mnemonic
 *   Shift+F10            pop-up menu of the focused window (else the world menu)
 *   Ctrl+Shift+P, Ctrl+K command palette (subsequence filter over enabled commands)
 *   accelerators         as defined by the CPU's commands
 *   F6 / Shift+F6        next / previous window
 *   Ctrl+F5              start move/resize mode
 *   F5                   zoom / restore the focused window
 *   Alt+F3, Ctrl+W       close request (the CPU decides)
 *   Ctrl+F12             keyboard pointer: arrows move (Shift: 1 px), Space/Enter click, Esc leaves
 * A visible modal window keeps focus to itself.
 *
 * Commands and menus come from the CPU (protocol ops 59-62). Menu 0 is the
 * menu bar (Turbo Vision, shown along the top) or the world menu (Smalltalk).
 * Menu items are command ids, VDS_ITEM_SEPARATOR, or VDS_ITEM_SUBMENU | menu.
 * Labels mark the mnemonic with '&'.
 *
 * Window ids VDS_UI_FIRST_WINDOW.. and surface ids VDS_UI_FIRST_SURFACE.. are
 * reserved for menus, the menu bar and the status line.
 *
 * Events (vds_proto_event kinds):
 *   06 FOCUS    id=new  args: previous
 *   10 WIN      id      args: what x(s16) y(s16) cols rows
 *                       what: 1 moved/resized, 2 close request, 3 zoomed, 4 restored
 *   11 COMMAND  id=focused window  args: command(u16) source (1 menu, 2 accelerator, 3 palette)
 *   12 ANIMDONE id      args: target prop
 *   07 POINTER  id=window under the pointer (VDS_NONE: desktop)
 *               args: x(s16) y(s16) col row part buttons; sent when buttons change */
#ifndef VIOS_VDS_UI_H
#define VIOS_VDS_UI_H
#include "vds_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VDS_VK_SPACE = 1, VDS_VK_DELETE = 130, VDS_VK_HOME = 133, VDS_VK_KP_HOME = 134, VDS_VK_END = 135, VDS_VK_KP_END = 136,
    VDS_VK_PAGEUP = 146, VDS_VK_KP_PAGEUP = 147, VDS_VK_PAGEDOWN = 148, VDS_VK_KP_PAGEDOWN = 149,
    VDS_VK_ESCAPE = 125, VDS_VK_BACKSPACE = 132, VDS_VK_TAB = 142, VDS_VK_RETURN = 143, VDS_VK_KP_ENTER = 144,
    VDS_VK_UP = 150, VDS_VK_KP_UP = 151, VDS_VK_DOWN = 152, VDS_VK_KP_DOWN = 153,
    VDS_VK_LEFT = 154, VDS_VK_KP_LEFT = 155, VDS_VK_RIGHT = 156, VDS_VK_KP_RIGHT = 157,
    VDS_VK_F1 = 159, VDS_VK_F3 = 161, VDS_VK_F5 = 163, VDS_VK_F6 = 164, VDS_VK_F10 = 168, VDS_VK_F12 = 170
};
enum { VDS_MOD_CTRL = 1, VDS_MOD_SHIFT = 2, VDS_MOD_LALT = 4, VDS_MOD_RALT = 8 };
enum { VDS_EVENT_POINTER = 0x07, VDS_EVENT_FOCUS = 0x06, VDS_EVENT_WIN = 0x10, VDS_EVENT_COMMAND = 0x11, VDS_EVENT_ANIMDONE = 0x12 };
enum { VDS_TWEEN_WINDOW = 0, VDS_TWEEN_SPRITE = 1 };
enum { VDS_PROP_X = 0, VDS_PROP_Y = 1, VDS_PROP_SCROLL_ROW = 2, VDS_PROP_SCROLL_COL = 3 };
enum { VDS_EASE_LINEAR = 0, VDS_EASE_IN_OUT = 1, VDS_EASE_OUT = 2, VDS_EASE_SPRING = 3 };
#define VDS_MAX_TWEENS 16

/* Widgets (G4): rendered by the ESP into their window's surface; events to
 * the CPU: 02 ACTIVATE id=widget args: window; 03 SELECT id=widget args:
 * index(u16) window; 04 CHANGE id=widget args: value(s16) window (TextView:
 * once when it becomes dirty); 05 TEXT id=TextView args: what sel_start(u16)
 * sel_end(u16) window (what: 1 accept Ctrl+S, 2 do it Ctrl+D, 3 print it
 * Ctrl+P, 4 inspect it Ctrl+I); 08 EXPAND id=tree list args: index(u16)
 * window open (Right 1, Left 0); 09 PAGE id=pager list args: dir(s8) window
 * (moving past the first/last row, PgUp/PgDn at an edge).
 *
 * Lists keep their rows in a pool shared by all lists (VDS_LIST_POOL rows of
 * VDS_ITEM_TEXT bytes). With column widths (op 93) a list is a table: '\t'
 * separates the cells of a row and of the header (the widget text). Tabs show
 * the widget text split at '|'; value is the selected tab. A TextView owns
 * one of VDS_TEXT_SLOTS buffers and is edited locally; the CPU reads it back. */
enum { VDS_EVENT_ACTIVATE = 0x02, VDS_EVENT_SELECT = 0x03, VDS_EVENT_CHANGE = 0x04, VDS_EVENT_TEXT = 0x05,
       VDS_EVENT_EXPAND = 0x08, VDS_EVENT_PAGE = 0x09 };
enum { VDS_TEXT_ACCEPT = 1, VDS_TEXT_DO = 2, VDS_TEXT_PRINT = 3, VDS_TEXT_INSPECT = 4 };
enum { VDS_WK_BOX = 1, VDS_WK_LABEL, VDS_WK_BUTTON, VDS_WK_CHECK, VDS_WK_RADIO, VDS_WK_INPUT, VDS_WK_LIST, VDS_WK_PROGRESS,
       VDS_WK_TEXT, VDS_WK_TABS };
enum { VDS_WF_HIDDEN = 1, VDS_WF_DISABLED = 2, VDS_WF_FILL = 4,   /* FILL: a root box covering the client area */
       VDS_WF_READONLY = 8, VDS_WF_TREE = 16, VDS_WF_PAGER = 32 };
enum { VDS_LAYOUT_NONE = 0, VDS_LAYOUT_COLUMN = 1, VDS_LAYOUT_ROW = 2 };
enum { VDS_KEYS_FORWARD = 0, VDS_KEYS_DROP = 1 };   /* key policy of a window (op 5A) */
#define VDS_MAX_WIDGETS 64
#define VDS_WIDGET_TEXT 32
#define VDS_LIST_POOL 1024              /* list rows held on the ESP, shared by all lists */
#define VDS_ITEM_TEXT 32
#define VDS_TABLE_COLS 8
#define VDS_TEXT_SLOTS 4
#define VDS_TEXT_MAX 4096               /* the module source limit */
#define VDS_CLIP_MAX 1024
typedef struct {
    uint8_t used, window, kind, parent, flags, layout, gap, fixed, weight;
    uint16_t seq;                       /* creation order: layout and Tab follow it */
    uint8_t col, row, w, h;             /* placement in window client cells (computed under a box) */
    int16_t value;                      /* check/radio 0/1, progress 0..100, list selection, input caret, tab */
    uint16_t top;                       /* first visible list row / input scroll */
    uint16_t count, first;              /* list: row count and first pool slot */
    uint8_t slot;                       /* TextView buffer, VDS_NONE if none */
    uint8_t ncols, cols[VDS_TABLE_COLS];   /* table column widths */
    char text[VDS_WIDGET_TEXT];
} vds_widget;
typedef struct {                        /* a TextView buffer */
    uint8_t used, widget, dirty;
    uint16_t len, caret, anchor, top, left, rev;   /* top: offset of the first visible line */
    char buf[VDS_TEXT_MAX];
} vds_text;
enum { VDS_WIN_MOVED = 1, VDS_WIN_CLOSE = 2, VDS_WIN_ZOOMED = 3, VDS_WIN_RESTORED = 4 };
enum { VDS_FROM_MENU = 1, VDS_FROM_ACCELERATOR = 2, VDS_FROM_PALETTE = 3 };

#define VDS_MAX_COMMANDS 64
#define VDS_MAX_MENUS 16
#define VDS_MENU_ITEMS 16
#define VDS_LABEL_MAX 24
#define VDS_MENU_DEPTH 3
#define VDS_POPUP_COLS 28
#define VDS_FILTER_MAX 15
#define VDS_ACCEL_MAX 24                /* accelerator text buffer incl. NUL */
#define VDS_ITEM_SEPARATOR 0xFFFFu
#define VDS_ITEM_SUBMENU 0xFE00u
#define VDS_UI_FIRST_WINDOW VDS_PROTO_WINDOWS     /* popups 27..29, status line 30, menu bar 31 */
#define VDS_UI_FIRST_SURFACE VDS_PROTO_SURFACES   /* popups 11..13, status line 14, menu bar 15 */

enum { VDS_CMD_USED = 1, VDS_CMD_ENABLED = 2, VDS_CMD_CHECKED = 4, VDS_CMD_STATUS = 8 };

typedef struct { int16_t x, y; uint8_t cols, rows; } vds_geometry;
typedef struct { uint16_t id; uint8_t flags, vk, mods, ascii; char label[VDS_LABEL_MAX]; } vds_command;
typedef struct { uint8_t used, count; uint16_t items[VDS_MENU_ITEMS]; char title[16]; } vds_menu;
typedef struct { uint8_t menu, sel, count; uint16_t items[VDS_MENU_ITEMS]; } vds_open_menu;
typedef struct { uint8_t used, target, id, prop, easing; int16_t from, to; uint16_t ms, elapsed; } vds_tween;

typedef struct vds_ui {
    vds_proto *p;
    uint8_t focus;                          /* window id or VDS_NONE */
    uint8_t wm_mode;                        /* keyboard move/resize in progress */
    uint8_t held, held_vk, held_ascii;      /* key whose press an intrinsic consumed */
    vds_geometry wm_saved;
    uint64_t zoomed;                        /* bit per window id */
    vds_geometry zoom_saved[VDS_MAX_WINDOWS];
    vds_command commands[VDS_MAX_COMMANDS];
    vds_menu menus[VDS_MAX_MENUS];
    uint8_t window_menu[VDS_MAX_WINDOWS];   /* pop-up menu per window, VDS_NONE: world menu */
    vds_open_menu open[VDS_MENU_DEPTH];
    uint8_t depth, bar_index, palette, filter_len;  /* bar_index: VDS_NONE unless a bar pulldown is open */
    char filter[VDS_FILTER_MAX + 1];
    uint8_t bar_shown, status_shown, style_seen;
    vds_cell popup_cells[VDS_MENU_DEPTH][(VDS_MENU_ITEMS + 1) * VDS_POPUP_COLS];   /* +1: palette filter row */
    vds_cell bar_cells[80], status_cells[80];
    vds_tween tweens[VDS_MAX_TWEENS];
    vds_widget widgets[VDS_MAX_WIDGETS];
    uint8_t widget_focus[VDS_MAX_WINDOWS];  /* focused widget per window, VDS_NONE */
    char items[VDS_LIST_POOL][VDS_ITEM_TEXT];
    uint16_t items_used;
    uint8_t caret_widget;
    vds_text texts[VDS_TEXT_SLOTS];
    char clip[VDS_CLIP_MAX];
    uint16_t clip_len;
    uint8_t key_policy[VDS_MAX_WINDOWS];
    uint16_t widget_seq;
    uint8_t widget_cols[VDS_MAX_WINDOWS], widget_rows[VDS_MAX_WINDOWS];   /* size at the last layout */
    uint8_t pointer_mode, buttons;
    int16_t px, py;
} vds_ui;

void vds_ui_init(vds_ui *u, vds_proto *p);
/* Route one physical key. Returns 1 if an intrinsic consumed it. */
int vds_ui_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk, uint8_t down);
/* Focus a window (raise, mark active, FOCUS event); VDS_NONE clears focus. */
int vds_ui_focus(vds_ui *u, uint8_t id);
/* Forget a closed window (call after vds_window_close). */
void vds_ui_forget(vds_ui *u, uint8_t id);

/* Commands and menus (also reachable through protocol ops 59-62). */
int vds_ui_command(vds_ui *u, uint16_t id, uint8_t flags, uint8_t vk, uint8_t mods, uint8_t ascii, const char *label);
int vds_ui_command_state(vds_ui *u, uint16_t id, uint8_t flags);
int vds_ui_menu(vds_ui *u, uint8_t menu, const char *title, const uint16_t *items, uint8_t count);
int vds_ui_window_menu(vds_ui *u, uint8_t window, uint8_t menu);
/* Tweens (op 70/71): ms 0 jumps at once; a new tween replaces the old one on
 * the same target, id and property. Returns 0 for an unknown target. */
int vds_ui_animate(vds_ui *u, uint8_t target, uint8_t id, uint8_t prop, int16_t from, int16_t to, uint16_t ms, uint8_t easing);
int vds_ui_stop(vds_ui *u, uint8_t target, uint8_t id, uint8_t prop);
/* Advance tweens by ms (called once per frame through the protocol hook). */
void vds_ui_tick(vds_ui *u, uint32_t ms);
/* Pointer at (x, y) with buttons (bit 0 primary): moves the cursor sprite; a
 * press focuses the window under it; button changes send POINTER events. */
void vds_ui_pointer(vds_ui *u, int16_t x, int16_t y, uint8_t buttons);
/* Widgets (ops 80-89). Placement is in client cells of `window`; children of
 * a box with a layout are placed by it in creation order (fixed cells, else a
 * share by weight). Tab moves through a window's widgets in creation order. */
int vds_ui_widget(vds_ui *u, uint8_t id, uint8_t window, uint8_t kind, uint8_t parent,
                  uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t flags);
int vds_ui_widget_text(vds_ui *u, uint8_t id, const char *text, size_t n);
int vds_ui_widget_value(vds_ui *u, uint8_t id, int16_t value);
int vds_ui_widget_layout(vds_ui *u, uint8_t id, uint8_t layout, uint8_t gap);
int vds_ui_widget_size(vds_ui *u, uint8_t id, uint8_t fixed, uint8_t weight);
int vds_ui_widget_item(vds_ui *u, uint8_t id, uint16_t index, const char *text, size_t n);
int vds_ui_widget_count(vds_ui *u, uint8_t id, uint16_t count);
/* Append a row to a list (count + 1). */
int vds_ui_widget_append(vds_ui *u, uint8_t id, const char *text, size_t n);
/* Table column widths (n <= VDS_TABLE_COLS; 0 makes it a plain list again). */
int vds_ui_widget_columns(vds_ui *u, uint8_t id, const uint8_t *widths, uint8_t n);
/* TextView: replace (append 0) or extend (append 1) the text; caret and
 * selection (anchor == caret: none); insert at the caret replacing the
 * selection, which then covers the inserted text. */
int vds_ui_text_set(vds_ui *u, uint8_t id, int append, const char *text, size_t n);
int vds_ui_text_select(vds_ui *u, uint8_t id, uint16_t caret, uint16_t anchor);
int vds_ui_text_insert(vds_ui *u, uint8_t id, const char *text, size_t n);
const vds_text *vds_ui_text(vds_ui *u, uint8_t id);
/* Key policy of a window (op 5A): VDS_KEYS_DROP keeps the keys no intrinsic
 * or widget uses from the CPU (F1 still goes through). */
int vds_ui_key_policy(vds_ui *u, uint8_t window, uint8_t policy);
int vds_ui_widget_delete(vds_ui *u, uint8_t id);
int vds_ui_widget_focus(vds_ui *u, uint8_t id);
/* Lay out and redraw every widget of a window (after resize or style change). */
void vds_ui_widgets_refresh(vds_ui *u, uint8_t window);
/* Internal to vds_ui.c: keys and clicks for widgets; 1 when consumed. */
int vds_ui_widget_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk);
int vds_ui_widget_click(vds_ui *u, uint8_t window, int col, int row);
int vds_ui_text_key(vds_ui *u, uint8_t id, uint8_t ascii, uint8_t modifiers, uint8_t vk);
void vds_ui_text_render(vds_ui *u, uint8_t id);
void vds_ui_text_release(vds_ui *u, uint8_t id);
void vds_ui_widgets_forget(vds_ui *u, uint8_t window);
/* Re-render the menu bar, status line and open menus (after style changes). */
void vds_ui_refresh(vds_ui *u);
/* Text shown for a command's accelerator, e.g. "Ctrl+S"; out holds VDS_ACCEL_MAX bytes. */
void vds_ui_accel_text(const vds_command *c, char *out);

#ifdef __cplusplus
}
#endif
#endif
