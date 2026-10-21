# VIOS user guide

VIOS boots into a Forth console on the Agon's VGA screen. Everything you can
do at the keyboard can also be done from the Mac (CLI or MCP tools), and the
console shows host commands as ordinary lines, so you can watch an LLM or
script work.

## At the keyboard

| Key | Console | Editor (workbench) |
| --- | --- | --- |
| F1 | switch to the editor | switch to the console |
| Enter | run the line | newline in the draft |
| Left/Right, Home/End | move in the line | move in the source |
| Backspace / Delete | delete before / under the cursor | same |
| Up/Down | recall the last 4 lines | move / select |
| Esc | clear the line | — |
| Tab, Ctrl+Space | — | switch focus, wide layout |
| F10 | — | close the editor (back to the console) |
| F5–F8 | — | stage, activate, save, … (see the workbench runbook) |

Typing is local until Enter; the line then runs whole, so it never mixes
with host commands. Output produced while the editor is shown is kept and
appears when you return to the console.

## Forth essentials

Standard words are available, including `DUP SWAP OVER ROT NIP TUCK`,
`+ - * / MOD /MOD */ ABS MIN MAX`, `= < > 0= 0< <> WITHIN`, `@ ! C@ C! +! ?`,
`IF ELSE THEN`, `BEGIN UNTIL AGAIN WHILE REPEAT`, `DO ?DO LOOP +LOOP I J LEAVE`,
`." S" TYPE EMIT . U. CR SPACE SPACES`, `VARIABLE CONSTANT CREATE ALLOT HERE , C,`,
`CHAR [CHAR] FILL MOVE ERASE`, `DEFER IS`, `' EXECUTE`. `WORDS` lists all,
`SEE name` shows a definition.

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
`SPAWN-CON` prints it. A word named `AUTORUN` in a boot module starts
automatically after boot. Tasks run while the console is idle or waiting.

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
word can serve a device of your own, Plan 9 style:

```forth
: COUNTER 10 0 DO I . LOOP ;
S" count" ' COUNTER DEVICE       \ reading /dev/count runs COUNTER
: SINK BEGIN KEY DUP -1 <> WHILE EMIT REPEAT DROP ;
S" sink" ' SINK DEVICE           \ writing /dev/sink feeds SINK's KEY
``` `HANDLES`
lists open handles and the last I/O error.

## Startup

At boot VIOS loads the confirmed source slot (your saved modules), then
`/VIOS/USER.FTH` if that file exists (put it there with `:put`), then starts
a word named `AUTORUN` if one is defined. A failing `USER.FTH` is reported
on the console and boot continues; delete or fix the file to change it.

## When something goes wrong

- `LAST-ERROR` shows the word, source token and callers of the last
  failure, with the stack as it was. From the Mac: `:why`.
- `TRACE word 50` single-steps a word, showing each token and the stack.
- `ABORT` clears the stacks; Ctrl+C at the console clears the line.
- A stuck task: `TASKS`, then `n TASK-KILL`.
- The host can always recover the board through the independent ZDI path
  (`:recover` in the CLI restarts it; unsaved RAM work is lost).

## From the Mac

```sh
.venv/bin/python tools/vios_harness.py --backend resident --tcp 127.0.0.1:8765
.venv/bin/python tools/vios_cli.py
```

CLI extras: `:help WORD`, Tab completion, `:snapshot FILE`, `:graph`,
`:why`, `:trace WORD N`, `:events` (what was typed on the Agon), `:ls`,
`:put LOCAL /PATH`, `:get /PATH [LOCAL]`, `:describe KIND ID`, `:resync`.
MCP clients get the same operations as tools (see the harness runbook).
