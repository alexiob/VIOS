# Owner's physical tests, 2026-10-21

Run by the owner at the board, guided step by step
([owner-physical-tests.md](owner-physical-tests.md)). Board: the v0.8.0
release (ESP main-59 = 0.8.0 plus the Down-arrow fix, SD resident from the
release package), boot slot B with HELLO's autorun retired.

## 1. Typing on the physical keyboard: passed

| Step | Result |
|---|---|
| `7 6 * .` | as expected |
| Insert with Left Left, `.S` | as expected |
| Home / End / Delete / Backspace mid-line | as expected |
| Esc clears the line | as expected |
| Up / Down history | **failed on 0.8.0**: Down submitted the line. The VDP sends Down as ASCII 10 and the ESP line editor took it for Enter before looking at the key code. Fixed in `0f7d762` (keys before ASCII; host test with the VDP's real arrow ASCII). **Passed** on main-59 |
| Typing while `SLOW` runs | as expected |
| Ctrl-C stops a loop | the doc's `BEGIN AGAIN` at the prompt correctly says "compile-only word"; with `: SPIN BEGIN AGAIN ;  SPIN`, Ctrl-C stops it and the prompt returns: **passed** |
| Text console (Ctrl+Alt+F1) | as expected |
| Typing speed | no complaint |

The HELLO demo's heartbeat interfered with typing; its autorun was retired
first ([vios-hello-autorun-retired-2026-10-21.md](vios-hello-autorun-retired-2026-10-21.md)).

## 2. No boot SD card: passed

With the card out, the screen showed MOS's own `could not find path` and
`No SD card present *`, then:

    VIOS: no boot SD card, or no viosboot on it.
    Insert the VIOS SD card and reset the board

The USB boot log of the same power-on (`owner-physical-tests-2026-10-21/no-sd/no-sd-02.json`,
captured with `tools/vios_coldboot_capture.py`):

    2.205 s  BOOT VIOS v0.8 (c) 2026 Alessandro Iob
    2.205 s  BOOT VIOS VDP 1.04 mux-v1 zdi-edge 1us
    2.264 s  BOOT user mode: no ZDI; MOS and its stub load the resident
    2.423 s  MOS: Agon Platform MOS Version 3.0.2 Arthur
    2.474 s  MOS: Could not find path
    12.314 s BOOT no stub request: insert the boot SD

(`no-sd-01` is a power blip during the replug with no data.) With the card
back in, VIOS booted normally.

## 3a. Power cuts during SAVE: passed

`tools/vios_power_cut.py --mode save --rounds 5`
([power-cut-save.json](owner-physical-tests-2026-10-21/power-cut-save.json)).
Each round staged, activated and SAVEd new versions of a test module PCUT
back to back until the owner pulled the USB-C cable; after replugging, the
board booted and every PCUT package saved so far was loaded and tested.

| Cut | Saves completed before it | Packages checked after it | Result |
|---|---|---|---|
| 1 | 21 | 21 | all loaded, tests passed |
| 2 | 5 | 26 | all loaded, tests passed |
| 3 | 9 | 35 | all loaded, tests passed |
| 4 | 5 | 40 | all loaded, tests passed |
| 5 | 4 (and any after the report's list) | 48 | all loaded, tests passed |

Every boot: `boot confirmed B candidate NONE state CLEAN`. No package was
torn or refused. The scripted check after cut 5 hit a port blip while the
cable was replugged (the device appeared and vanished); it was run by hand
with the script's own `verify()` over every package in `/VIOS/335FDFDB/`.
The PCUT packages stay on the card (48 small files).
