#!/usr/bin/env python3
"""Host-only GUI look mockups at the device's real geometry.

Renders 640x480 frames limited to one 16-entry palette of the VDP's RGB222
colours, using the Agon 8x8 font parsed from the vendored VDP source, so the
images show what mode 0 can actually display. Window client areas follow the
compositor's 8 px x snap. No hardware is touched.

    python3 tools/gui_mockup.py [--out docs/gui/mockups] [--scale 2]
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONT_SOURCE = ROOT / 'firmware/esp32-vios/vendor/agon-vdp/video/agon_fonts.h'
W, H = 640, 480


def load_font():
    text = FONT_SOURCE.read_text(encoding='latin-1')
    body = text[text.index('FONT_AGON_BITMAP[]'):]
    body = body[body.index('{') + 1:body.index('};')]
    data = [int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body)]
    assert len(data) % 8 == 0, 'font bitmap is not whole glyphs'
    # The bitmap starts at the space character.
    return {32 + i: data[i * 8:i * 8 + 8] for i in range(len(data) // 8)}


FONT = load_font()


def rgb222(v):
    return ((v >> 4) & 3) * 85, ((v >> 2) & 3) * 85, (v & 3) * 85


class Frame:
    """A 4 bpp indexed framebuffer: every pixel is a palette index 0..15."""

    def __init__(self, palette, shade):
        assert len(palette) == 16 and len(shade) == 16
        self.palette, self.shade = palette, shade
        self.px = bytearray(W * H)

    def fill(self, x, y, w, h, c):
        for yy in range(max(0, y), min(H, y + h)):
            row = yy * W
            for xx in range(max(0, x), min(W, x + w)):
                self.px[row + xx] = c

    def dither(self, x, y, w, h, a, b):
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                self.px[yy * W + xx] = a if (xx + yy) & 1 else b

    def rect(self, x, y, w, h, c, t=1):
        self.fill(x, y, w, t, c); self.fill(x, y + h - t, w, t, c)
        self.fill(x, y, t, h, c); self.fill(x + w - t, y, t, h, c)

    def darken(self, x, y, w, h):
        """Translucent shadow: remap indices through the shade table."""
        for yy in range(max(0, y), min(H, y + h)):
            for xx in range(max(0, x), min(W, x + w)):
                i = yy * W + xx
                self.px[i] = self.shade[self.px[i]]

    def text(self, x, y, s, fg, bg=None):
        for ch in s:
            glyph = FONT.get(ord(ch), FONT[ord('?')])
            for r, bits in enumerate(glyph):
                for c in range(8):
                    on = bits & (0x80 >> c)
                    if on or bg is not None:
                        xx, yy = x + c, y + r
                        if 0 <= xx < W and 0 <= yy < H:
                            self.px[yy * W + xx] = fg if on else bg
            x += 8
        return x

    def save(self, path, scale):
        from PIL import Image
        assert max(self.px) < 16
        img = Image.frombytes('P', (W, H), bytes(self.px))
        flat = []
        for v in self.palette:
            flat.extend(rgb222(v))
        img.putpalette(flat + [0] * (768 - len(flat)))
        if scale > 1:
            img = img.resize((W * scale, H * scale), Image.NEAREST)
        img.save(path, optimize=True)


def cells(n):
    return n * 8


FORTH = [
    '\\ WBM1P: workbench demo module',
    '',
    ': SQUARE ( n -- n*n )  DUP * ;',
    ': CUBE   ( n -- n^3 )  DUP SQUARE * ;',
    '',
    ': ROW    ( n -- )',
    '   DUP 4 .R  DUP SQUARE 6 .R',
    '   CUBE 8 .R  CR ;',
    '',
    ': TABLE  ( -- )',
    '   CR ."     n    n^2     n^3" CR',
    '   11 1 DO  I ROW  LOOP ;',
    '',
    ': DEMO   ( -- )  TABLE ;',
]
TRANSCRIPT = ['boot confirmed B', 'VIOS v0.8 / Forth 24 / Agon Light 2', '(c) 2026 Alessandro Iob',
              'vios> 3 4 + .  7  ok', 'vios> DEMO', '    n    n^2     n^3',
              '   1      1       1', '   2      4       8', '   3      9      27',
              '   4     16      64', 'ok']


# ---------------------------------------------------------------- Turbo Vision

TV_PAL = [0x00, 0x30, 0x3C, 0x3F,   # 0-3 pinned to today's mode 1 colours
          0x02, 0x06, 0x2A, 0x15,   # 4 window blue, 5 desktop blue, 6 light grey, 7 dark grey
          0x08, 0x0F, 0x0A, 0x28,   # 8 green, 9 cyan, 10 dark cyan, 11 dark yellow
          0x01, 0x2F, 0x20, 0x17]   # 12 navy, 13 pale cyan, 14 dark red, 15 slate
TV_SHADE = [0, 14, 11, 7, 12, 12, 7, 0, 0, 10, 12, 0, 0, 10, 0, 12]


def tv_window(f, cx, cy, cw, ch, title, active, lines, cursor=None):
    x, y, w, h = cells(cx), cells(cy), cells(cw), cells(ch)
    f.darken(x + w, y + 8, 16, h)            # drop shadow: 2 cells right, 1 cell down
    f.darken(x + 16, y + h, w, 8)
    f.fill(x, y, w, h, 4)
    frame = 3 if active else 6
    if active:
        f.rect(x + 2, y + 2, w - 4, h - 4, frame); f.rect(x + 5, y + 5, w - 10, h - 10, frame)
    else:
        f.rect(x + 3, y + 3, w - 6, h - 6, frame)
    label = f' {title} '
    tx = x + (w - len(label) * 8) // 2 // 8 * 8
    f.text(tx, y, label, 3 if active else 6, 4)
    if active:
        f.text(x + 16, y, '[', 3, 4); f.fill(x + 26, y + 2, 4, 4, 8); f.text(x + 32, y, ']', 3, 4)
        f.text(x + w - 40, y, '[', 3, 4); f.text(x + w - 32, y, '^', 8, 4)
        f.text(x + w - 24, y, ']', 3, 4)
        # Vertical and horizontal scrollbars on the frame.
        f.fill(x + w - 8, y + 8, 8, h - 16, 10); f.fill(x + w - 8, y + 24, 8, 8, 13)
        f.text(x + w - 8, y + 8, '^', 0, 9); f.text(x + w - 8, y + h - 16, 'v', 0, 9)
        f.fill(x + 96, y + h - 8, w - 112, 8, 10); f.fill(x + 120, y + h - 8, 8, 8, 13)
        f.text(x + 16, y + h - 8, ' 12:4 ', 3, 4)
    for i, line in enumerate(lines[:ch - 2]):
        f.text(x + 8, y + 8 + i * 8, line[:cw - 2], 2 if active else 9, None)
    if cursor:
        cxp, cyp = cursor
        f.fill(x + 8 + cxp * 8, y + 8 + cyp * 8 + 6, 8, 2, 3)


def tv_menu(f, cx, cy, items, sel):
    width = max(len(a) + len(b) for a, b in items if a) + 6
    x, y, w, h = cells(cx), cells(cy), cells(width), cells(len(items) + 2)
    f.darken(x + w, y + 8, 16, h); f.darken(x + 16, y + h, w, 8)
    f.fill(x, y, w, h, 6); f.rect(x + 3, y + 3, w - 6, h - 6, 0)
    for i, (label, key) in enumerate(items):
        yy = y + 8 + i * 8
        if not label:
            f.fill(x + 3, yy + 3, w - 6, 1, 0)
            continue
        bg = 8 if i == sel else 6
        fg = 3 if i == sel else 0
        f.fill(x + 8, yy, w - 16, 8, bg)
        f.text(x + 16, yy, label, fg, bg)
        f.text(x + 16, yy, label[0], 2 if i == sel else 1, bg)   # mnemonic
        f.text(x + w - 16 - len(key) * 8, yy, key, fg, bg)


def turbo_vision(out, scale):
    f = Frame(TV_PAL, TV_SHADE)
    f.dither(0, 8, W, H - 16, 5, 12)
    # Menu bar with mnemonics and the open File menu highlighted.
    f.fill(0, 0, W, 8, 6)
    x = 8
    for i, name in enumerate(['File', 'Edit', 'Search', 'Run', 'Module', 'Window', 'Help']):
        bg = 8 if i == 0 else 6
        f.fill(x, 0, (len(name) + 2) * 8, 8, bg)
        f.text(x + 8, 0, name, 3 if i == 0 else 0, bg)
        f.text(x + 8, 0, name[0], 2 if i == 0 else 1, bg)
        x += (len(name) + 2) * 8
    tv_window(f, 2, 17, 23, 26, 'Modules', False,
              ['', 'LIVE1I    11', 'LIVE1P    11', 'LIVE1D    11', 'WBM1P    1.0', '', '', '',
               '', '', '', '', '', '', '', '', '', '', '', '', 'Words: 146/512'])
    tv_window(f, 44, 37, 34, 20, 'Transcript', False, TRANSCRIPT)
    tv_window(f, 27, 5, 51, 30, 'WBM1P.FTH', True, FORTH, cursor=(3, 11))
    # Confirmation dialog over the other windows, with buttons and shadows.
    dx, dy, dw, dh = cells(17), cells(45), cells(34), cells(9)
    f.darken(dx + dw, dy + 8, 16, dh); f.darken(dx + 16, dy + dh, dw, 8)
    f.fill(dx, dy, dw, dh, 6); f.rect(dx + 2, dy + 2, dw - 4, dh - 4, 3); f.rect(dx + 5, dy + 5, dw - 10, dh - 10, 3)
    f.text(dx + (dw - 9 * 8) // 2 // 8 * 8, dy, ' Confirm ', 3, 6)
    f.text(dx + 16, dy + 16, 'Activate WBM1P revision 3?', 0, 6)
    for i, (lab, bx) in enumerate([(' Activate ', 4), ('  Cancel  ', 19)]):
        bxp, byp = dx + cells(bx), dy + cells(5)
        f.fill(bxp + 8, byp + 8, len(lab) * 8, 8, 0)           # button shadow
        f.fill(bxp, byp, len(lab) * 8, 8, 8 if i == 0 else 9)
        f.text(bxp, byp, lab, 3 if i == 0 else 0, 8 if i == 0 else 9)
        if i == 0:
            f.rect(bxp - 2, byp - 2, len(lab) * 8 + 4, 12, 2)     # focus ring
    tv_menu(f, 1, 1, [('New', 'Ctrl+N'), ('Open...', 'F3'), ('Save', 'F2'), ('Save as...', ''),
                      ('', ''), ('Stage', 'Ctrl+T'), ('Activate', 'Ctrl+A'), ('Revert', ''),
                      ('', ''), ('Exit', 'Alt+X')], sel=2)
    # Status line: key hints for the current context.
    f.fill(0, H - 8, W, 8, 6)
    x = 8
    for key, label in [('F1', 'Help'), ('F2', 'Save'), ('F3', 'Open'), ('Alt+F3', 'Close'),
                       ('F10', 'Menu'), ('Ctrl+Shift+P', 'Commands')]:
        x = f.text(x, H - 8, key, 1, 6); x = f.text(x + 8, H - 8, label, 0, 6) + 16
    f.save(out / 'turbo-vision.png', scale)


# ---------------------------------------------------------------- Smalltalk

ST_PAL = [0x00, 0x30, 0x3C, 0x3F,   # 0-3 pinned
          0x2A, 0x15, 0x3E, 0x2B,   # 4 light grey, 5 dark grey, 6 pale yellow, 7 pale blue
          0x17, 0x3A, 0x2E, 0x1B,   # 8 blue, 9 pale pink, 10 pale green, 11 mid blue
          0x05, 0x29, 0x39, 0x01]   # 12 slate, 13 warm grey, 14 rose, 15 navy
ST_SHADE = [0, 14, 13, 4, 5, 0, 13, 11, 15, 14, 13, 15, 0, 5, 1, 0]


def st_window(f, cx, cy, cw, ch, title, active, accent):
    x, y, w, h = cells(cx), cells(cy), cells(cw), cells(ch)
    f.darken(x + 4, y + h, w, 4); f.darken(x + w, y + 4, 4, h)      # soft pixel shadow
    f.fill(x, y, w, h, 3)
    f.rect(x, y, w, h, 0, 2 if active else 1)
    # Title tab sitting on the top-left edge, Smalltalk-80 style.
    tw = cells(len(title) + 2)
    f.fill(x, y - 10, tw, 11, accent if active else 4); f.rect(x, y - 10, tw, 11, 0)
    f.text(x + 8, y - 8, title, 0, None)
    return x, y, w, h


def st_list(f, x, y, w, h, items, sel, focused):
    f.fill(x, y, w, h, 3)
    for i, it in enumerate(items[:h // 9]):
        yy = y + 2 + i * 9
        if i == sel:
            f.fill(x + 1, yy - 1, w - 2, 10, 0 if focused else 4)
        f.text(x + 4, yy, it[:w // 8 - 1], 3 if (i == sel and focused) else 0, None)


def smalltalk(out, scale):
    f = Frame(ST_PAL, ST_SHADE)
    f.dither(0, 0, W, H, 4, 5)
    # Transcript, behind.
    x, y, w, h = st_window(f, 46, 44, 32, 14, 'Transcript', False, 10)
    for i, line in enumerate(TRANSCRIPT):
        f.text(x + 8, y + 6 + i * 9, line[:30], 0)
        if y + 6 + (i + 2) * 9 > y + h: break
    # Workspace, behind.
    x, y, w, h = st_window(f, 2, 45, 38, 12, 'Workspace', False, 10)
    for i, line in enumerate(['3 4 + .', "' SQUARE INSPECT", 'DEMO', 'WBM1P MODULE-SOURCE']):
        f.text(x + 8, y + 6 + i * 9, line, 0)
    # Module browser: four list panes over a code pane, the focus has a 2 px frame.
    x, y, w, h = st_window(f, 4, 5, 60, 36, 'Module Browser', True, 6)
    panes = [(['LIVE1I', 'LIVE1P', 'LIVE1D', 'WBM1P'], 3), (['all', 'arithmetic', 'tables', 'demo'], 2),
             (['ROW', 'TABLE', 'DEMO'], 1), (['source', 'callers', 'deps'], 0)]
    pw = (w - 4) // 4 // 8 * 8
    for i, (items, sel) in enumerate(panes):
        px = x + 2 + i * pw
        st_list(f, px, y + 2, pw - 1, cells(12), items, sel, i == 2)
        f.fill(px + pw - 1, y + 2, 1, cells(12), 0)
    f.fill(x + 2, y + 2 + cells(12), w - 4, 1, 0)
    code_y = y + 6 + cells(12)
    for i, line in enumerate(FORTH[5:13]):
        if i in (4, 5, 6):
            f.fill(x + 6, code_y + i * 9 - 1, cells(32), 10, 7)       # selection
        f.text(x + 8, code_y + i * 9, line, 0, None)
    f.fill(x + 8 + cells(6), code_y + 5 * 9 + 8, 8, 1, 0)                # caret
    # Inspector overlapping the browser's right edge.
    ix, iy, iw, ih = st_window(f, 58, 9, 19, 13, 'Inspector', False, 10)
    for i, (k, v) in enumerate([('word', 'TABLE'), ('module', 'WBM1P'), ('xt', '05A1C0'),
                                ('kind', 'colon'), ('bytes', '58'), ('callers', '1')]):
        f.text(ix + 6, iy + 6 + i * 9, k, 5); f.text(ix + 6 + cells(8), iy + 6 + i * 9, v, 0)
    # The classic pop-up menu ("yellow button" menu) with keyboard equivalents.
    items = [('do it', 'Ctrl+D'), ('print it', 'Ctrl+P'), ('inspect it', 'Ctrl+I'), ('', ''),
             ('accept', 'Ctrl+S'), ('cancel', 'Esc'), ('', ''), ('copy', 'Ctrl+C'), ('paste', 'Ctrl+V')]
    mx, my, mw = x + cells(30), code_y + 30, cells(21)
    mh = 6 + 10 * len(items)
    f.darken(mx + 4, my + mh, mw, 4); f.darken(mx + mw, my + 4, 4, mh)
    f.fill(mx, my, mw, mh, 3); f.rect(mx, my, mw, mh, 0)
    for i, (lab, key) in enumerate(items):
        yy = my + 3 + i * 10
        if not lab:
            f.fill(mx + 2, yy + 4, mw - 4, 1, 5)
            continue
        on = i == 1
        if on:
            f.fill(mx + 2, yy - 1, mw - 4, 10, 0)
        f.text(mx + 8, yy, lab, 3 if on else 0)
        f.text(mx + mw - 8 - len(key) * 8, yy, key, 4 if on else 5)
    f.save(out / 'smalltalk.png', scale)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--out', default=str(ROOT / 'docs/gui/mockups'))
    p.add_argument('--scale', type=int, default=2)
    a = p.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    turbo_vision(out, a.scale)
    smalltalk(out, a.scale)
    print('wrote', out / 'turbo-vision.png', out / 'smalltalk.png')


if __name__ == '__main__':
    main()
