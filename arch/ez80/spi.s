/* eZ80F92 SPI for the onboard SD socket. Preserve unrelated PB GPIO bits.
 * Pin setup follows MOS SPI by Leigh Brown; MIT notice: drivers/fatfs/MOS-LICENSE.
 * PB2 /SS stays high; PB4 is active-low SD CS. PB3/6/7 use SPI mode 5.
 * C byte result in A, arguments each occupy a 3-byte stack slot.
 */
.assume ADL = 1
.section .text
.global _ez80_spi_init, _ez80_spi_select, _ez80_spi_xfer, _ez80_spi_read
_ez80_spi_init:
    in0 a,(0xdb)
    and 0xd7
    out0 (0xdb),a
    in0 a,(0x9a)
    or 0x14
    out0 (0x9a),a
    in0 a,(0x9b)
    and 0xeb
    or 0xc8
    out0 (0x9b),a
    in0 a,(0x9c)
    and 0x23
    out0 (0x9c),a
    in0 a,(0x9d)
    and 0xeb
    or 0xc8
    out0 (0x9d),a
    xor a
    out0 (0xba),a
    out0 (0xb9),a
    ld hl,3
    add hl,sp
    ld a,(hl)
    or a
    ld a,32
    jr z,.Lspi_divisor
    ld a,3
.Lspi_divisor:
    out0 (0xb8),a
    ld a,0x30
    out0 (0xba),a
    ret
_ez80_spi_select:
    ld hl,3
    add hl,sp
    ld a,(hl)
    or a
    jr z,.Lspi_deselect
    in0 a,(0x9a)
    and 0xef
    out0 (0x9a),a
    ret
.Lspi_deselect:
    in0 a,(0x9a)
    or 0x10
    out0 (0x9a),a
    ret
_ez80_spi_xfer:
    ld hl,3
    add hl,sp
    ld a,(hl)
    out0 (0xbc),a
    ld bc,65535
.Lspi_poll:
    in0 a,(0xbb)
    ld e,a
    and 0x50
    jr nz,.Lspi_fault
    ld a,e
    and 0x80
    jr nz,.Lspi_received
    dec bc
    ld a,b
    or c
    jr nz,.Lspi_poll
.Lspi_fault:
    xor a
    ret
.Lspi_received:
    in0 a,(0xbc)
    ld e,a
    ld hl,6
    add hl,sp
    ld hl,(hl)
    ld (hl),e
    ld a,1
    ret
/* ez80_spi_read(buffer, count): clock count bytes in (sending 0xFF) to buffer.
 * A = 1, or 0 on an SPI fault or a byte that never completes. One call per
 * SD sector instead of one per byte: the per-byte C call cost ~8 ms a sector. */
_ez80_spi_read:
    ld hl,6
    add hl,sp
    ld bc,(hl)
    ld hl,3
    add hl,sp
    ld hl,(hl)
.Lspi_read_next:
    ld a,b
    or c
    jr z,.Lspi_read_done
    ld a,255
    out0 (0xbc),a
    push bc
    ld bc,65535
.Lspi_read_poll:
    in0 a,(0xbb)
    ld e,a
    and 0x50
    jr nz,.Lspi_read_fault
    ld a,e
    and 0x80
    jr nz,.Lspi_read_byte
    dec bc
    ld a,b
    or c
    jr nz,.Lspi_read_poll
.Lspi_read_fault:
    pop bc
    xor a
    ret
.Lspi_read_byte:
    pop bc
    in0 a,(0xbc)
    ld (hl),a
    inc hl
    dec bc
    jr .Lspi_read_next
.Lspi_read_done:
    ld a,1
    ret
