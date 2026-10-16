#ifndef VIOS_PLATFORM_INPUT_H
#define VIOS_PLATFORM_INPUT_H
/* Pinned VDP 1.04/FabGL VirtualKey wire values (fabutils.h). Key-up ASCII
 * and modifiers can repeat the previous key-down: always inspect down first. */
struct platform_key_event { unsigned char ascii,modifiers,virtual_key,down; };
enum platform_virtual_key {
 PLATFORM_KEY_SPACE=1,
 PLATFORM_KEY_ESCAPE=125,PLATFORM_KEY_DELETE=130,PLATFORM_KEY_BACKSPACE=132,
 PLATFORM_KEY_HOME=133,PLATFORM_KEY_KP_HOME=134,PLATFORM_KEY_END=135,PLATFORM_KEY_KP_END=136,
 PLATFORM_KEY_TAB=142,PLATFORM_KEY_RETURN=143,PLATFORM_KEY_KP_ENTER=144,
 PLATFORM_KEY_PAGEUP=146,PLATFORM_KEY_KP_PAGEUP=147,
 PLATFORM_KEY_PAGEDOWN=148,PLATFORM_KEY_KP_PAGEDOWN=149,
 PLATFORM_KEY_UP=150,PLATFORM_KEY_KP_UP=151,PLATFORM_KEY_DOWN=152,PLATFORM_KEY_KP_DOWN=153,
 PLATFORM_KEY_LEFT=154,PLATFORM_KEY_KP_LEFT=155,PLATFORM_KEY_RIGHT=156,PLATFORM_KEY_KP_RIGHT=157,
 PLATFORM_KEY_F5=163,PLATFORM_KEY_F6=164,PLATFORM_KEY_F7=165,PLATFORM_KEY_F8=166,PLATFORM_KEY_F10=168
};
enum platform_key_modifier {
 PLATFORM_MOD_CTRL=1,PLATFORM_MOD_SHIFT=2,PLATFORM_MOD_LALT=4,PLATFORM_MOD_RALT=8,
 PLATFORM_MOD_CAPSLOCK=16,PLATFORM_MOD_NUMLOCK=32,PLATFORM_MOD_SCROLLLOCK=64,PLATFORM_MOD_GUI=128
};
/* Only physical 0x81 events are routed; FE host console input is unaffected.
 * A route transition clears queued UI events. Polling is nonblocking.
 * Route/event calls leave RX paused, including empty/null polls. Complete
 * bounded C work before platform_input_resume() yields a receive window. */
void platform_input_resume(void);
void platform_keyboard_route(int enabled);
int platform_keyboard_event(struct platform_key_event *out);
unsigned platform_keyboard_dropped(void);
#endif
