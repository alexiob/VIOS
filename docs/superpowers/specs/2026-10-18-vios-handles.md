# VIOS handles, devices and pipes (Stage 3, slice 1)

Goal: one small, inspectable I/O contract that Forth code, background tasks
and tools share — files on SD, device endpoints and pipes between tasks — in
the Plan 9 spirit of "everything is a named stream", without claiming POSIX
or 9P compatibility.

## Contract

All primitives are non-blocking natives usable in background tasks
(`FORTH_NATIVE_TASK`); blocking is policy written in Forth on top of them.

| Word | Stack | Result |
| --- | --- | --- |
| `OPEN` | `( addr u mode -- h )` | mode 1 read, 2 write (create/truncate); `h` 1..8 or a negative error. |
| `CLOSE` | `( h -- r )` | 0 or negative error. |
| `READ` | `( addr u h -- n )` | n>0 bytes, 0 end of file, -1 would block, other negative error. Short reads are normal. |
| `WRITE` | `( addr u h -- n )` | n>=0 bytes accepted (short when a pipe is nearly full), -1 would block (pipe full), -2 broken pipe. |
| `PIPE` | `( -- hr hw )` | Read and write ends of a 128-byte ring, or `-n 0` on error. |

Forth-defined policy (in the base dictionary, replaceable): `READ-WAIT` and
`WRITE-ALL` loop with `PAUSE` on -1, so they block cooperatively inside tasks
and run other tasks while the foreground waits.

Error codes: -1 again, -2 broken pipe, -3 bad handle, -4 no free handle or
pipe, -5 not found/invalid path, -6 I/O error, -7 wrong direction.

## Namespace

Paths beginning `/dev/` name devices; everything else is the FAT32 volume
(8.3 names). Devices in this slice:

- `/dev/null` — read EOF, write discards.
- `/dev/cons` — write only; bytes go through the VM output path, so in a
  background task they land in that task's output ring and in the foreground
  on the console. Console *reading* stays with `KEY` in the foreground.
- `/dev/tasks` — read a text snapshot (taken at open) of the task table.

## Ownership and cleanup

Each handle records its owner: the foreground (0) or a task id. When a task
finishes, fails or is killed, its handles are closed: a write end closing
makes readers see EOF after the buffered bytes; a read end closing makes
writers get -2. Foreground handles stay open until `CLOSE`. `HANDLES` (outer
command and harness tool) lists every handle with kind, owner, direction,
byte counts and path, followed by the text of the last I/O error. There is no locking of a file opened twice; FatFs
`FF_FS_LOCK` is 0 and the second writer wins, as documented.

## Budget

8 handles, 4 pipes x 128 bytes, 4 FatFs `FIL` objects (`FF_FS_TINY`, shared
sector window), one 512-byte device snapshot buffer: about 1.3 KiB BSS.

## Agon results, 2026-10-18

[Acceptance record](../../bring-up/vios-handles-acceptance-2026-10-18.json):
SD file write/chunked read/remove through handles (1.3 s for 192 bytes
including host round trips), producer/consumer tasks moving exactly 10000
bytes through one 128-byte pipe with EOF on producer close (1.56 s wall,
bounded by host polling), `/dev/tasks` read, no leaked handles, no console
drops. Code size: CPU image 117731 -> 124716 bytes (+7 KB, mostly per-byte
VM memory access in `OPEN`/transfer; a bulk accessor is a later
optimization); BSS +1.5 KiB, end `0x9E48D`. The base dictionary still lacks
`>`; Forth code uses `0 SWAP <` until Stage 5 widens the core word set.

## Pipelines (slice 2, 2026-10-19)

`id h TASK-INPUT` / `id h TASK-OUTPUT` attach a handle as a task's stdin or
stdout and give it ownership. Redirected `EMIT`/`.`/`CR` fill the task's ring,
which drains into the handle between slices; reaching 112 of 128 bytes ends
the slice, and the scheduler skips a task whose output cannot drain
(backpressure, no blocking inside the VM). A background `KEY` with stdin
reads one byte; an empty pipe rewinds IP by one instruction (1 byte TOKEN/C,
3 bytes DIRECT) and yields, so the KEY simply retries; EOF gives -1. A task
that returns drains its ring before it is marked done; a closed reader turns
writes into `broken pipe`. `' a ' b PIPELINE ( -- id1 id2 )` wires a's
output into b's input with b printing to the console.

[Agon acceptance](../../bring-up/vios-pipeline-acceptance-2026-10-19.json):
upper-casing pipeline output on the console, 5000 bytes exact through one
128-byte pipe with no drops (662 B/s with the host polling `tasks` every
0.25 s; draining only between slices is the current limit), broken pipe
reported, no leaked handles, no console drops.
