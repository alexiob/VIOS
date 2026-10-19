#ifndef VIOS_UI_H
#define VIOS_UI_H
/* Forth words for the ESP display server (kernel/vios_ui.c). Window ids
 * 0..26 and surface ids 0..10 belong to the CPU; higher ids are reserved for
 * the server's menus and bars. */
#define VIOS_UI_WINDOWS 27
#define VIOS_UI_SURFACES 11
#define VIOS_UI_EVENT_MAX 29
/* With VIOS_GUI=1 the workbench text screen is this display-server surface. */
#define VIOS_GUI_SCREEN_SURFACE 10
int vios_ui_init(void);
#endif
