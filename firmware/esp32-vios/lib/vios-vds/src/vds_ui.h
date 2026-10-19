/* VIOS Display Server intrinsics (GUI windows G2): window focus, keyboard
 * move/resize mode, zoom and close requests, handled on the ESP so they react
 * within a frame whatever the CPU is doing. Only semantic events reach the
 * CPU; keys the router does not use are left for the existing 0x81 path.
 *
 * Keys are the pinned VDP 1.04 / FabGL virtual keys and modifier bits
 * (include/platform/input.h). Bindings (section 8 of the windows design):
 *   F6 / Shift+F6        next / previous window (raise and focus)
 *   Ctrl+F5              move/resize mode: arrows move (8 px across, 8 px down),
 *                        Shift+arrows resize by one cell, Enter keeps, Esc reverts
 *   F5                   zoom: maximise / restore the focused window
 *   Alt+F3, Ctrl+W       close request (the CPU decides)
 * A visible modal window keeps focus to itself.
 *
 * Events (vds_proto_event kinds):
 *   06 FOCUS  id=new  args: previous
 *   10 WIN    id      args: what x(s16) y(s16) cols rows
 *                     what: 1 moved/resized, 2 close request, 3 zoomed, 4 restored */
#ifndef VIOS_VDS_UI_H
#define VIOS_VDS_UI_H
#include "vds_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    VDS_VK_ESCAPE = 125, VDS_VK_TAB = 142, VDS_VK_RETURN = 143, VDS_VK_KP_ENTER = 144,
    VDS_VK_UP = 150, VDS_VK_KP_UP = 151, VDS_VK_DOWN = 152, VDS_VK_KP_DOWN = 153,
    VDS_VK_LEFT = 154, VDS_VK_KP_LEFT = 155, VDS_VK_RIGHT = 156, VDS_VK_KP_RIGHT = 157,
    VDS_VK_F1 = 159, VDS_VK_F3 = 161, VDS_VK_F5 = 163, VDS_VK_F6 = 164, VDS_VK_F10 = 168
};
enum { VDS_MOD_CTRL = 1, VDS_MOD_SHIFT = 2, VDS_MOD_LALT = 4, VDS_MOD_RALT = 8 };
enum { VDS_EVENT_FOCUS = 0x06, VDS_EVENT_WIN = 0x10 };
enum { VDS_WIN_MOVED = 1, VDS_WIN_CLOSE = 2, VDS_WIN_ZOOMED = 3, VDS_WIN_RESTORED = 4 };

typedef struct { int16_t x, y; uint8_t cols, rows; } vds_geometry;

typedef struct vds_ui {
    vds_proto *p;
    uint8_t focus;                          /* window id or VDS_NONE */
    uint8_t wm_mode;                        /* keyboard move/resize in progress */
    uint8_t held, held_vk, held_ascii;      /* key whose press an intrinsic consumed */
    vds_geometry wm_saved;
    uint32_t zoomed;                        /* bit per window id */
    vds_geometry zoom_saved[VDS_MAX_WINDOWS];
} vds_ui;

void vds_ui_init(vds_ui *u, vds_proto *p);
/* Route one physical key. Returns 1 if an intrinsic consumed it. */
int vds_ui_key(vds_ui *u, uint8_t ascii, uint8_t modifiers, uint8_t vk, uint8_t down);
/* Focus a window (raise, mark active, FOCUS event); VDS_NONE clears focus. */
int vds_ui_focus(vds_ui *u, uint8_t id);
/* Forget a closed window (call after vds_window_close). */
void vds_ui_forget(vds_ui *u, uint8_t id);

#ifdef __cplusplus
}
#endif
#endif
