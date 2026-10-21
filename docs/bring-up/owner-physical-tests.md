# Owner's physical tests

Three checks only a person at the board can run. The board should be on the
current firmware (main-53 or later, `tools/vios_firmware_ab.py --port … status`)
with a matching SD copy. Stop any harness first (`pkill -f vios_harness.py`),
and make sure no other session is using the board.

## 1. Typing on the physical keyboard (GUI toolkit Stage 2a)

Physical lines are edited on the ESP now; the CPU no longer has its own line
editor. At the `vios>` prompt, in both the GUI console window and the text
console (Ctrl+Alt+F1 switches between them):

| Try | Expect |
|---|---|
| Type `7 6 * .` then Enter | `42 ok`; the line appears once |
| Type `7 6`, Left Left, type `9 ` (insert), Enter | `9 7 6` is evaluated (prints nothing, leaves 3 numbers; `.S` or `. . .` shows them) |
| Home / End while editing | the cursor jumps to the start / end |
| Delete and Backspace mid-line | the character under / before the cursor goes |
| Esc while editing | the line clears |
| Up / Down after a few lines | the last 4 lines come back, newest first; Down past the newest gives an empty line |
| `: SLOW 3000 0 DO I DROP LOOP ;` then type while `SLOW` runs | the keys are not lost; they become the next line |
| `: SPIN BEGIN AGAIN ;` then `SPIN`, then Ctrl-C (`BEGIN` … `AGAIN` only works inside a definition) | the loop stops with an error and the prompt returns |
| Typing speed | feels as immediate as before |

Report anything that feels different from before, with the exact keys.

## 2. No boot SD card

1. Power off. Take the SD card out.
2. Power on and wait about 15 seconds.
3. Expect: the Console window says "VIOS: no boot SD card, or no viosboot on
   it. Insert the VIOS SD card and reset the board." If a terminal is on the
   USB port (`tools/serial_monitor.py --port /dev/cu.usbserial-210`), it shows
   `BOOT no stub request: insert the boot SD`.
4. Power off, put the card back, power on: VIOS boots normally.

## 3. Power cuts during SAVE and BOOT-TRIAL (Stage 7 item 1)

`tools/vios_power_cut.py` drives this. It boots the board, checks the boot
state, prints **UNPLUG NOW**, and repeats the write back to back until you pull
the USB-C cable (power and USB share it), so the cut lands inside a write.
Plug the cable back in when you like; the script waits for the board, checks
it again and starts the next round.

```bash
.venv/bin/python tools/vios_power_cut.py --mode save --rounds 5 --output docs/bring-up/vios-power-cut-save.json
```

```bash
.venv/bin/python tools/vios_power_cut.py --mode trial --rounds 5 --output docs/bring-up/vios-power-cut-trial.json
```

- **save** stages a new version of a test module `PCUT` and SAVEs its package
  each time. After every cut, all PCUT packages saved so far must load and pass
  their test, or be refused cleanly (a torn package must never run). It leaves
  a few small PCUT package files under `/VIOS` on the card.
- **trial** repeats `BOOT-TRIAL` with the modules you already have: boot
  bundles and boot records are written each time. After every cut the board
  must boot, with the confirmed slot readable and no trial left pending. This
  **changes which boot slot is confirmed** (both hold the same modules), so it
  needs your approval to run.

Each round prints the boot status and any problems; the run ends with PASSED or
FAILED and a JSON report. Vary the moment you unplug (right away, after a few
seconds). `--simulate 2` runs the same rounds without unplugging (a dry run;
save mode passed it on 2026-10-21).
