#ifndef VIOS_TEXT_SCREEN_H
#define VIOS_TEXT_SCREEN_H
#include <stddef.h>
#include <stdint.h>
#define VIOS_TEXT_COLUMNS 80
#define VIOS_TEXT_ROWS 60
#define VIOS_TEXT_BATCH_BYTES 128
struct vios_text_cell { unsigned char ch,attr; };
/* cells counts transmitted glyphs (including profitable clean gaps); spans
 * counts styled runs, batches/bytes include queued operation traffic. */
struct vios_text_stats { uint32_t cells,spans,batches,bytes; };
/* Call after mode/palette/CLS has established a blank fg3/bg0 screen. The
 * workbench owns display state until close; raw user VDU is not tracked. */
void vios_text_reset(void);
void vios_text_begin_frame(void);
/* Retained composition may clear just a pane/line. Reset/full-frame clear
 * advance generation so cached composers detect an externally blanked plane. */
void vios_text_clear(unsigned x,unsigned y,unsigned width,unsigned height);
uint32_t vios_text_generation(void);
void vios_text_put(unsigned x,unsigned y,const char *text,size_t length,unsigned fg,unsigned bg);
/* Shift a clean rectangle; positive rows move up, negative down. Vacated
 * cells become fg3/bg0 spaces. Returns 0 without mutation for invalid
 * geometry, abs(rows)>31, abs(rows)>=height, region dirt, an existing queued
 * operation, or a rectangle containing the reserved bottom-right cell.
 * Dirt outside the rectangle is allowed. Reset cancels/invalidate scrolls
 * and their two dedicated VDP command buffers (IDs 0xFA00 and 0xFA01). */
int vios_text_scroll(unsigned x,unsigned y,unsigned width,unsigned height,int rows);
unsigned vios_text_render(unsigned budget);
int vios_text_pending(void);
int vios_text_cell(unsigned x,unsigned y,struct vios_text_cell *out);
void vios_text_stats(struct vios_text_stats *out);
void vios_text_stats_reset(void);
/* 1 when the screen belongs to the GUI display server (text_screen_vds.c):
 * the runtime then sends no mode changes, clears or legacy VDU setup. */
int vios_text_owns_display(void);
/* The workbench view opened / closed: the GUI shows or hides its window
 * (no-ops on VDU). */
void vios_text_show(void);
void vios_text_hide(void);
#endif
