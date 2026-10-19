/* VIOS Display Server core (GUI windows design, G1): overlapping windows of
 * 8x8 cell surfaces composited into a 4 bpp framebuffer.
 *
 * Portable C99: no fabgl, Arduino or heap. The adapter supplies framebuffer
 * row pointers (VGA16 layout: high nibble is the left pixel), the 8x8 font
 * and surface cell memory (PSRAM on the device). Composition is beam-ordered:
 * vds_frame() walks damaged scanlines top to bottom, composes each span into
 * an internal line buffer (desktop, then windows bottom to top, shadows as
 * shade-table remaps) and copies it to the framebuffer once, so the screen
 * never shows an intermediate state. See
 * docs/superpowers/specs/2026-10-19-vios-gui-windows.md sections 5 and 14. */
#ifndef VIOS_VDS_H
#define VIOS_VDS_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VDS_MAX_WIDTH 640
#define VDS_MAX_WINDOWS 32
#define VDS_MAX_SURFACES 16
#define VDS_MAX_DAMAGE 32
#define VDS_TITLE_MAX 32
#define VDS_LUT_SLOTS 8
#define VDS_MAX_SPRITES 16
#define VDS_SPRITE_MAX 64           /* width and height limit */
#define VDS_CURSOR_SPRITE 15        /* convention: the pointer is the topmost sprite */
#define VDS_NONE 255

typedef struct { int16_t x, y, w, h; } vds_rect;
/* attr = fg | bg << 4, palette indices. Indices 0-3 are pinned (black, red,
 * yellow, white) in every style; 4-15 belong to the style, so content meant to
 * look the same in both styles uses 0-3 or the style's role indices. */
typedef struct { uint8_t ch, attr; } vds_cell;

enum { VDS_STYLE_SMALLTALK = 0, VDS_STYLE_TURBO = 1, VDS_STYLE_COUNT };

enum {
    VDS_W_VISIBLE = 1,
    VDS_W_DECOR = 2,    /* frame and title per style */
    VDS_W_SHADOW = 4,
    VDS_W_ACTIVE = 8,
    VDS_W_FREE_X = 16,  /* never snap this window's x */
    VDS_W_MODAL = 32,   /* the topmost visible modal window dims everything below it */
    VDS_W_OVERLAY = 64, /* stays above all ordinary windows (menus, bars) */
    VDS_W_MENU = 128    /* plain 1 px frame in the style's menu colours */
};

typedef struct {
    uint8_t kind;
    uint8_t palette[16];        /* RGB222 values for the adapter; 0-3 pinned */
    uint8_t shade[16];          /* colour index -> its shadowed index */
    uint8_t desktop_a, desktop_b, body, frame, frame_inactive;
    uint8_t title_fg, title_fg_inactive, tab, tab_inactive;
    uint8_t menu_fg, menu_bg, menu_key, menu_sel_fg, menu_sel_bg, menu_disabled, menu_frame;
} vds_style;

enum { VDS_CURSOR_OFF = 0, VDS_CURSOR_BLOCK = 1, VDS_CURSOR_UNDERLINE = 2, VDS_CURSOR_BAR = 3 };

typedef struct {
    uint8_t cols, rows;
    vds_cell *cells;            /* cols * rows, owned by the adapter */
    uint8_t cur_col, cur_row, cur_shape, cur_blink;     /* text caret, blinked locally */
} vds_surface;

typedef struct {
    uint8_t used, surface, flags, cols, rows, scroll_col, scroll_row;
    int16_t x, y;               /* client-area origin in pixels */
    char title[VDS_TITLE_MAX];
} vds_window;

/* A sprite is a 4 bpp bitmap (rows of (w + 1) / 2 bytes, high nibble left)
 * drawn above all windows, higher ids on top. Colour index `transparent`
 * (or VDS_NONE) shows what is underneath. (x, y) is the hotspot position. */
typedef struct {
    uint8_t used, visible, w, h, hot_x, hot_y, transparent;
    int16_t x, y;
    const uint8_t *pixels;      /* owned by the adapter */
} vds_sprite;

typedef struct {
    uint32_t frames, rows, spans, pixels, carried, lut_misses;
} vds_stats;

typedef struct vds {
    uint8_t **rows;             /* framebuffer rows, 4 bpp */
    int16_t width, height;
    const uint8_t *font;        /* 256 glyphs x 8 bytes, MSB = left pixel */
    vds_style style;
    uint8_t snap_x;             /* 8 (default) or 1 */
    vds_surface surfaces[VDS_MAX_SURFACES];
    vds_window windows[VDS_MAX_WINDOWS];
    vds_sprite sprites[VDS_MAX_SPRITES];
    uint8_t order[VDS_MAX_WINDOWS], count;      /* z order, bottom first */
    vds_rect damage[VDS_MAX_DAMAGE];
    uint8_t ndamage;
    uint32_t line[VDS_MAX_WIDTH / 8];           /* one composed scanline */
    struct { uint8_t attr, valid; uint32_t px[256]; } lut[VDS_LUT_SLOTS];
    uint8_t lut_next;
    vds_stats stats;
    uint32_t blink_ms;          /* time since the last blink phase change */
    uint8_t blink_on;
    void (*row_written)(void *ctx, int16_t y);  /* optional probe hook */
    void *row_ctx;
} vds;

/* Setup. width must be a multiple of 8 and at most VDS_MAX_WIDTH. */
int vds_init(vds *v, uint8_t **rows, int16_t width, int16_t height, const uint8_t *font);
int vds_set_style(vds *v, uint8_t kind);                /* damages everything */
const vds_style *vds_builtin_style(uint8_t kind);
int vds_set_snap(vds *v, uint8_t snap_x);               /* 8 or 1 */

/* Surfaces: cell grids shown by any number of windows. */
int vds_surface_init(vds *v, uint8_t id, uint8_t cols, uint8_t rows, vds_cell *cells, uint8_t attr);
int vds_put(vds *v, uint8_t id, uint8_t col, uint8_t row, uint8_t attr, const char *text, size_t n);
int vds_fill(vds *v, uint8_t id, uint8_t col, uint8_t row, uint8_t w, uint8_t h, uint8_t ch, uint8_t attr);
int vds_scroll(vds *v, uint8_t id, int dy, uint8_t attr);  /* whole surface; dy > 0 moves up */
int vds_cursor(vds *v, uint8_t id, uint8_t col, uint8_t row, uint8_t shape, uint8_t blink);
/* Advance local time: blinking carets change phase every VDS_BLINK_MS. */
#define VDS_BLINK_MS 500
void vds_tick(vds *v, uint32_t ms);

/* Windows. x, y is the client origin; x snaps unless VDS_W_FREE_X or snap 1. */
int vds_window_open(vds *v, uint8_t id, uint8_t surface, int16_t x, int16_t y,
                    uint8_t cols, uint8_t rows, uint8_t flags, const char *title);
int vds_window_move(vds *v, uint8_t id, int16_t x, int16_t y);
int vds_window_resize(vds *v, uint8_t id, uint8_t cols, uint8_t rows);
int vds_window_raise(vds *v, uint8_t id);
int vds_window_lower(vds *v, uint8_t id);
int vds_window_flags(vds *v, uint8_t id, uint8_t flags);
int vds_window_scroll(vds *v, uint8_t id, uint8_t col, uint8_t row);
int vds_window_title(vds *v, uint8_t id, const char *title);
int vds_window_surface(vds *v, uint8_t id, uint8_t surface);
int vds_window_close(vds *v, uint8_t id);
vds_rect vds_window_outer(const vds *v, uint8_t id);    /* incl. title tab and shadow */

/* Sprites (cursor = VDS_CURSOR_SPRITE). Defining one leaves it hidden. */
int vds_sprite_define(vds *v, uint8_t id, uint8_t w, uint8_t h, uint8_t hot_x, uint8_t hot_y,
                      uint8_t transparent, const uint8_t *pixels);
/* Built-in 12x16 arrow (colours 0 and 3, transparent 15, hotspot 0,0). */
#define VDS_ARROW_W 12
#define VDS_ARROW_H 16
#define VDS_ARROW_TRANSPARENT 15
extern const uint8_t vds_arrow_cursor[VDS_ARROW_W / 2 * VDS_ARROW_H];
int vds_sprite_move(vds *v, uint8_t id, int16_t x, int16_t y);
int vds_sprite_show(vds *v, uint8_t id, int visible);
int vds_sprite_undefine(vds *v, uint8_t id);
vds_rect vds_sprite_rect(const vds *v, uint8_t id);

/* Damage and frames. */
void vds_damage(vds *v, vds_rect r);
void vds_damage_all(vds *v);
/* Compose damaged scanlines top to bottom until about `budget` pixels are
 * done, stopping only at a scanline boundary. Returns 1 when no damage is
 * left, 0 when the rest carries over to the next frame. */
int vds_frame(vds *v, uint32_t budget);

#ifdef __cplusplus
}
#endif
#endif
