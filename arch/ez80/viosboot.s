/* VIOS untethered boot stub: /mos/viosboot.bin, run by MOS from autoexec.txt
 * (docs/superpowers/specs/2026-10-19-vios-untethered-boot.md, U3). MIT.
 *
 * A MOS moslet at 0x0B0000. It loads the VIOS resident to 0x040000 and jumps
 * to it; on failure it prints why and returns to the MOS prompt.
 *
 *   viosboot          ESP image first, then the SD copy
 *   viosboot sd       SD copy first, then the ESP image
 *   viosboot force    ignore the failed-boot counter (also: "force sd")
 *
 * Sources:
 *   ESP: VDU 23,0,E8 (firmware/esp32-vios/include/vios_bootserve.h): a 16-byte
 *        manifest, then 192-byte blocks with a CRC each, requested BURST at a
 *        time. Interrupts are off and UART0 is polled; RTS is asserted only
 *        while a packet is read, so the ESP pauses while a block is checked
 *        and copied (the ESP honours the CPU's RTS). Other VDP packets
 *        (keyboard) are skipped by code.
 *   SD:  /vios/vios.man (the same 16-byte manifest) and /vios/vios.bin.
 * Either way the image must match the manifest's size and CRC-32 and carry the
 * ADL MOS header before it runs.
 *
 * Counter: /vios/boot.cnt (one byte) counts boots started; at BOOTS the stub
 * stays in MOS. The resident clears it once healthy (U5). No SD: no counter.
 *
 * MOS API used (rst.lil 0x08, function in A; to verify on hardware, U0):
 * 1 mos_load (HL name, DE address, BC max size), 2 mos_save (HL name,
 * DE address, BC size; it does not overwrite), 5 mos_del (HL name); A returns
 * 0 on success. rst.lil 0x10 prints A. Every register the stub needs after a
 * MOS call is reloaded from memory: nothing relies on MOS preserving them.
 * MOS passes the argument string in HL.
 */
.assume ADL = 1
/* Set with --defsym (Makefile): VIOS_BOOT_BURST blocks per request, 1 until
 * UART0 flow control is measured (U0 item 10); VIOS_BOOT_TIMEOUT x 65536 UART
 * polls per wait, ~43 ms each at 18.432 MHz (12: ~0.5 s). */
.equ IMAGE, 0x040000
.equ IMAGE_MAX, 0x06F000
.equ BLOCK, 192
.equ TRIES, 3
.equ BOOTS, 3
.equ PACKET_BOOT, 0x7B
.equ CHUNK, 0x8000             /* whole-image CRC in chunks below 64 KB */

.section .text
.global _viosboot
_viosboot:
    jp viosboot_entry
    .space 0x40 - (. - _viosboot), 0
    .ascii "MOS"
    .byte 0, 1
viosboot_entry:                
    push ix
    push iy
    ld (saved_sp), sp
    call parse_args
    call count_boot
    jp c, .Llimit
    ld a, (flag_sd)
    or a, a
    jp nz, .Lsd_first
    call load_esp
    jp nc, .Lrun
    ld hl, msg_esp
    call print
    call load_sd
    jp nc, .Lrun
    jp .Lfail
.Lsd_first:
    call load_sd
    jp nc, .Lrun
    ld hl, msg_sd
    call print
    call load_esp
    jp nc, .Lrun
.Lfail:
    ld hl, msg_fail
    call print
    jp .Lexit
.Llimit:
    ld hl, msg_limit
    call print
.Lexit:
    ei
    ld sp, (saved_sp)
    pop iy
    pop ix
    ld hl, 0
    ret
.Lrun:
    di
    jp IMAGE

/* Arguments: words "sd" and "force" in any order (first letter decides). */
parse_args:
    xor a, a
    ld (flag_sd), a
    ld (flag_force), a
.Larg:
    ld a, (hl)
    or a, a
    ret z
    cp a, 32
    jp z, .Larg_next
    cp a, 's'
    jr nz, 1f
    ld a, 1
    ld (flag_sd), a
    jp .Larg_word
1:
    cp a, 'f'
    jp nz, .Larg_word
    ld a, 1
    ld (flag_force), a
.Larg_word:                    /* skip the rest of the word */
    inc hl
    ld a, (hl)
    or a, a
    ret z
    cp a, 32
    jp nz, .Larg_word
.Larg_next:
    inc hl
    jp .Larg

/* Carry set: BOOTS boots started without the resident clearing the counter. */
count_boot:
    xor a, a
    ld (count), a
    ld hl, name_cnt
    ld de, count
    ld bc, 1
    ld a, 1
    rst.lil 0x08
    or a, a
    jr z, 1f
    xor a, a                   /* no counter file: none started */
    ld (count), a
1:
    ld a, (flag_force)
    or a, a
    jr z, 2f
    xor a, a
    ld (count), a
2:
    ld a, (count)
    cp a, BOOTS
    jr c, 3f
    scf
    ret
3:
    inc a
    ld (count), a
    ld hl, name_cnt
    ld a, 5
    rst.lil 0x08               /* mos_del first: mos_save does not overwrite (FA_CREATE_NEW) */
    ld hl, name_cnt
    ld de, count
    ld bc, 1
    ld a, 2
    rst.lil 0x08               /* best effort: a read-only or absent card has no counter */
    or a, a                    /* carry clear */
    ret

/* ---------------------------------------------------------------- ESP */

/* Carry set on failure (interrupts back on); clear with a verified image (DI). */
load_esp:
    di
    call flush                 /* stale bytes only; a quiet-line drain would cost a timeout */
    ld hl, req_manifest
    ld b, 4
    call send_bytes
    call read_packet
    jp c, .Lesp_fail
    ld a, (pkt_len)
    cp a, 16
    jp nz, .Lesp_fail
    call take_manifest
    jp c, .Lesp_fail
    ld hl, IMAGE
    ld (dest), hl
    ld hl, (man_size)
    ld (remain), hl
    ld hl, 0
    ld (index), hl
.Lburst:
    ld a, TRIES
    ld (tries), a
.Lrequest:
    /* count = min(BURST, blocks - index) */
    ld hl, (blocks)
    ld de, (index)
    or a, a
    sbc hl, de
    ld de, VIOS_BOOT_BURST
    or a, a
    sbc hl, de
    ld a, VIOS_BOOT_BURST
    jr nc, 1f
    add hl, de
    ld a, l
1:
    ld (left), a
    ld (req_count), a
    ld a, (index)
    ld (req_index), a
    ld a, (index+1)
    ld (req_index+1), a
    ld hl, req_blocks
    ld b, 7
    call send_bytes
.Lblock:
    call read_packet
    jp c, .Lretry
    call take_block
    jp c, .Lretry
    ld hl, (remain)
    ld de, 0
    or a, a
    sbc hl, de
    jp z, .Lesp_done
    ld a, (left)
    dec a
    ld (left), a
    jp nz, .Lblock
    jp .Lburst
.Lretry:
    call drain                 /* the rest of the burst, then ask again from index */
    ld a, (tries)
    dec a
    ld (tries), a
    jp nz, .Lrequest
.Lesp_fail:
    ei
    scf
    ret
.Lesp_done:
    call verify_image
    jp c, .Lesp_fail
    ret

/* Manifest in pkt: 'M' ver=1 block=192 size crc ... Carry set if refused. */
take_manifest:
    ld a, (pkt)
    cp a, 'M'
    jp nz, .Lrefuse
    ld a, (pkt+1)
    cp a, 1
    jp nz, .Lrefuse
    ld a, (pkt+2)
    cp a, BLOCK
    jp nz, .Lrefuse
    ld a, (pkt+6)              /* size < 16 MB */
    or a, a
    jp nz, .Lrefuse
    ld hl, (pkt+3)
    ld (man_size), hl
    ld de, 1                   /* 1 <= size <= IMAGE_MAX */
    or a, a
    sbc hl, de
    jp c, .Lrefuse
    ld de, IMAGE_MAX
    or a, a
    sbc hl, de
    jp nc, .Lrefuse
    ld hl, (pkt+7)
    ld (man_crc), hl
    ld a, (pkt+10)
    ld (man_crc+3), a
    /* blocks = ceil(size / BLOCK) */
    ld hl, (man_size)
    ld bc, 0
    ld de, BLOCK
1:
    inc bc
    or a, a
    sbc hl, de
    jr z, 2f
    jr nc, 1b
2:
    ld (blocks), bc
    or a, a
    ret
.Lrefuse:
    scf
    ret

/* Block packet in pkt: 'B' index data crc. Checks, copies to dest, advances. */
take_block:
    ld a, (pkt)
    cp a, 'B'
    jp nz, .Lblock_refuse
    ld a, (index)
    ld b, a
    ld a, (pkt+1)
    cp a, b
    jp nz, .Lblock_refuse
    ld a, (index+1)
    ld b, a
    ld a, (pkt+2)
    cp a, b
    jp nz, .Lblock_refuse
    /* n = min(BLOCK, remain); length must be n + 7 */
    ld hl, (remain)
    ld de, BLOCK
    or a, a
    sbc hl, de
    ld a, BLOCK
    jr nc, 1f
    ld a, (remain)
1:
    ld (n), a
    add a, 7
    ld b, a
    ld a, (pkt_len)
    cp a, b
    jp nz, .Lblock_refuse
    call crc_start
    ld ix, pkt+3
    ld iy, 0
    ld a, (n)
    ld iyl, a
    call crc_bytes             /* the CRC follows the data: IX points at it */
    call crc_check
    jp c, .Lblock_refuse
    ld hl, pkt+3
    ld de, (dest)
    ld bc, 0
    ld a, (n)
    ld c, a
    ldir
    ld (dest), de
    ld hl, (remain)
    ld de, 0
    ld a, (n)
    ld e, a
    or a, a
    sbc hl, de
    ld (remain), hl
    ld hl, (index)
    inc hl
    ld (index), hl
    or a, a
    ret
.Lblock_refuse:
    scf
    ret

/* Discard what is already in the RX FIFO (no waiting). */
flush:
    ld b, 0
1:
    in0 a, (0xC5)
    bit 0, a
    ret z
    in0 a, (0xC0)
    djnz 1b
    ret

/* Discard input until the line has been quiet for one timeout. */
drain:
    call read_packet
    jr nc, drain
    ret

/* Send B bytes from HL to the ESP (CTS and THR polled, bounded). */
send_bytes:
    push bc
    ld bc, 0
1:
    in0 a, (0xC6)
    bit 4, a
    jr z, 2f
    in0 a, (0xC5)
    bit 5, a
    jr nz, 3f
2:
    djnz 1b
    dec c
    jr nz, 1b                  /* ~65536 polls, then send regardless */
3:
    pop bc
    ld a, (hl)
    out0 (0xC0), a
    inc hl
    djnz send_bytes
    ret

/* Read packets until a boot packet arrives: pkt, pkt_len. Carry on timeout.
 * RTS is asserted only while reading. */
read_packet:
    in0 a, (0xC4)
    or a, 2
    out0 (0xC4), a
.Lheader:
    call getc
    jp c, .Lquiet
    bit 7, a
    jp z, .Lheader             /* resynchronise on the next packet header */
    and a, 0x7F
    ld (pkt_code), a
    call getc
    jp c, .Lquiet
    ld (pkt_len), a
    ld hl, pkt
    or a, a
    jr z, 2f
    ld d, a
1:
    call getc
    jp c, .Lquiet
    ld (hl), a
    inc hl
    dec d
    jr nz, 1b
2:
    ld a, (pkt_code)
    cp a, PACKET_BOOT
    jp nz, .Lheader
    in0 a, (0xC4)
    and a, 0xFD
    out0 (0xC4), a
    or a, a
    ret
.Lquiet:
    in0 a, (0xC4)
    and a, 0xFD
    out0 (0xC4), a
    scf
    ret

/* One byte in A, carry clear; carry set after VIOS_BOOT_TIMEOUT x 65536 polls. */
getc:
    in0 a, (0xC5)
    bit 0, a
    jr z, 1f
    in0 a, (0xC0)
    or a, a
    ret
1:
    ld a, VIOS_BOOT_TIMEOUT
    ld (wait), a
    ld bc, 0
2:
    in0 a, (0xC5)
    bit 0, a
    jr nz, 3f
    djnz 2b
    dec c
    jr nz, 2b
    ld a, (wait)
    dec a
    ld (wait), a
    jr nz, 2b
    scf
    ret
3:
    in0 a, (0xC0)
    or a, a
    ret

/* ---------------------------------------------------------------- SD */

load_sd:
    ei
    ld hl, name_man
    ld de, pkt
    ld bc, 16
    ld a, 1
    rst.lil 0x08
    or a, a
    jp nz, .Lsd_refuse
    call take_manifest
    ret c
    ld hl, name_bin
    ld de, IMAGE
    ld bc, IMAGE_MAX
    ld a, 1
    rst.lil 0x08
    or a, a
    jp nz, .Lsd_refuse
    /* fall through */

/* The image at IMAGE matches the manifest's CRC and has the MOS header. */
verify_image:
    call crc_start
    ld ix, IMAGE
    ld hl, (man_size)
    ld (remain), hl
1:
    ld hl, (remain)
    ld de, CHUNK
    or a, a
    sbc hl, de
    jr c, 2f
    ld (remain), hl
    ld iy, CHUNK
    call crc_bytes
    ld hl, (remain)
    ld de, 0
    or a, a
    sbc hl, de
    jr nz, 1b
    jr 3f
2:
    ld iy, (remain)
    call crc_bytes
3:
    ld ix, man_crc
    call crc_check
    ret c
    ld hl, IMAGE+64
    ld de, mos_header
    ld b, 5
4:
    ld a, (de)
    cp a, (hl)
    jp nz, .Lsd_refuse
    inc hl
    inc de
    djnz 4b
    or a, a
    ret
.Lsd_refuse:
    scf
    ret

/* ---------------------------------------------------------------- CRC-32 */

crc_start:
    ld a, 0xFF
    ld (crc), a
    ld (crc+1), a
    ld (crc+2), a
    ld (crc+3), a
    ret

/* Update crc with IY (1..65535) bytes at IX; IX ends past them. */
crc_bytes:
    ld a, (crc)
    ld e, a
    ld a, (crc+1)
    ld d, a
    ld a, (crc+2)
    ld c, a
    ld a, (crc+3)
    ld b, a
    ld hl, crc_planes
1:
    ld a, (ix+0)
    xor a, e
    ld l, a
    ld a, (hl)
    xor a, d
    ld e, a
    inc h
    ld a, (hl)
    xor a, c
    ld d, a
    inc h
    ld a, (hl)
    xor a, b
    ld c, a
    inc h
    ld b, (hl)
    dec h
    dec h
    dec h
    inc ix
    dec iy
    ld a, iyl
    or a, iyh
    jr nz, 1b
    ld a, e
    ld (crc), a
    ld a, d
    ld (crc+1), a
    ld a, c
    ld (crc+2), a
    ld a, b
    ld (crc+3), a
    ret

/* Carry set unless ~crc equals the 4 little-endian bytes at IX. */
crc_check:
    ld hl, crc
    ld b, 4
1:
    ld a, (hl)
    cpl
    cp a, (ix+0)
    jr nz, 2f
    inc hl
    inc ix
    djnz 1b
    or a, a
    ret
2:
    scf
    ret

/* ---------------------------------------------------------------- output */

print:
    ld a, (hl)
    or a, a
    ret z
    rst.lil 0x10
    inc hl
    jr print

/* ---------------------------------------------------------------- data */

req_manifest: .byte 23, 0, 0xE8, 0
req_blocks:   .byte 23, 0, 0xE8, 1
req_index:    .byte 0, 0
req_count:    .byte 0
mos_header:   .ascii "MOS"
              .byte 0, 1
name_cnt:     .asciz "/vios/boot.cnt"
name_man:     .asciz "/vios/vios.man"
name_bin:     .asciz "/vios/vios.bin"
msg_esp:      .asciz "VIOSBOOT: no ESP image, trying /vios/vios.bin\r\n"
msg_sd:       .asciz "VIOSBOOT: no SD image, trying the ESP\r\n"
msg_fail:     .asciz "VIOSBOOT: no bootable VIOS image; staying in MOS\r\n"
msg_limit:    .asciz "VIOSBOOT: 3 boots did not finish; staying in MOS (viosboot force)\r\n"

saved_sp:     .space 3
flag_sd:      .space 1
flag_force:   .space 1
count:        .space 1
wait:         .space 1
tries:        .space 1
left:         .space 1
n:            .space 1
pkt_code:     .space 1
pkt_len:      .space 1
man_size:     .space 3
man_crc:      .space 4
blocks:       .space 3
index:        .space 3
dest:         .space 3
remain:       .space 3
crc:          .space 4
pkt:          .space 256
    .balign 256
crc_planes:
    .include "viosboot_crc.inc"
