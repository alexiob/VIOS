#ifndef VIOS_UI_H
#define VIOS_UI_H
#include <stddef.h>
/* Forth words for the ESP display server (kernel/vios_ui.c). Window ids
 * 0..26 and surface ids 0..10 belong to the CPU; higher ids are reserved for
 * the server's menus and bars. */
#define VIOS_UI_WINDOWS 27
#define VIOS_UI_SURFACES 11
#define VIOS_UI_EVENT_MAX 29
/* With VIOS_GUI=1 the workbench text screen is this display-server surface. */
#define VIOS_GUI_SCREEN_SURFACE 10
#define VIOS_UI_WIDGETS 64
#define VIOS_UI_ROWS 1024
#define VIOS_UI_TEXT_MAX 4096
int vios_ui_init(void);
/* C clients: ops go into the current batch (sent at once unless batching);
 * requests flush and wait for the display server's reply (ops <= 246 bytes). */
void vios_ui_op(const unsigned char *bytes, unsigned n);
int vios_ui_request(const unsigned char *bytes, unsigned n, unsigned char *reply, unsigned capacity, unsigned *length);
void vios_ui_begin(void);
void vios_ui_commit(void);
/* Events pulled from the server go to hook first; it returns 1 to keep an
 * event (seq kind id args...) from the UI-NEXT ring. */
void vios_ui_claim(int (*hook)(const unsigned char *event, unsigned len));
unsigned vios_ui_resets(void);   /* UI-RESETs so far: the tools reinstall after one */
/* STREAM events (kind 0x14: a routed node's text line) go to hook instead
 * (vios_handles.c: /dev/win/N/event). */
void vios_ui_route(int (*hook)(const unsigned char *event, unsigned len));
int vios_ui_poll(void);
/* TextView text (chunked), state and read-back. */
struct vios_ui_text_info { unsigned length, caret, anchor, revision, dirty; };
void vios_ui_text(unsigned char id, const char *s, size_t n, int append);
int vios_ui_text_info(unsigned char id, struct vios_ui_text_info *out);
int vios_ui_text_read(unsigned char id, unsigned offset, unsigned n, void (*sink)(void *, const char *, unsigned), void *ctx);
#endif
