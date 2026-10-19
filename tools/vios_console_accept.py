#!/usr/bin/env python3
"""VGA console view acceptance on the Agon through the shared owner.

Reads the real framebuffer with CHAR@ (pixel reconstruction), so it checks
what a person at the monitor sees. Defines temporary RAM words ROW?/SCR,
clears the console text, opens/closes the editor repeatedly. No SD writes.
"""
import argparse
import json
from pathlib import Path
import re
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--round-trips', type=int, default=20)
    a = p.parse_args()
    r = {'status': 'running', 'checks': {}}

    def check(name, ok, detail=None):
        r['checks'][name] = {'ok': bool(ok), 'detail': detail}
        if not ok:
            raise AssertionError(f'{name}: {detail}')
    try:
        with RPCClient(timeout=300) as c:
            c.initialize()

            def ev(s, t=290):
                out = c.tool('evaluate', {'source': s, 'timeout': t})
                if not out['ok']:
                    raise AssertionError(f'{s!r} failed: {out["output"][-200:]}')
                return out
            drops = lambda: int(re.search(r'console-dropped (\d+)', ev('VDP-INFO')['output'])[1])
            # VDP CHAR@ matches glyphs against the *current* text colours; the
            # blinking cursor can leave them inverse, so reset them first.
            ev(': ROW? 80 0 DO I OVER CHAR@ DUP 32 < IF DROP 46 THEN EMIT LOOP DROP CR ; : SCR 3 COLOUR 128 COLOUR 0 DO I ROW? LOOP ;')
            ev(': FILL 0 DO I . CR LOOP ;')

            def screen(rows=12, settle=1.5):
                # The console is rendered on the ESP (GUI Stage 1c): read its exact
                # text over the control channel. Reading with CHAR@ would print
                # into the very console being read and scroll it mid-read.
                time.sleep(settle)
                return c.tool('esp_console', {'rows': rows})['rows']
            r['dropped_before'] = drops()
            ev('WB-CONSOLE-CLEAR')
            ev('6 7 * .')
            ev('S" console-marker" CONSOLE-WRITE CR')
            shown = screen()
            r['screen_after_commands'] = shown
            check('command_echo_visible', 'vios> 6 7 * .' in shown, shown)
            check('result_visible', '42' in shown, shown)
            check('marker_visible', 'console-marker' in shown, shown)
            check('envelopes_hidden', not any('@' in l and '!' in l for l in shown), shown)
            # The ESP's text is what is on the VGA output: CHAR@ matches the drawn
            # glyph pixels of the marker's first cell (short output, no scroll).
            marker_row = shown.index('console-marker')
            pixel = ev(f'3 COLOUR 128 COLOUR 0 {marker_row} CHAR@ .')['output']
            r['marker_pixel_readback'] = pixel.split('\r\n')[1:2]
            check('pixels_match_text', '99 ' in pixel, pixel[-80:])
            times = []
            for i in range(a.round_trips):
                t = time.perf_counter()
                ev('WB-OPEN')
                ev('30 FILL')          # output while the editor is visible
                ev('WB-CLOSE')
                times.append(round((time.perf_counter() - t) * 1000))
            r['round_trip_ms'] = times
            ev('WB-CONSOLE-CLEAR')
            ev('S" after-trips" CONSOLE-WRITE CR')
            ev('WB-OPEN')
            ev('S" hidden-while-editor" CONSOLE-WRITE CR')
            ev('WB-CLOSE')
            r['state_before_read'] = ev('WB-CONSOLE? WB-RENDER-STATS WB-STATUS')['output'].split('\r\n')[1:4]
            shown = [l for l in screen(rows=58, settle=3) if l]
            r['screen_after_trip'] = shown
            check('preserved_across_editor', 'after-trips' in shown and 'hidden-while-editor' in shown, shown)
            state = ev('WB-CONSOLE?')['output'].split('\r\n')[1]
            r['console_state'] = state
            r['dropped_after'] = drops()
            check('no_console_drops', r['dropped_after'] == r['dropped_before'], (r['dropped_before'], r['dropped_after']))
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'round_trip_ms', 'screen_after_trip', 'error')})[:1500])


if __name__ == '__main__':
    main()
