/* Diagnostic MOS program: enable headless console, then return to MOS. */
.assume ADL = 1
.section .text
.global _console_image
_console_image:
    jp .Lconsole
    .space 0x40 - (. - _console_image), 0
    .ascii "MOS"
    .byte 0, 1
.Lconsole:
    push bc
    push de
    push ix
    push iy
    ld a, 23
    rst 0x10
    xor a
    rst 0x10
    ld a, 254
    rst 0x10
    ld a, 1
    rst 0x10
    ld hl, .Lmessage
.Lnext:
    ld a, (hl)
    or a
    jr z, .Ldone
    push hl
    rst 0x10
    pop hl
    inc hl
    jr .Lnext
.Ldone:
    pop iy
    pop ix
    pop de
    pop bc
    ld hl, 0
    ret
.Lmessage:
    .asciz "\r\nserial-bootstrap ready\r\n"
