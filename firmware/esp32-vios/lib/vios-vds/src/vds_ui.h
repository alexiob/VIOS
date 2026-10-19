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
 *   11 COMMAND  id=focused window  args: command(u16) source (1 menu, 2 accelerator, 3 palette) */
#ifndef VIOS_VDS_UI_H
#define VIOS_VDS_UI_H
#include "vds_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VDS_VK_ESCAPE = 125, VDS_VK_BACKSPACE = 132, VDS_VK_TAB = 142, VDS_VK_RETURN = 143, VDS_VK_KP_ENTER = 144,
    VDS_VK_UP = 150, VDS_VK_KP_UP = 151, VDS_VK_DOWN = 152, VDS_VK_KP_DOWN = 153,
    VDS_VK_LEFT = 154, VDS_VK_KP_LEFT = 155, VDS_VK_RIGHT = 156, VDS_VK_KP_RIGHT = 157,
    VDS_VK_F1 = 159, VDS_VK_F3 = 161, VDS_VK_F5 = 163, VDS_VK_F6 = 164, VDS_VK_F10 = 168, VDS_VK_F12 = 170
};
enum { VDS_MOD_CTRL = 1, VDS_MOD_SHIFT = 2, VDS_MOD_LALT = 4, VDS_MOD_RALT = 8 };
enum { VDS_EVENT_FOCUS = 0x06, VDS_EVENT_WIN = 0x10, VDS_EVENT_COMMAND = 0x11 };
enum { VDS_WIN_MOVED = 1, VDS_WIN_CLOSE = 2, VDS_WIN_ZOOMED = 3, VDS_WIN_RESTORED = 4 };
enum { VDS_FROM_MENU = 1, VDS_FROM_ACCELERATOR = 2, VDS_FROM_PALETTE = 3 };

#define VDS_MAX_COMMANDS 64
#define VDS_MAX_MENUS 16
#define VDS_MENU_ITEMS 16
#define VDS_LABEL_MAX 24
#define VDS_MENU_DEPTH 3
#define VDS_POPUP_COLS 28
#define VDS_FILTER_MAX 15
#define VDS_ITEM_SEPARATOR 0xFFFFu
#define VDS_ITEM_SUBMENU 0xFE00u
#define VDS_UI_FIRST_WINDOW VDS_PROTO_WINDOWS     /* popups 27..29, status line 30, menu bar 31 */
#define VDS_UI_FIRST_SURFACE VDS_PROTO_SURFACES   /* popups 11..13, status line 14, menu bar 15 */

enum { VDS_CMD_USED = 1, VDS_CMD_ENABLED = 2, VDS_CMD_CHECKED = 4, VDS_CMD_STATUS = 8 };

typedef struct { int16_t x, y; uint8_t cols, rows; } vds_geometry;
typedef struct { uint16_t id; uint8_t flags, vk, mods, ascii; char label[VDS_LABEL_MAX]; } vds_command;
typedef struct { uint8_t used, count; uint16_t items[VDS_MENU_ITEMS]; char title[16]; } vds_menu;
typedef struct { uint8_t menu, sel, count; uint16_t items[VDS_MENU_ITEMS]; } vds_open_menu;

typedef struct vds_ui {
    vds_proto *p;
    uint8_t focus;                          /* window id or VDS_NONE */
    uint8_t wm_mode;                        /* keyboard move/resize in progress */
    uint8_t held, held_vk, held_ascii;      /* key whose press an intrinsic consumed */
    vds_geometry wm_saved;
    uint32_t zoomed;                        /* bit per window id */
    vds_geometry zoom_saved[VDS_MAX_WINDOWS];
    vds_command commands[VDS_MAX_COMMANDS];
    vds_menu menus[VDS_MAX_MENUS];
    uint8_t window_menu[VDS_MAX_WINDOWS];   /* pop-up menu per window, VDS_NONE: world menu */
    vds_open_menu open[VDS_MENU_DEPTH];
    uint8_t depth, bar_index, palette, filter_len;  /* bar_index: VDS_NONE unless a bar pulldown is open */
    char filter[VDS_FILTER_MAX + 1];
    uint8_t bar_shown, status_shown, style_seen;
    vds_cell popup_cells[VDS_MENU_DEPTH][VDS_MENU_ITEMS * VDS_POPUP_COLS];
    vds_cell bar_cells[80], status_cells[80];
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
/* Re-render the menu bar, status line and open menus (after style changes). */
void vds_ui_refresh(vds_ui *u);
/* Text shown for a command's accelerator, e.g. "Ctrl+S" (out: 20 bytes). */
void vds_ui_accel_text(const vds_command *c, char *out);

#ifdef __cplusplus
}
#endif
#endif
