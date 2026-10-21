#!/usr/bin/env python3
"""Render docs/manual/VIOS-MANUAL.md as a static site styled like the VIOS GUI.

  python3 tools/manual/build_site.py [--out build/site]

One page per chapter (## heading), a home page from the title and contents,
cross-links rewritten between pages, the screenshots from docs/manual/media,
and the Agon 8x8 bitmap font (firmware/esp32-vios/vendor/agon-vdp) converted
into a web font. Needs the packages in tools/manual/requirements.txt.
The output is plain files: any static host (GitHub Pages, Cloudflare Pages,
Netlify...) serves it as is; `python3 -m http.server -d build/site` previews it.
"""
import argparse
import hashlib
import html
import re
import shutil
import struct
import sys
from pathlib import Path

import markdown
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

ROOT = Path(__file__).resolve().parents[2]
MANUAL = ROOT / 'docs/manual/VIOS-MANUAL.md'
MEDIA = ROOT / 'docs/manual/media'
ASSETS = Path(__file__).resolve().parent / 'site'
FONT_H = ROOT / 'firmware/esp32-vios/vendor/agon-vdp/video/agon_fonts.h'
TITLE = 'VIOS Manual'
# Asset URLs carry a content hash so browsers never mix old and new styles.
VERSION = hashlib.sha256(b''.join((ASSETS / f).read_bytes() for f in ('vios.css', 'vios.js'))).hexdigest()[:10]


# ---------------------------------------------------------------- font

def agon_glyphs():
    """{code: 8 row bytes} for the printable characters of the Agon font."""
    text = FONT_H.read_text(encoding='latin-1')
    body = text[text.index('FONT_AGON_BITMAP[]'):]
    body = body[body.index('{') + 1:body.index('};')]
    data = bytes(int(v, 16) for v in re.findall(r'0x([0-9a-fA-F]{2})', body))
    glyphs = {}
    for code in range(32, 256):
        if code == 127 or 128 <= code < 160:
            continue
        k = (code - 32) * 8
        if k + 8 <= len(data):
            glyphs[code] = data[k:k + 8]
    return glyphs


def build_font(path):
    """A TrueType/WOFF font with one square per lit pixel: 8 px = 1 em, so at
    16 px each font pixel is exactly 2 CSS pixels."""
    px, em = 128, 1024
    glyphs = agon_glyphs()
    order, cmap, outlines, metrics = ['.notdef'], {}, {}, {}
    for code, rows in glyphs.items():
        name = f'u{code:04X}'
        pen = TTGlyphPen(None)
        for y, bits in enumerate(rows):
            x = 0
            while x < 8:
                if bits & (0x80 >> x):
                    start = x
                    while x < 8 and bits & (0x80 >> x):
                        x += 1
                    top, bottom = (7 - y) * px - px, (7 - y) * px    # baseline under row 6
                    pen.moveTo((start * px, top))
                    pen.lineTo((start * px, bottom))
                    pen.lineTo((x * px, bottom))
                    pen.lineTo((x * px, top))
                    pen.closePath()
                else:
                    x += 1
        order.append(name)
        cmap[code] = name
        outlines[name] = pen.glyph()
        metrics[name] = (em, 0)
    outlines['.notdef'] = TTGlyphPen(None).glyph()
    metrics['.notdef'] = (em, 0)
    fb = FontBuilder(em, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(outlines)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=7 * px, descent=-px)
    fb.setupNameTable({'familyName': 'Agon8', 'styleName': 'Regular',
                       'copyright': 'Agon VDP font by Damien Guard and Lennart Benschop; converted to an outline '
                                    'font for the VIOS manual (altered version).'})
    fb.setupOS2(sTypoAscender=7 * px, sTypoDescender=-px, usWinAscent=7 * px, usWinDescent=px)
    fb.setupPost(isFixedPitch=1)
    fb.font.flavor = 'woff'
    fb.save(str(path))


def favicon():
    """The Agon 'V' as an SVG icon."""
    rows = agon_glyphs()[ord('V')]
    rects = ''.join(f'<rect x="{x}" y="{y}" width="1" height="1"/>'
                    for y, bits in enumerate(rows) for x in range(8) if bits & (0x80 >> x))
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="-1 -1 10 10" shape-rendering="crispEdges">'
            f'<rect x="-1" y="-1" width="10" height="10" fill="#ffffaa"/><g fill="#000">{rects}</g></svg>')


# ---------------------------------------------------------------- markdown

def gh_slug(value, separator='-'):
    """GitHub's heading anchors: lower case, punctuation dropped, spaces to dashes."""
    value = re.sub(r'<[^>]+>', '', html.unescape(value)).strip().lower()
    value = re.sub(r'[^\w\- ]', '', value)
    return value.replace(' ', separator)


def image_size(path):
    data = path.read_bytes()[:32]
    if data[:8] == b'\x89PNG\r\n\x1a\n':
        return struct.unpack('>II', data[16:24])
    if data[:4] == b'GIF8':
        return struct.unpack('<HH', data[6:10])
    return None


def figures(body):
    """A paragraph holding only an image becomes a framed figure with a caption."""
    def figure(m):
        alt, src = html.unescape(m.group(1)), m.group(2)
        size = image_size(MEDIA / Path(src).name) if src.startswith('media/') else None
        dims = f' width="{size[0]}" height="{size[1]}"' if size else ''
        cls = ' anim' if src.endswith('.gif') else ''
        return (f'<figure class="shot{cls}"><a href="{src}"><img src="{src}" alt="{html.escape(alt)}"{dims} '
                f'loading="lazy"></a><figcaption>{html.escape(alt)}</figcaption></figure>')
    return re.sub(r'<p><img alt="([^"]*)" src="([^"]+)" ?/?></p>', figure, body)


def split_pages(body):
    """[(id, title, html)]: the part before the first chapter is the home page."""
    parts = re.split(r'(?=<h2 id=")', body)
    pages = [('index', TITLE, parts[0])]
    for part in parts[1:]:
        m = re.match(r'<h2 id="([^"]+)">(.*?)</h2>', part, re.S)
        pages.append((m.group(1), re.sub(r'<[^>]+>', '', m.group(2)), part))
    return pages


def link_pages(pages):
    """Rewrite #anchor links to the page that holds the anchor."""
    owner = {}
    for pid, _, body in pages:
        for anchor in re.findall(r'\bid="([^"]+)"', body):
            owner.setdefault(anchor, pid)
    def fix(pid):
        def sub(m):
            anchor = m.group(1)
            target = owner.get(anchor)
            if target is None:
                print(f'warning: broken link #{anchor} on {pid}', file=sys.stderr)
                return m.group(0)
            if target == pid:
                return f'href="#{anchor}"'
            if anchor == target:                                   # a chapter: its page
                return f'href="{target}.html"'
            return f'href="{target}.html#{anchor}"'
        return sub
    return [(pid, title, re.sub(r'href="#([^"]+)"', fix(pid), body)) for pid, title, body in pages]


def subheads(body):
    return re.findall(r'<h3 id="([^"]+)">(.*?)</h3>', body, re.S)


# ---------------------------------------------------------------- pages

def page(pid, title, body, chapters, prev, nxt):
    nav = []
    for cid, ctitle, cbody in chapters:
        here = cid == pid
        nav.append(f'<li{" class=\"here\"" if here else ""}><a href="{cid}.html">{html.escape(ctitle)}</a>')
        if here and pid != 'word-reference':
            subs = subheads(cbody)
            if subs:
                nav.append('<ul>' + ''.join(f'<li><a href="#{sid}">{stitle}</a></li>' for sid, stitle in subs) + '</ul>')
        nav.append('</li>')
    pager = '<nav class="pager">'
    if prev:
        pager += f'<a class="button" rel="prev" href="{prev[0]}.html">[ &lt; {html.escape(prev[1])} ]</a>'
    if nxt:
        pager += f'<a class="button" rel="next" href="{nxt[0]}.html">[ {html.escape(nxt[1])} &gt; ]</a>'
    pager += '</nav>'
    heading = ''
    if pid != 'index':                       # the chapter heading becomes the window's title tab
        body = re.sub(r'^<h2 id="[^"]+">.*?</h2>', '', body, count=1, flags=re.S)
        heading = html.escape(title)
    else:
        heading = TITLE
    filt = ''
    if pid == 'word-reference':
        filt = ('<div class="filter"><label for="wf">Find a word:</label> <input id="wf" type="search" '
                'placeholder="e.g. DUP, UI-WINDOW, ui-text, task" autocomplete="off" spellcheck="false"></div>')
    page_title = TITLE if pid == 'index' else f'{title} - {TITLE}'
    return f'''<!doctype html>
<html lang="en" data-style="smalltalk">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(page_title)}</title>
<meta name="description" content="VIOS: a malleable Forth operating system for the Agon Light 2. Build, install, set up and use it.">
<link rel="icon" href="favicon.svg" type="image/svg+xml">
<link rel="stylesheet" href="vios.css?v={VERSION}">
<script>try{{var s=new URLSearchParams(location.search).get('style')||localStorage.getItem('vios-style');if(s==='turbo'||s==='smalltalk')document.documentElement.dataset.style=s}}catch(e){{}}</script>
</head>
<body class="page-{pid}">
<header class="menubar">
  <a class="logo" href="index.html"><span class="mn">V</span>IOS</a>
  <nav>
    <a href="index.html#contents"><span class="mn">C</span>ontents</a>
    <a href="the-forth-language.html"><span class="mn">F</span>orth</a>
    <a href="the-gui-tools.html"><span class="mn">T</span>ools</a>
    <a href="word-reference.html"><span class="mn">W</span>ords</a>
    <a href="example-programs.html"><span class="mn">E</span>xamples</a>
    <a href="VIOS-MANUAL.md"><span class="mn">M</span>arkdown</a>
    <button id="style" type="button" title="Switch between the Smalltalk and Turbo Vision styles"><span class="mn">S</span>tyle</button>
  </nav>
</header>
<div class="desktop">
  <aside class="window contents-window">
    <details id="toc" open>
      <summary class="tab">Contents</summary>
      <ol class="toc">{"".join(nav)}</ol>
    </details>
  </aside>
  <main class="window main-window" id="top">
    <div class="tab title">{heading}</div>
    {filt}
    <article>
{body}
    </article>
    {pager}
  </main>
</div>
<script src="vios.js?v={VERSION}"></script>
</body>
</html>
'''


def build(out):
    text = MANUAL.read_text()
    md = markdown.Markdown(extensions=['tables', 'fenced_code', 'toc', 'sane_lists', 'attr_list'],
                           extension_configs={'toc': {'slugify': gh_slug}})
    body = figures(md.convert(text))
    # Code blocks carry their language as a title tab (forth, sh, text...).
    body = re.sub(r'<pre><code class="language-([\w-]+)">', r'<pre data-lang="\1"><code class="language-\1">', body)
    pages = link_pages(split_pages(body))
    home, chapters = pages[0], [p for p in pages[1:] if p[0] != 'contents']
    contents = next((p for p in pages if p[0] == 'contents'), None)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    (out / 'fonts').mkdir()
    build_font(out / 'fonts/agon8.woff')
    (out / 'favicon.svg').write_text(favicon())
    for asset in ('vios.css', 'vios.js'):
        shutil.copy(ASSETS / asset, out / asset)
    shutil.copytree(MEDIA, out / 'media')
    shutil.copy(MANUAL, out / 'VIOS-MANUAL.md')
    (out / '.nojekyll').write_text('')
    home_body = home[2]
    if contents:
        home_body += contents[2].replace('<h2 id="contents">', '<h2 id="contents" class="inline">', 1)
    nav = [home] + chapters
    for k, (pid, title, body) in enumerate(nav):
        if pid == 'index':
            body = home_body
        prev = nav[k - 1] if k > 0 else None
        nxt = nav[k + 1] if k + 1 < len(nav) else None
        (out / f'{pid}.html').write_text(page(pid, title, body, nav, prev, nxt))
    missing = '<h2 id="lost">Page not found</h2><p>That page is not in the manual. Try the <a href="index.html">contents</a>.</p>'
    (out / '404.html').write_text(page('404', 'Page not found', missing, nav, None, None))
    print(f'{len(nav)} pages in {out}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default=str(ROOT / 'build/site'))
    build(Path(ap.parse_args().out).resolve())


if __name__ == '__main__':
    main()
