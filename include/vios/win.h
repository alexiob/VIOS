#ifndef VIOS_WIN_H
#define VIOS_WIN_H
#include "forth/modules.h"
/* /dev/win and /dev/ui/tree (kernel/vios_win.c): the display server's windows
 * and widgets as files. The ESP owns their state and renders and parses the
 * text (vds_files.c, ops 9C-9E); the CPU keeps only the cursors of open
 * handles. Nodes: a widget id 0..63, or VIOS_WIN_WINDOW | window id (w0..w26). */
enum { VIOS_WIN_LIST = 1, VIOS_WIN_NEW, VIOS_WIN_CTL, VIOS_WIN_TEXT, VIOS_WIN_EVENT, VIOS_WIN_TREE };
#define VIOS_WIN_WINDOW 0x80
#define VIOS_WIN_NONE 0xFF
#define VIOS_WIN_STREAM 0x14            /* event kind: a routed node's text line */
struct vios_win_cursor { unsigned char file, node, written, col; unsigned short step, sub; };
/* The part of a path after "/dev/": 1 and the file and node if it is ours. */
int vios_win_path(const char *device, unsigned char *file, unsigned char *node);
/* Start a cursor; 0 (vios_win_error) if the node does not exist. */
int vios_win_open(struct vios_win_cursor *c);
/* Up to want bytes from the cursor: the count (short at the end) or -1. */
int vios_win_read(struct vios_win_cursor *c, unsigned char *out, unsigned want);
/* Apply a write of len bytes of Forth memory at addr: 1 ok, 0 on error. */
int vios_win_write(struct vios_win_cursor *c, fcell addr, fcell len);
/* Stream a node's events as VIOS_WIN_STREAM text lines (on 0: stop). */
void vios_win_route(unsigned node, int on);
const char *vios_win_error(void);
#endif
