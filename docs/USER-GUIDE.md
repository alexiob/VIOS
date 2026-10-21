# VIOS user guide

A short quick-start. The complete reference (build, install, every word,
examples, troubleshooting) is the [VIOS manual](manual/VIOS-MANUAL.md).

VIOS boots into a GUI desktop (mode 0, 640×480, 16 colours) with a *Console*
window holding the Forth REPL. Everything you can do at the keyboard can also
be done from the Mac (CLI or MCP tools), and the console shows host commands
as ordinary lines, so you can watch an LLM or script work.

## At the keyboard

| Key | Console | Editor (text workbench) |
| --- | --- | --- |
| F1 | switch to the editor | switch to the console |
| Ctrl+Alt+F1 | GUI desktop ↔ full-screen text console | — |
| Enter | run the line | newline in the draft |
| Left/Right, Home/End | move in the line | move in the source |
| Backspace / Delete | delete before / under the cursor | same |
| Up/Down | recall the last 4 lines | move / select |
| Esc or Ctrl+C | clear the line | — |
| Tab, Ctrl+Space | — | switch focus, wide layout |
| F5 / F6 / F7 / F8 | — | stage and test / activate / save / undo |
| F10 | — | close the editor (back to the console) |

The line is edited on the ESP32 until Enter, so typing stays instant while
tasks run; the line then runs whole and never mixes with host commands.
While a command runs, **Ctrl+C** interrupts it (`error: interrupted`, normally
within 50 ms; needs the 1 kHz tick, which runs on a normal MOS boot) and
other keys become `KEY` input. Output produced while the editor is shown is
kept and appears when you return to the console. In the GUI, keys such as
F5, F6 and F10 belong to the window manager outside the editor; see the
manual's [Console and REPL](manual/VIOS-MANUAL.md#the-console-and-repl) and
[GUI Tools](manual/VIOS-MANUAL.md#the-gui-tools) chapters.

## Forth essentials

Standard words are available, including `DUP SWAP OVER ROT NIP TUCK`,
`+ - * / MOD /MOD */ ABS MIN MAX`, `= < > 0= 0< <> WITHIN`, `@ ! C@ C! +! ?`,
`IF ELSE THEN`, `BEGIN UNTIL AGAIN WHILE REPEAT`, `DO ?DO LOOP +LOOP I J LEAVE`,
`." S" TYPE EMIT . U. CR SPACE SPACES`, `VARIABLE CONSTANT CREATE ALLOT HERE , C,`,
`CHAR [CHAR] FILL MOVE ERASE`, `DEFER IS`, `' EXECUTE`. `WORDS` lists all,
`SEE name` shows a definition.

The compiler is extensible: `IMMEDIATE`, `POSTPONE`, `[ ]` with `LITERAL`,
`STATE`, `CREATE ... DOES>` with `>BODY`, `EVALUATE`, `FIND`, `COMPILE,` and
`MARKER`. For example `: CONST CREATE , DOES> @ ;  42 CONST ANSWER` or
`: UNLESS POSTPONE 0= POSTPONE IF ; IMMEDIATE`. See "Extending the compiler"
in the [manual](manual/VIOS-MANUAL.md#extending-the-compiler).

## Tasks and pipes

```forth
: TICK BEGIN 1000 MS ." tick" CR AGAIN ;
' TICK SPAWN-CON .      \ runs in the background, prints to the console
TASKS                   \ list tasks;  1 TASK-KILL  stops task 1
: SAY S" hello pipes" TYPE CR ;
: UPPER BEGIN KEY DUP -1 <> WHILE DUP 97 123 WITHIN IF 32 - THEN EMIT REPEAT DROP ;
' SAY ' UPPER PIPELINE 2DROP   \ prints HELLO PIPES
```

`SPAWN` keeps a task's output in a private buffer (`TASK-OUT id`);
`SPAWN-CON` prints it when the console is idle. A word named `AUTORUN`
(from a boot module or `USER.FTH`) starts automatically after boot. Tasks are
preempted by the board's 1 kHz timer tick, so a task that never calls `PAUSE`
cannot freeze the others or the console.

## Files

```forth
S" /VIOS/NOTE.TXT" 2 OPEN CONSTANT H      \ 2 = write, 1 = read
S" remember the milk" H WRITE-ALL .  H CLOSE .
DIR /VIOS                                  \ list a directory
LOAD /VIOS/APP.FTH                         \ run a source file
```

**Mac files:** while the harness runs, `/host/...` paths are files in
`~/VIOS-host` on the Mac (`--host-root` changes it). Edit on the Mac and run
on the Agon with `LOAD /host/app.fth`; `OPEN`, `READ`, `WRITE` work too.

Devices: `/dev/null`, `/dev/cons` (write), `/dev/tasks` (read). Any Forth
word can serve a device of your own, Plan 9 style; each `OPEN` starts the
word as a task on the other end of a pipe:

```forth
: COUNTER 10 0 DO I . LOOP ;
S" count" ' COUNTER DEVICE       \ reading /dev/count returns COUNTER's output
VARIABLE GOT
: TALLY BEGIN KEY DUP -1 <> WHILE DROP 1 GOT +! REPEAT DROP ;
S" tally" ' TALLY DEVICE         \ writing /dev/tally feeds TALLY's KEY
```

A write device's own `EMIT` output goes to its task's private output ring,
not to the console. `HANDLES` lists open handles and the last I/O error.

## Startup

At boot VIOS loads the confirmed boot bundle (your saved modules), then
`/VIOS/USER.FTH` if that file exists (put it there with `:put`), then starts
a word named `AUTORUN` if one is defined. A failing `USER.FTH` is reported
on the console and boot continues; delete or fix the file to change it.

## When something goes wrong

- `LAST-ERROR` shows the word, offset and callers of the last failure, with
  the stack as it was. From the Mac: `:why`.
- `TRACE word 50` single-steps a word, showing each step and the stack.
- Ctrl+C interrupts a running command; at the prompt Esc or Ctrl+C clears
  the line. `ABORT` clears the stacks and any unfinished definition.
- A stuck task: `TASKS`, then `n TASK-KILL`.
- Restart: reset button, power cycle, or restart the harness (opening the
  port resets the board). `:recover` in the CLI forces a restart; unsaved RAM
  work is lost. Without the ZDI wire (normal untethered boot) it restarts only
  the ESP, and the harness cannot reattach until a board reset, power cycle or
  harness restart. See the manual's
  [Recovery](manual/VIOS-MANUAL.md#recovery) section.

## From the Mac

```sh
ls /dev/cu.usbserial-*          # the suffix changes with the USB socket
.venv/bin/python tools/vios_harness.py --backend resident \
    --port /dev/cu.usbserial-210 --tcp 127.0.0.1:8765
.venv/bin/python tools/vios_cli.py
```

CLI extras: `:help WORD`, Tab completion, `:snapshot FILE`, `:graph`,
`:why`, `:trace WORD N`, `:events` (what was typed on the Agon), `:ls`,
`:put LOCAL /PATH`, `:get /PATH [LOCAL]`, `:describe KIND ID`, `:resync`.
MCP clients get the same operations as tools (see the manual's
[Host Harness and MCP Tools](manual/VIOS-MANUAL.md#the-host-harness-and-mcp-tools)
chapter).
