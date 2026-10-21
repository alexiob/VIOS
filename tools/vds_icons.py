"""Art for the built-in 16x16 canvas icons; prints the C array body for vds_canvas.c."""
ICONS = {
'DOCUMENT': """
..kkkkkkkkk.....
..kwwwwwwwkk....
..kwwwwwwwkwk...
..kwwwwwwwkkkk..
..kwkkkkkwwwwk..
..kwwwwwwwwwwk..
..kwkkkkkkkkwk..
..kwwwwwwwwwwk..
..kwkkkkkkkkwk..
..kwwwwwwwwwwk..
..kwkkkkkkkkwk..
..kwwwwwwwwwwk..
..kwkkkkkwwwwk..
..kwwwwwwwwwwk..
..kkkkkkkkkkkk..
................""",
'FOLDER': """
................
................
.kkkkk..........
kyyyyyk.........
kyyyyyykkkkkkkk.
kyyyyyyyyyyyyyk.
kkkkkkkkkkkkkkkk
kyyyyyyyyyyyyyyk
kyyyyyyyyyyyyyyk
kyyyyyyyyyyyyyyk
kyyyyyyyyyyyyyyk
kyyyyyyyyyyyyyyk
kyyyyyyyyyyyyyyk
kkkkkkkkkkkkkkkk
................
................""",
'WORD': """
................
.kkkkkkkkkkkkkk.
.kwwwwwwwwwwwwk.
.kwkkwwwwwwwwwk.
.kwkkwwkkkwwwwk.
.kwwwwwwwwwwwwk.
.kwwkkwkkkkkwwk.
.kwwwwwwwwwwwwk.
.kwwkkkkwkkwwwk.
.kwwwwwwwwwwwwk.
.kwwkkwkkkwkkwk.
.kwwwwwwwwwwwwk.
.kwkkwwwwwwwwwk.
.kwwwwwwwwwwwwk.
.kkkkkkkkkkkkkk.
................""",
'MODULE': """
................
......kkkk......
....kkwwwwkk....
..kkwwwwwwwwkk..
.kwwwwwwwwwwwwk.
.kkkwwwwwwwwkkk.
.kyykkwwwwkkrrk.
.kyyyykkkkrrrrk.
.kyyyyyykrrrrrk.
.kyyyyyykrrrrrk.
.kyyyyyykrrrrrk.
.kyyyyyykrrrrrk.
..kkyyyykrrrkk..
....kkyykrkk....
......kkkk......
................""",
'INFO': """
................
.....kkkkkk.....
...kkwwwwwwkk...
..kwwwwkkwwwwk..
.kwwwwwkkwwwwwk.
.kwwwwwwwwwwwwk.
kwwwwwkkkwwwwwwk
kwwwwwwkkwwwwwwk
kwwwwwwkkwwwwwwk
kwwwwwwkkwwwwwwk
.kwwwwwkkwwwwwk.
.kwwwwkkkkwwwwk.
..kwwwwwwwwwwk..
...kkwwwwwwkk...
.....kkkkkk.....
................""",
'WARNING': """
.......kk.......
......kyyk......
......kyyk......
.....kyyyyk.....
.....kykkyk.....
....kyykkyyk....
....kyykkyyk....
...kyyykkyyyk...
...kyyykkyyyk...
..kyyyykkyyyyk..
..kyyyyyyyyyyk..
.kyyyyykkyyyyyk.
.kyyyyykkyyyyyk.
kyyyyyyyyyyyyyyk
kkkkkkkkkkkkkkkk
................""",
'ERROR': """
................
.....kkkkkk.....
...kkrrrrrrkk...
..krrrrrrrrrrk..
.krrwrrrrrrwrrk.
.krwwwrrrrwwwrk.
krrrwwwrrwwwrrrk
krrrrwwwwwwrrrrk
krrrrrwwwwrrrrrk
krrrrwwwwwwrrrrk
krrrwwwrrwwwrrrk
.krwwwrrrrwwwrk.
.krrwrrrrrrwrrk.
..krrrrrrrrrrk..
...kkrrrrrrkk...
.....kkkkkk.....""",
'DISK': """
................
.kkkkkkkkkkkkkk.
.kkkwwwwwwwwkkk.
.kkkwwwwwwkwkkk.
.kkkwwwwwwkwkkk.
.kkkwwwwwwwwkkk.
.kkkkkkkkkkkkkk.
.kkkkkkkkkkkkkk.
.kkwwwwwwwwwwkk.
.kkwkkkkkkkkwkk.
.kkwwwwwwwwwwkk.
.kkwkkkkkkkkwkk.
.kkwwwwwwwwwwkk.
.kkwwwwwwwwwwkk.
.kkkkkkkkkkkkkk.
................""",
}
MAP = {'.': 15, 'k': 0, 'r': 1, 'y': 2, 'w': 3}
out = []
for name, art in ICONS.items():
    rows = art.strip('\n').split('\n')
    assert len(rows) == 16 and all(len(r) == 16 for r in rows), name
    data = []
    for r in rows:
        for i in range(0, 16, 2):
            data.append(MAP[r[i]] << 4 | MAP[r[i + 1]])
    lines = ['    {   /* %s */' % name.lower()]
    for i in range(0, 128, 16):
        lines.append('        ' + ', '.join('0x%02X' % b for b in data[i:i + 16]) + ',')
    lines.append('    },')
    out.append('\n'.join(lines))
print('\n'.join(out))
