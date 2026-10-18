# Physical keyboard, 2026-10-18

Board on `/dev/cu.usbserial-224130`, firmware `firmware-echo-lasterror.bin`
(app0 sha256 prefix `c43bddde`), same PS/2-capable keyboard as 2026-10-16.

- User report: "keyboard not detected / not working". ZDI reads of the
  resident transport (`tools/vios_keyboard_probe.py`, evidence in
  `vios-keyboard-probe-2026-10-18.log`) showed key-down bytes arriving in the
  cooked local line (`fdrtdabc`, later `dadsascabc1 2 +`) and Enter handing
  the line to the REPL with no drops. Detection was fine.
- Cause of the perceived failure: the workbench was closed after the day's
  fresh boots/restores, so keys went to the bare REPL, whose console stream
  is USB-only (no VGA mirroring). The 2026-10-18 keyboard-isolation firmware
  also stopped per-key echo; fixed by live local-line echo (native test
  `echo`), verified on board (`local_echoed` 15 of 15).
- With the workbench reopened (`wb_open`), the user confirmed arrows,
  typing, Tab focus and Ctrl+Space zoom all work. Held-key repeat and
  behavior under concurrent CLI/rendering load were not separately observed.
- Follow-up chosen by the user: a VGA REPL console view.
