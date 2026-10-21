/* VIOS Display Server G5: canvas display lists and icons. Portable C99, no
 * heap. A canvas (vds.h) keeps a retained display list of up to 255 items
 * and a 4 bpp raster rebuilt from it: edits only mark a dirty rect, and
 * vds_canvas_prepare() replays the list there once per frame, under a pixel
 * budget, before composition. The adapter supplies each canvas's memory
 * (vds_canvas_bytes: the item array, then the raster). See
 * docs/superpowers/specs/2026-10-21-vios-gui-g5-canvas.md. */
#ifndef VIOS_VDS_CANVAS_H
#define VIOS_VDS_CANVAS_H
#include "vds.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VDS_CANVAS_ITEMS 255            /* item ids 0..254; 255 means all / none */
#define VDS_CANVAS_TEXT_MAX 20
#define VDS_CANVAS_RASTER_MAX (256u * 1024u)    /* all canvases' rasters together */
#define VDS_BITMAP_MAX 64               /* width and height limit */
#define VDS_ICON_FIRST 24               /* built-in 16x16 icons: bitmaps 24..31 */
#define VDS_ICON_SIZE 16
#define VDS_ICON_TRANSPARENT 15

enum { VDS_ICON_DOCUMENT = 24, VDS_ICON_FOLDER, VDS_ICON_WORD, VDS_ICON_MODULE,
       VDS_ICON_INFO, VDS_ICON_WARNING, VDS_ICON_ERROR, VDS_ICON_DISK };

/* Item kinds. a b c d are canvas pixels: x y w h, or x0 y0 x1 y1 for a line. */
enum { VDS_DRAW_NONE = 0, VDS_DRAW_DOT = 1, VDS_DRAW_LINE, VDS_DRAW_RECT, VDS_DRAW_BAR,
       VDS_DRAW_OVAL, VDS_DRAW_DISC, VDS_DRAW_TEXT, VDS_DRAW_ICON, VDS_DRAW_STIPPLE, VDS_DRAW_KINDS };

/* extra: the bitmap of an ICON, the second colour of a STIPPLE. */
typedef struct {
    uint8_t kind, colour, extra, n;
    int16_t a, b, c, d;
    char text[VDS_CANVAS_TEXT_MAX];
} vds_item;

/* Memory the adapter must supply for a w x h canvas. */
uint32_t vds_canvas_bytes(int16_t w, int16_t h);

/* Create or replace canvas id on a surface: x a multiple of 8, 1 <= w <= 640,
 * 1 <= h <= 480, all rasters within VDS_CANVAS_RASTER_MAX. Drops the items. */
int vds_canvas_create(vds *v, uint8_t id, uint8_t surface, int16_t x, int16_t y, int16_t w, int16_t h,
                      uint8_t bg, uint8_t flags, void *memory);
/* Would vds_canvas_create accept these (given memory)? Check before asking
 * the adapter for memory: an adapter may move the memory of an existing
 * canvas id, which must then be replaced. */
int vds_canvas_valid(const vds *v, uint8_t id, uint8_t surface, int16_t x, int16_t w, int16_t h, uint8_t bg, uint8_t flags);
int vds_canvas_move(vds *v, uint8_t id, int16_t x, int16_t y);
int vds_canvas_flags(vds *v, uint8_t id, uint8_t flags);       /* VDS_CV_VISIBLE | VDS_CV_CLEAR */
int vds_canvas_delete(vds *v, uint8_t id);

int vds_canvas_set(vds *v, uint8_t id, uint8_t item, const vds_item *it);
int vds_canvas_erase(vds *v, uint8_t id, uint8_t item);        /* 255: every item */
int vds_canvas_shift(vds *v, uint8_t id, uint8_t item, int16_t dx, int16_t dy);
int vds_canvas_ink(vds *v, uint8_t id, uint8_t item, uint8_t colour);
const vds_item *vds_canvas_item(const vds *v, uint8_t id, uint8_t item);   /* NULL if none */
/* Topmost item whose box holds canvas pixel (x, y), 255 if none. */
uint8_t vds_canvas_hit(const vds *v, uint8_t id, int x, int y);
/* An item's box, clipped to its canvas. */
vds_rect vds_canvas_item_box(const vds *v, uint8_t id, const vds_item *it);

/* Bitmaps (ids 0..31; 24..31 are the built-in icons after vds_canvas_icons).
 * Defining or changing one redraws the canvases that show it. */
int vds_bitmap_define(vds *v, uint8_t id, uint8_t w, uint8_t h, uint8_t transparent, const uint8_t *pixels);
void vds_bitmap_changed(vds *v, uint8_t id);
void vds_bitmap_undefine(vds *v, uint8_t id);
void vds_canvas_icons(vds *v);
extern const uint8_t vds_icons[8][VDS_ICON_SIZE / 2 * VDS_ICON_SIZE];

/* Rasterise dirty canvas rows, at least one band, then until about `budget`
 * pixels are done; damages what changed on screen. Returns pixels done. */
uint32_t vds_canvas_prepare(vds *v, uint32_t budget);
/* As vds_canvas_prepare, also stopping once budget_us microseconds have
 * passed by v->clock_us (checked after each band of at most
 * VDS_CANVAS_BAND_PX pixels). */
#define VDS_CANVAS_BAND_PX (640u * 24u)
uint32_t vds_canvas_prepare_within(vds *v, uint32_t budget, uint32_t budget_us);
int vds_canvas_pending(const vds *v);
/* Replay canvas id's list over its canvas-local rect r into `out` (rows of
 * (w + 1) / 2 bytes): what prepare draws. For tests and inspection. */
void vds_canvas_replay(const vds *v, uint8_t id, vds_rect r, uint8_t *out);

#ifdef __cplusplus
}
#endif
#endif
