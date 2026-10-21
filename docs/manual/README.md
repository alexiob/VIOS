# The VIOS manual

[`VIOS-MANUAL.md`](VIOS-MANUAL.md) is the user manual: one Markdown file with
a table of contents, cross-links, a word reference and example programs. It
reads on GitHub as is, and `tools/manual/build_site.py` renders it as a static
site in the style of the VIOS GUI.

| Path | What it is |
| --- | --- |
| `VIOS-MANUAL.md` | The manual. Edit it directly, except between the `WORD REFERENCE` markers. |
| `words/*.toml` | Stack effect, section and description of every word, by area. The word reference is generated from these and from the sources. |
| `scenes/*.scene`, `scenes/*.inc` | Scripts for the screenshots and animations (keys, Forth, shots, frames). |
| `media/` | The rendered PNG screenshots and GIF animations (generated; committed). |

## Regenerating

```sh
make manual          # = python3 tools/vios_manual.py all
make manual-site     # HTML into build/site
make manual-preview  # http://127.0.0.1:8000
```

`tools/vios_manual.py` (host only, never touches the board):

- `check`: the word list from the sources (a host build of the resident
  dictionary via `tools/manual/vios_doc_host.c`, the words `forth/system.c`
  parses, the commands `kernel/*.c` parses, the board-only natives of
  `kernel/vios_irq.c`) must match `words/*.toml` exactly. A new word without a
  description, or a description of a removed word, fails.
- `words`: rewrites the word reference section of the manual.
- `examples`: runs every ` ```forth ` block on the host Forth, one fresh system
  per chapter, in order. A block that only makes sense on the board is marked
  with `<!-- nocheck -->` on the line before the fence.
- `media`: renders each scene with `tools/manual/vios_shots.c`, which links the
  real CPU-side GUI code with the real ESP display server. `shot NAME` writes
  `media/NAME.png`; a scene with `frame MS` lines also becomes
  `media/SCENE.gif`. The scene commands are listed at the top of
  `tools/manual/vios_shots.c`; `tools/vios_manual.py` adds `include NAME`,
  `frame MS`, `ktype TEXT` (typed on the keyboard, a frame per character)
  and `ctype TEXT` (typed at the console). A scene with a `runtime full` line
  runs instead on the whole resident runtime (`tools/manual/vios_doc_host.c`,
  with handles and `/dev` files) against the real display server; there only
  `eval` and `shot` lines count.

`tests/test_vios_manual.py` (part of `make test`) runs `check`, verifies the
word reference is current and runs the examples.

The site is served at https://vios.iob.dev; building, deploying and the
Cloudflare setup are in [HOSTING.md](HOSTING.md) (kept out of the site). The
workflow is `.github/workflows/manual.yml`.

The site uses the Agon 8x8 font (Damien Guard and Lennart Benschop, from the
vendored Agon VDP), converted to an outline web font; the font file says so.
