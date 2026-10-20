#ifndef VIOS_TOOLS_H
#define VIOS_TOOLS_H
/* GUI development tools (kernel/vios_tools.c): 0 Browser, 1 Workspace,
 * 2 Inspector, 3 Manager. Windows 20-25, surfaces 5-9, widgets 40-63 and
 * command ids 0x7F00-0x7FFF are reserved for them. */
int vios_tools_init(void);
/* REPL idle: set up the commands once per display-server epoch, poll events
 * when the server nudges (or once a second while a tool is open), act on them. */
void vios_tools_idle(void);
int vios_tools_open(unsigned tool);
unsigned vios_tools_open_mask(void);
#endif
