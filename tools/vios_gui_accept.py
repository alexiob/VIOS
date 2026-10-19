#!/usr/bin/env python3
"""On-board acceptance for the GUI display server (window manager, menus, widgets).

Runs through the harness (127.0.0.1:8765) on a board booted into the GUI:
  windows   open a decorated window over the desktop, move it, raise, close;
            framebuffer pixels change where it is and return to the desktop
            colour where it was;
  commands  a Ctrl+S accelerator injected at the window manager produces a
            COMMAND event the CPU pulls with UI-NEXT;
  widgets   an input and a button: typed text reads back with UI-WGET, Tab +
            Space activates the button (ACTIVATE event);
  health    no protocol errors, frame work and vblank start measured.
Uses window 3 and surface 3 only (console 32/16 and workbench 26/10 untouched).
"""
import argparse
import json
from pathlib import Path
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient

WINDOW, SURFACE = 3, 3


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=120) as c:
            c.initialize()
            c.tool('connect', {})

            def ev(source):
                out = c.tool('evaluate', {'source': source, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{source!r}: {out["output"][-200:]}')
                return out['output']

            def result(source):
                """The line after the command's echo (background task output such as
                HELLO's heartbeat may come before it)."""
                lines = ev(source).split('\r\n')
                for i, line in enumerate(lines):
                    if line.strip() == source.strip() and i + 1 < len(lines):
                        return lines[i + 1]
                raise AssertionError(f'no echo of {source!r}: {lines}')

            def pixels(points):
                time.sleep(0.4)  # let the compositor finish the damage (a few frames)
                return c.tool('esp_gui', {'pixels': points})['pixels']

            def drain():
                events = []
                while True:
                    words = result('GA-NEXT').split()
                    if words == ['0']:
                        return events
                    events.append([int(w) for w in words])  # flag arg id kind (printed top of stack first)

            # IF is compile-only: one helper prints flag arg id kind, or 0.
            ev(': GA-NEXT UI-NEXT IF -1 . . . . ELSE 0 . THEN ;')
            status = c.tool('esp_gui')['status']
            check('gui_on', status.startswith('GUI on'), status)
            drain()
            c.tool('esp_stats', {'reset': True})
            old_center, new_center = [152, 48], [480, 152]  # disjoint 240x64 window spots on the desktop above the console window
            before = pixels([old_center, new_center])
            check('points_on_desktop', before[0] == before[1], before)
            # Windows: open, move, raise, close.
            ev(f'{SURFACE} 60 20 48 UI-SURFACE  S" VIOS GUI acceptance" {SURFACE} 1 1 48 UI-TYPE')
            ev(f'{WINDOW} {SURFACE} 32 16 30 8 UI-VISIBLE UI-DECOR OR UI-SHADOW OR UI-WINDOW  S" Accept" {WINDOW} UI-TITLE  {WINDOW} UI-FOCUS')
            opened = pixels([old_center, new_center])
            r['pixels'] = {'before': before, 'opened': opened}
            check('window_draws', opened[0] != before[0], (before, opened))
            ev(f'{WINDOW} 360 120 30 8 UI-PLACE')
            moved = pixels([old_center, new_center])
            r['pixels']['moved'] = moved
            check('move_restores_desktop', moved[0] == before[0] and moved[1] != before[1], (before, moved))
            ev(f'{WINDOW} UI-RAISE')
            # Commands: Ctrl+S through the window manager -> COMMAND event.
            drain()
            ev('S" &Save" 3 UI-ENABLED 0 UI-CTRL CHAR s UI-COMMAND')
            used = c.tool('esp_gui', {'vk': 0, 'ascii': ord('s'), 'mods': 1})['used']
            events = drain()
            r['command_events'] = events
            check('accelerator_command_event', used and any(e[0] in (-1, 0xFFFFFF) and e[3] == 17 and e[1] == 3 for e in events), (used, events))
            # Widgets: input + button in the window.
            ev(f'0 {WINDOW} UI-BOX UI-NONE 0 0 0 0 UI-WF-FILL UI-WIDGET  0 UI-COLUMN 1 UI-LAYOUT')
            ev(f'1 {WINDOW} UI-INPUT 0 0 0 0 0 0 UI-WIDGET  1 1 0 UI-WSIZE')
            ev(f'2 {WINDOW} UI-BUTTON 0 0 0 0 0 0 UI-WIDGET  2 1 0 UI-WSIZE  S" &Go" 2 UI-WTEXT')
            ev('1 UI-WFOCUS')
            drain()
            for ch in 'ok':
                c.tool('esp_gui', {'vk': 0, 'ascii': ord(ch)})
            ev('CREATE GTXT 32 ALLOT')
            got = result('1 GTXT UI-WGET . .  GTXT C@ EMIT GTXT 1+ C@ EMIT')
            r['input_readback'] = got
            check('input_text', got.split()[:2] == ['2', '2'] and got.split()[2].startswith('ok'), got)
            c.tool('esp_gui', {'vk': 142, 'ascii': 9})
            c.tool('esp_gui', {'vk': 1, 'ascii': ord(' ')})
            events = drain()
            r['widget_events'] = events
            check('button_activates', any(e[3] == 2 and e[2] == 2 for e in events), events)  # kind ACTIVATE, widget 2
            ev('1 UI-WDELETE 2 UI-WDELETE 0 UI-WDELETE')
            ev(f'{WINDOW} UI-CLOSE')
            closed = pixels([old_center, new_center])
            r['pixels']['closed'] = closed
            check('close_restores_desktop', closed == before, (before, closed))
            sync = result('UI-SYNC . . .').split()
            r['sync'] = sync
            check('no_protocol_errors', sync and sync[0] == '0', sync)
            s = c.tool('esp_stats')
            r['frames'] = {k: s[k] for k in ('frames', 'vblank_start_max', 'frame_work_max')}
            r['gui'] = c.tool('esp_gui')['status']
            check('frame_budget', s['frame_work_max'] <= 6000 and s['vblank_start_max'] <= 1000, r['frames'])
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'error', 'frames')}))


if __name__ == '__main__':
    main()
