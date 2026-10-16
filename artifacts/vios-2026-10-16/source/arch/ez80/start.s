/* MOS is only the bootstrap. No MOS calls after this entry. */
.assume ADL = 1
.section .init
.global _image_start, _start, _platform_restart
.extern __stack_top, __bss_start, __bss_size, _kernel_main
_image_start:
    jp _start
    .space 0x40 - (. - _image_start), 0
    .ascii "MOS"
    .byte 0, 1
_start:
    di
    ld sp, __stack_top
    /* SBC checks the whole 24-bit size. BC=0 would make LDIR copy 16 MiB. */
    ld hl, __bss_size
    ld de, 1
    or a
    sbc hl, de
    jr c, .Lready
    push hl
    pop bc
    ld hl, __bss_start
    ld (hl), 0
    /* LD/PUSH/POP preserve the size-minus-one Z flag from SBC. */
    jr z, .Lready
    ld de, __bss_start + 1
    ldir
.Lready:
    call _kernel_main
_platform_restart:
    di
.ifdef RAM_RESTART
.Ldrain:
    in0 a, (0xC5)
    and 0x40
    jr z, .Ldrain
    jp _image_start
.else
    jp 0
.endif
