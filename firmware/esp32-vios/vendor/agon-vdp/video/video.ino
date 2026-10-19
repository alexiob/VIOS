//
// Title:			Agon Video BIOS
// Author:			Dean Belfield
// Contributors:	Jeroen Venema (Sprite Code, VGA Mode Switching)
//					Damien Guard (Fonts)
//					Igor Chaves Cananea (vdp-gl maintenance)
//					Steve Sims (Audio enhancements, refactoring, bug fixes)
// Created:			22/03/2022
// Last Updated:	11/11/2023
//
// Modinfo:
// 11/07/2022:		Baud rate tweaked for Agon Light, HW Flow Control temporarily commented out
// 26/07/2022:		Added VDU 29 support
// 03/08/2022:		Set codepage 1252, fixed keyboard mappings for AGON, added cursorTab, VDP serial protocol
// 06/08/2022:		Added a custom font, fixed UART initialisation, flow control
// 10/08/2022:		Improved keyboard mappings, added sprites, audio, new font
// 05/09/2022:		Moved the audio class to agon_audio.h, added function prototypes in agon.h
// 02/10/2022:		Version 1.00: Tweaked the sprite code, changed bootup title to Quark
// 04/10/2022:		Version 1.01: Can now change keyboard layout, origin and sprites reset on mode change, available modes tweaked
// 23/10/2022:		Version 1.02: Bug fixes, cursor visibility and scrolling
// 15/02/2023:		Version 1.03: Improved mode, colour handling and international support
// 04/03/2023:					+ Added logical screen resolution, sendScreenPixel now sends palette index as well as RGB values
// 09/03/2023:					+ Keyboard now sends virtual key data, improved VDU 19 to handle COLOUR l,p as well as COLOUR l,r,g,b
// 15/03/2023:					+ Added terminal support for CP/M, RTC support for MOS
// 21/03/2023:				RC2 + Added keyboard repeat delay and rate, logical coords now selectable
// 22/03/2023:					+ VDP control codes now indexed from 0x80, added paged mode (VDU 14/VDU 15)
// 23/03/2023:					+ Added VDP_GP
// 26/03/2023:				RC3 + Potential fixes for FabGL being overwhelmed by faster comms
// 27/03/2023:					+ Fix for sprite system crash
// 29/03/2023:					+ Typo in boot screen fixed
// 01/04/2023:					+ Added resetPalette to MODE, timeouts to VDU commands
// 08/04/2023:				RC4 + Removed delay in readbyte_t, fixed VDP_SCRCHAR, VDP_SCRPIXEL
// 12/04/2023:					+ Fixed bug in play_note
// 13/04/2023:					+ Fixed bootup fail with no keyboard
// 17/04/2023:				RC5 + Moved wait_completion in vdu so that it only executes after graphical operations
// 18/04/2023:					+ Minor tweaks to wait completion logic
// 12/05/2023:		Version 1.04: Now uses vdp-gl instead of FabGL, implemented GCOL mode, sendModeInformation now sends video mode
// 19/05/2023:					+ Added VDU 4/5 support
// 25/05/2023:					+ Added VDU 24, VDU 26 and VDU 28, fixed inverted text colour settings
// 30/05/2023:					+ Added VDU 23,16 (cursor movement control)
// 28/06/2023:					+ Improved get_screen_char, fixed vdu_textViewport, cursorHome, changed modeline for Mode 2
// 30/06/2023:					+ Fixed vdu_sys_sprites to correctly discard serial input if bitmap allocation fails
// 13/08/2023:				RC2	+ New video modes, mode change resets page mode
// 05/09/2023:					+ New audio enhancements, improved mode change code
// 12/09/2023:					+ Refactored
// 11/11/2023:				RC3 + See Github for full list of changes

#include <WiFi.h>
#include <HardwareSerial.h>
#include <fabgl.h>

#define VERSION			1
#define REVISION		4
#define RC				0

#define	DEBUG			0						// Serial Debug Mode: 1 = enable
#define SERIALBAUDRATE	115200

HardwareSerial	DBGSerial(0);

bool			terminalMode = false;			// Terminal mode (for CP/M)
bool			consoleMode = false;			// Serial console mode (0 = off, 1 = console enabled)

#include "vios_bridge_api.h"
#include "agon.h"								// Configuration file
#include "agon_ps2.h"							// Keyboard support
#include "agon_audio.h"							// Audio support
#include "agon_ttxt.h"
#include "graphics.h"							// Graphics support
#include "cursor.h"								// Cursor support
#include "vdp_protocol.h"						// VDP Protocol
#include "vdu_stream_processor.h"
#include "hexload.h"

fabgl::Terminal			Terminal;				// Used for CP/M mode
VDUStreamProcessor *	processor;				// VDU Stream Processor

#include "zdi.h"								// ZDI debugging console

#include "vios_zdi.h"
#include "vios_bridge.h"
#include "vios_console_vdp.h"
#include "vios_gui.h"
#include "vios_g0_probe.h"
#include "vios_frame.h"

void setup() {
	disableCore0WDT(); delay(200);				// Disable the watchdog timers
	disableCore1WDT(); delay(200);
	DBGSerial.setRxBufferSize(2048);
	DBGSerial.begin(SERIALBAUDRATE, SERIAL_8N1, 3, 1);
	DBGSerial.write((uint8_t)0); // Resynchronize after ROM boot text.
	if(!vios_bridge_init()){vios_unsupported("ERR bridge resource allocation; graphics disabled\r\n");while(true){vios_bridge_poll();delay(1);}}
	if(!vios_bridge_start()){vios_unsupported("ERR control task allocation; graphics disabled\r\n");while(true){vios_bridge_poll();delay(1);}}
	#if VIOS_ZDI_EARLY_PROBE
	vios_bridge_probe();
	vios_bridge_handoff();
	return;
	#endif
	setupVDPProtocol();
	processor = new VDUStreamProcessor(&VDPSerial);
	vios_frame_start();
	initialised = true; // VIOS cold boot does not wait for MOS handshake.
	setupKeyboardAndMouse();
	init_audio();
	copy_font();
	// VIOS requests mode information explicitly after its UART initialization.
	// An unsolicited packet here could wait on an old/halted CPU RTS forever.
	#if VIOS_GUI_BOOT
	vios_display_mode0();					// GUI first: desktop before the CPU loads
	if (!vios_gui_start()) { set_mode(1); boot_screen(); }
	#else
	set_mode(1);
	boot_screen();
	#endif
	vios_bridge_boot();
	vios_bridge_handoff();
}

// The main loop
//
void loop() {
	#if VIOS_ZDI_EARLY_PROBE
	while(true)delay(1);
	#endif
	uint32_t count = 0;						// Generic counter, incremented every loop iteration
	bool cursorVisible = false;
	bool cursorState = false;

	while (true) {
		uint32_t section = (uint32_t)esp_timer_get_time();
		vios_bridge_service_vdp();
		vios_loop_section(0, section);
		if (terminalMode) {
			do_keyboard_terminal();
			continue;
		}
		cursorVisible = ((count & 0xFFFF) == 0);
		if (cursorVisible) {
    		if (!cursorState && ttxtMode) ttxt_instance.flash(true);
			cursorState = !cursorState;
			do_cursor();
			vios_glyph_state_valid = false;
      		if (!cursorState && ttxtMode) ttxt_instance.flash(false);
		}
		section = (uint32_t)esp_timer_get_time();
		vios_console_tick();
		vios_loop_section(1, section);
		vios_g0_service();
		section = (uint32_t)esp_timer_get_time();
		if (!vios_cpu_is_paused()) {
			do_keyboard();
			do_mouse();
		}
		vios_loop_section(2, section);

		if (!vios_cpu_is_paused() && processor->byteAvailable()) {
			if (cursorState) {
				cursorState = false;
				do_cursor();
				vios_glyph_state_valid = false;
			}
			vios_vdp_parser_busy(true);
			uint32_t parse_start = (uint32_t)esp_timer_get_time();
			processor->processNext();
			vios_parser_time((uint32_t)esp_timer_get_time() - parse_start);
			vios_vdp_parser_busy(false);
		}
		count++;
	}
}

// Handle the keyboard: BBC VDU Mode
//
void do_keyboard() {
	uint8_t keycode;
	uint8_t modifiers;
	uint8_t vk;
	uint8_t down;
	if (getKeyboardKey(&keycode, &modifiers, &vk, &down)) {
		if (vios_gui_key(keycode, modifiers, vk, down)) return;	// window manager, menus, widgets
		// Handle some control keys
		//
		switch (keycode) {
			case 14: setPagedMode(true); break;
			case 15: setPagedMode(false); break;
		}
		// Create and send the packet back to MOS
		//
		uint8_t packet[] = {
			keycode,
			modifiers,
			vk,
			down,
		};
		if (down) vios_probe_key(keycode);
		processor->send_packet(PACKET_KEYCODE, sizeof packet, packet);
	}
	else if (vios_injected_key(&keycode)) {
		// VIOS Stage 0: host-injected key at the PS/2 boundary (down then up).
		uint8_t press[] = { keycode, 0, 0, 1 }, release[] = { keycode, 0, 0, 0 };
		bool used = vios_gui_key(keycode, 0, 0, 1);
		vios_gui_key(keycode, 0, 0, 0);
		if (used) return;
		vios_probe_key(keycode);
		processor->send_packet(PACKET_KEYCODE, sizeof press, press);
		processor->send_packet(PACKET_KEYCODE, sizeof release, release);
	}
}

// Handle the keyboard: CP/M Terminal Mode
// 
void do_keyboard_terminal() {
	uint8_t ascii;
	if (getKeyboardKey(&ascii)) {
		// send raw byte straight to z80
		processor->writeByte(ascii);
	}

	// Write anything read from z80 to the screen
	//
	while (processor->byteAvailable()) {
		Terminal.write(processor->readByte());
	}
}

// Handle the mouse
//
void do_mouse() {
#if VIOS_USB_MUX
	return; // No mouse port on this board profile; ZDI owns GPIO26/27.
#endif
	// get mouse delta, if the mouse is active
	MouseDelta delta;
	if (mouseMoved(&delta)) {
		auto mouse = getMouse();
		auto mStatus = mouse->status();
		// update mouse cursor position if it's active
		setMouseCursorPos(mStatus.X, mStatus.Y);
		processor->sendMouseData(&delta);
	}
}

// The boot screen
//
void boot_screen() {
	printFmt("Agon Quark VDP Version %d.%02d", VERSION, REVISION);
	#if RC > 0
		printFmt(" RC%d", RC);
	#endif
	printFmt("\n\r");
}

// Debug printf to PC
//
void debug_log(const char *format, ...) {
	#if DEBUG == 1
	va_list ap;
	va_start(ap, format);
	auto size = vsnprintf(nullptr, 0, format, ap) + 1;
	if (size > 0) {
		va_end(ap);
		va_start(ap, format);
		char buf[size + 1];
		vsnprintf(buf, size, format, ap);
		DBGSerial.print(buf);
	}
	va_end(ap);
	#endif
}

// Set console mode
// Parameters:
// - mode: 0 = off, 1 = on
//
void setConsoleMode(bool mode) {
	consoleMode = mode;
}

// Switch to terminal mode
//
void switchTerminalMode() {
#if VIOS_USB_MUX
	vios_unsupported("ERR legacy terminal unavailable in VIOS multiplex profile\r\n");
	return;
#else
	cls(true);
	canvas.reset();
	Terminal.begin(_VGAController.get());	
	Terminal.connectSerialPort(VDPSerial);
	Terminal.enableCursor(true);
	terminalMode = true;
#endif
}

void print(char const * text) {
	for (auto i = 0; i < strlen(text); i++) {
		processor->vdu(text[i]);
	}
}

void printFmt(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	int size = vsnprintf(nullptr, 0, format, ap) + 1;
	if (size > 0) {
		va_end(ap);
		va_start(ap, format);
		char buf[size + 1];
		vsnprintf(buf, size, format, ap);
		print(buf);
	}
	va_end(ap);
}
