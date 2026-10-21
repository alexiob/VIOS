#!/usr/bin/env python3
"""On-board acceptance for G5 canvases (display lists and icons).

Runs through the harness (127.0.0.1:8765) on a board booted into the GUI:
  picture   a 320x200 canvas with 200 bars plus lines, ovals, discs, text,
            a stipple, built-in and uploaded icons, drawn from Forth; 64
            framebuffer pixels match a host replay of the same list (the
            real vds_canvas.c compiled here);
  clear     a transparent canvas of icons over the cells;
  edits     100 single-item moves (UI-NUDGE, 11 bytes each on the wire):
            seconds per edit, and the picture still matches afterwards;
  drag      a window slid over the canvas and over plain text (B10 variant):
            frame work and vblank start in both;
  pointer   a press on the canvas queues a CANVAS event with the item;
  health    no new protocol errors, frame budgets as vios_gui_accept.
Uses window 4 and 5, surfaces 4 and 3, canvases 0 and 1, bitmap 0; removes
them at the end (console 32/16, workbench 26/10 and tools 20-24 untouched).
"""
import argparse
import json
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path
import time
try:
    from .vios_cli import RPCClient
except ImportError:
    from vios_cli import RPCClient

ROOT = Path(__file__).resolve().parents[1]
VDS = ROOT / 'firmware/esp32-vios/lib/vios-vds/src'
WINDOW, SURFACE, OVER, OVER_SURFACE = 4, 4, 5, 3
WX, WY, CW, CH = 160, 120, 320, 200          # window client origin = canvas 0 origin on screen
BITMAP = [0x12, 0x3F, 0x45, 0x6F, 0xF7, 0x7F]  # bitmap 0: 3x3, transparent 15

RENDER_C = r'''
#include <stdio.h>
#include <string.h>
#include "vds_canvas.h"
static vds v; static uint8_t fb[480][320], *rows[480], font[2048];
static uint8_t mem[VDS_CANVAS_ITEMS * sizeof(vds_item) + 320 * 480], bm[2048];
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb"); if (!f || fread(font, 1, sizeof font, f) != sizeof font) return 2; fclose(f);
    for (int y = 0; y < 480; ++y) rows[y] = fb[y];
    vds_init(&v, rows, 640, 480, font); vds_canvas_icons(&v);
    char line[256]; int x, y;
    while (fgets(line, sizeof line, stdin)) {
        unsigned a[9]; char text[32] = "";
        if (line[0] == 'C' && sscanf(line + 1, "%u %u %u", &a[0], &a[1], &a[2]) == 3)
            vds_canvas_create(&v, 0, 0, 0, 0, (int16_t)a[0], (int16_t)a[1], (uint8_t)a[2], 1, mem);
        else if (line[0] == 'B') {
            int n = 0, off; unsigned w, h, tr; sscanf(line + 1, "%u %u %u%n", &w, &h, &tr, &off);
            for (char *p = line + 1 + off; sscanf(p, "%x%n", &a[0], &off) == 1; p += off) bm[n++] = (uint8_t)a[0];
            vds_bitmap_define(&v, 0, (uint8_t)w, (uint8_t)h, (uint8_t)tr, bm);
        } else if (line[0] == 'I') {
            int ia, ib, ic, id; unsigned item, kind, colour, extra;
            sscanf(line + 1, "%u %u %u %u %d %d %d %d %31s", &item, &kind, &colour, &extra, &ia, &ib, &ic, &id, text);
            vds_item it; memset(&it, 0, sizeof it);
            it.kind = (uint8_t)kind; it.colour = (uint8_t)colour; it.extra = (uint8_t)extra;
            it.a = (int16_t)ia; it.b = (int16_t)ib; it.c = (int16_t)ic; it.d = (int16_t)id;
            if (kind == VDS_DRAW_TEXT) { if (!strcmp(text, "-")) text[0] = 0; it.n = (uint8_t)strlen(text); memcpy(it.text, text, it.n); }
            if (!vds_canvas_set(&v, 0, (uint8_t)item, &it)) return 3;
        } else if (line[0] == 'S' && sscanf(line + 1, "%u %d %d", &a[0], &x, &y) == 3) {
            if (!vds_canvas_shift(&v, 0, (uint8_t)a[0], (int16_t)x, (int16_t)y)) return 4;
        } else if (line[0] == 'P' && sscanf(line + 1, "%d %d", &x, &y) == 2) {
            const vds_canvas *c = &v.canvases[0];
            vds_rect all = {0, 0, c->w, c->h};
            vds_canvas_replay(&v, 0, all, c->pixels);
            const uint8_t *row = c->pixels + (size_t)y * ((c->w + 1u) / 2);
            printf("%d\n", (x & 1) ? row[x >> 1] & 15 : row[x >> 1] >> 4);
        }
    }
    return 0;
}
'''


class HostCanvas:
    """The same display list replayed by the real library on the host."""
    def __init__(self, temp):
        sys.path.insert(0, str(ROOT / 'tests'))
        from test_esp_vds import agon_font
        self.font = Path(temp) / 'font.bin'
        self.font.write_bytes(agon_font())
        src, self.binary = Path(temp) / 'render.c', Path(temp) / 'render'
        src.write_text(RENDER_C)
        subprocess.run(['cc', '-std=c99', '-O1', '-I' + str(VDS), str(src), str(VDS / 'vds.c'), str(VDS / 'vds_canvas.c'),
                        '-o', str(self.binary)], check=True)
        self.script = []

    def pixels(self, points):
        lines = self.script + [f'P {x} {y}' for x, y in points]
        out = subprocess.run([str(self.binary), str(self.font)], input='\n'.join(lines) + '\n', capture_output=True, text=True, check=True)
        return [int(v) for v in out.stdout.split()]


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
        with tempfile.TemporaryDirectory() as temp, RPCClient(timeout=120) as c:
            host = HostCanvas(temp)
            c.initialize()
            c.tool('connect', {})

            def ev(source):
                out = c.tool('evaluate', {'source': source, 'timeout': 60})
                if not out['ok']:
                    raise AssertionError(f'{source!r}: {out["output"][-200:]}')
                return out['output']

            def result(source):
                lines = ev(source).split('\r\n')
                for i, line in enumerate(lines):
                    if line.strip() == source.strip():
                        rest = [l for l in lines[i + 1:] if not re.match(r'^[a-z][\w-]*: ', l)]
                        if rest:
                            return rest[0]
                raise AssertionError(f'no echo of {source!r}: {lines}')

            def screen(points, settle=0.4):
                time.sleep(settle)          # a few frames: rasterise, compose
                return c.tool('esp_gui', {'pixels': [[WX + x, WY + y] for x, y in points]})['pixels']

            def drain():
                events = []
                while True:
                    words = result('GC-NEXT').split()
                    if words == ['0']:
                        return events
                    events.append([int(w) for w in words])   # flag arg id kind (top of stack first)

            def errors():
                return int(result('UI-SYNC . . .').split()[0])

            sections = {}

            def section(name):
                st = c.tool('esp_stats', {'reset': True})
                sections[name] = {k: st.get(k) for k in ('frames', 'vblank_start_max', 'frame_work_max', 'frame_gui_max',
                                                         'present_frames', 'present_work_max', 'late_us', 'loop_gap_max')}
                sections[name]['protocol_errors'] = errors()      # running total: a step that adds one is named
                return sections[name]

            status = c.tool('esp_gui')['status']
            check('gui_on', status.startswith('GUI on'), status)
            if not c.tool('evaluate', {'source': "' GC-NEXT DROP", 'timeout': 20})['ok']:   # a second run reuses them
                ev(': GC-NEXT UI-NEXT IF -1 . . . . ELSE 0 . THEN ;')
                ev(': GC-BARS 200 0 DO  0 I 12 MOD 4 + UI-PEN  I  I 16 MOD 20 *  I 16 / 15 *  18 13 UI-BAR  LOOP ;')
                ev(': GC-EDITS 0 1 UI-PEN 100 0 DO  0 I 1 AND IF -1 ELSE 1 THEN 0 UI-NUDGE  LOOP ;')
                ev('CREATE GC-BM 6 ALLOT')
            drain()
            errors_before = errors()
            section('start')

            # The picture: window 4 on surface 4, canvas 0 filling it.
            ev(f'{SURFACE} 40 25 48 UI-SURFACE  S" text under the canvas" {SURFACE} 1 1 48 UI-TYPE')
            ev(f'{WINDOW} {SURFACE} {WX} {WY} 40 25 UI-VISIBLE UI-DECOR OR UI-SHADOW OR UI-WINDOW  S" Canvas" {WINDOW} UI-TITLE  {WINDOW} UI-FOCUS')
            t = time.perf_counter()
            ev(f'UI-BEGIN  0 {SURFACE} 0 0 {CW} {CH} 3 UI-CV-VISIBLE UI-CANVAS  GC-BARS  UI-COMMIT')
            host.script += [f'C {CW} {CH} 3'] + [f'I {i} 4 {4 + i % 12} 0 {i % 16 * 20} {i // 16 * 15} 18 13 -' for i in range(200)]
            extra = [  # item kind colour extra a b c d text (Forth word for it)
                (200, 2, 0, 0, 0, 0, 319, 199, '-', '0 0 UI-PEN 200 0 0 319 199 UI-LINE'),
                (201, 2, 1, 0, 0, 199, 319, 0, '-', '0 1 UI-PEN 201 0 199 319 0 UI-LINE'),
                (202, 6, 2, 0, 100, 60, 61, 41, '-', '0 2 UI-PEN 202 100 60 61 41 UI-DISC'),
                (203, 5, 0, 0, 98, 58, 65, 45, '-', '0 0 UI-PEN 203 98 58 65 45 UI-OVAL'),
                (204, 3, 1, 0, 4, 140, 90, 50, '-', '0 1 UI-PEN 204 4 140 90 50 UI-RECT'),
                (205, 9, 0, 3, 10, 150, 40, 30, '-', '0 0 UI-PEN 205 10 150 40 30 3 UI-STIPPLE'),
                (206, 7, 0, 0, 200, 150, 0, 0, 'G5canvas', '0 0 UI-PEN S" G5canvas" 206 200 150 UI-PRINT'),
                (207, 8, 0, 25, 280, 20, 0, 0, '-', '207 280 20 UI-ICON-FOLDER UI-ICON'),
                (208, 8, 0, 30, 260, 40, 0, 0, '-', '208 260 40 UI-ICON-ERROR UI-ICON'),
                (209, 8, 0, 0, 300, 180, 0, 0, '-', '209 300 180 0 UI-ICON'),
                (210, 1, 1, 0, 319, 0, 0, 0, '-', '0 1 UI-PEN 210 319 0 UI-DOT'),
            ]
            ev(f'0 3 3 15 UI-BITMAP  {" ".join(f"{b} GC-BM {i} + C!" for i, b in enumerate(BITMAP))}  GC-BM 6 0 0 UI-BITS')
            host.script.append('B 3 3 15 ' + ' '.join(f'{b:x}' for b in BITMAP))
            ev('UI-BEGIN')                  # source lines are at most 235 bytes: one item per line
            for e in extra:
                ev(e[9])
            ev('UI-COMMIT')
            host.script += [f'I {i} {k} {col} {x} {a_} {b_} {c_} {d_} {txt}' for i, k, col, x, a_, b_, c_, d_, txt, _ in extra]
            r['draw_s'] = round(time.perf_counter() - t, 3)
            rng = random.Random(5)
            points = [[rng.randrange(CW), rng.randrange(CH)] for _ in range(48)] + \
                     [[130, 80], [0, 0], [319, 0], [319, 199], [210, 152], [285, 27], [266, 47], [301, 181],
                      [11, 151], [12, 151], [4, 160], [93, 160], [9, 6], [29, 6], [309, 186], [150, 100]]
            got, want = screen(points), host.pixels(points)
            bad = [(pt, g, w) for pt, g, w in zip(points, got, want) if g != w]
            r['picture'] = {'points': len(points), 'mismatches': bad[:8]}
            check('picture_matches_host_replay', not bad, bad[:8])
            check('picture_not_blank', len(set(got)) >= 8, sorted(set(got)))
            r['draw_frames'] = section('draw')

            # Transparent icons over the cells: canvas 1 on the same surface, above canvas 0.
            ev(f'1 {SURFACE} 0 0 136 16 15 UI-CV-VISIBLE UI-CV-CLEAR OR UI-CANVAS  1 0 UI-PEN')
            ev(' '.join(f'{i} {i * 17} 0 {24 + i} UI-ICON' for i in range(8)))
            icons = screen([[17 * i + 8, 8] for i in range(8)])
            r['icons'] = icons
            check('icons_drawn', len(set(icons)) >= 2 and 15 not in icons, icons)
            ev('1 -1 UI-CANVAS-FLAGS')

            # Edits: 100 nudges of item 0, net zero; then one net move of item 1.
            c.tool('esp_stats', {'reset': True})
            t = time.perf_counter()
            ev('GC-EDITS')
            dt = time.perf_counter() - t
            ev('0 0 UI-PEN 1 3 2 UI-NUDGE')
            host.script.append('S 1 3 2')
            r['edits'] = {'count': 100, 'seconds': round(dt, 3), 'ms_per_edit': round(dt * 10, 2), 'bytes_per_edit': 4 + 7}
            got, want = screen(points), host.pixels(points)
            bad = [(pt, g, w) for pt, g, w in zip(points, got, want) if g != w]
            check('picture_after_edits', not bad, bad[:8])
            r['edit_frames'] = section('edits')

            # Drag a window over the canvas, then over plain text (canvas hidden): B10 variant.
            ev(f'{OVER_SURFACE} 20 6 48 UI-SURFACE  S" over" {OVER_SURFACE} 1 1 48 UI-TYPE')
            ev(f'{OVER} {OVER_SURFACE} 0 {WY + 60} 20 6 UI-VISIBLE UI-DECOR OR UI-SHADOW OR UI-WINDOW  {OVER} UI-RAISE')
            section('over_open')
            for name, flags in (('drag_over_canvas', 'UI-CV-VISIBLE'), ('drag_over_text', '0')):
                ev(f'0 {flags} UI-CANVAS-FLAGS')
                time.sleep(0.3)
                c.tool('esp_stats', {'reset': True})
                ev(f'UI-TWEEN-WINDOW {OVER} UI-X 0 480 1500 UI-LINEAR UI-ANIMATE')
                time.sleep(1.8)
                r[name] = section(name)
                ev(f'UI-TWEEN-WINDOW {OVER} UI-X 0 0 0 UI-LINEAR UI-ANIMATE')
                time.sleep(0.3)
            ev(f'{OVER} UI-CLOSE')
            ev('0 UI-CV-VISIBLE UI-CANVAS-FLAGS')

            # Pointer: a press on the canvas reports canvas 0 and the item there.
            drain()
            ev(f'{WX + 130} {WY + 80} 1 UI-POINTER  {WX + 130} {WY + 80} 0 UI-POINTER')
            events = drain()
            r['pointer_events'] = events
            canvas = [e for e in events if e[3] == 19]
            check('canvas_event', canvas and canvas[0][2] == 0 and canvas[0][1] == 130, events)

            # Clean up.
            ev(f'0 -1 UI-CANVAS-FLAGS  {WINDOW} UI-CLOSE  UI-CURSOR FALSE UI-SPRITE-SHOW')
            gone = c.tool('esp_gui', {'pixels': [[WX + 130, WY + 80]]})['pixels']
            r['after_close'] = gone
            new_errors = errors() - errors_before
            r['protocol_errors'] = new_errors
            check('no_protocol_errors', new_errors == 0, new_errors)
            r['sections'] = sections
            r['gui'] = c.tool('esp_gui')['status']
            worst = {k: max((s.get(k) or 0) for s in sections.values()) for k in ('frame_work_max', 'vblank_start_max', 'present_work_max')}
            r['frames'] = worst
            check('frame_budget', worst['frame_work_max'] <= 6000 and worst['vblank_start_max'] <= 1000, worst)
            check('present_budget', worst['present_work_max'] <= 15000, worst)
        r['status'] = 'passed'
    except BaseException as error:
        r.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        a.output.write_text(json.dumps(r, indent=2) + '\n')
        print(json.dumps({k: r.get(k) for k in ('status', 'error', 'frames', 'edits')}))


if __name__ == '__main__':
    main()
