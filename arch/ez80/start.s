/* MOS is only the bootstrap. No MOS calls after this entry. */
.assume ADL = 1
.section .init
.global _image_start, _start, _platform_restart, _platform_boot_source
.extern __stack_top, __bss_start, __bss_size, _kernel_main
_image_start:
    jp _start
    .space 0x40 - (. - _image_start), 0
    .ascii "MOS"
    .byte 0, 1
_start:
    di
    ld sp, __stack_top
    push hl                    /* the MOS boot stub's handoff (VIOS_BOOT_MAGIC) survives the BSS clear */
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
    pop hl
    ld (_platform_boot_source), hl
    call _kernel_main
_platform_restart:
    di
.ifdef RAM_RESTART
.Ldrain:
    in0 a, (0xC5)
    and 0x40
    jr z, .Ldrain
    ld hl, (_platform_boot_source)
    ld de, 0x56420B            /* VIOS_BOOT_MAGIC */
    or a
    sbc hl, de
    jr nz, .Lram
    /* Booted by the MOS stub (no ZDI to reload us): a real CPU reset through
     * the watchdog, so MOS, autoexec and viosboot load a verified image again.
     * WDT_CTL: enable, reset (not NMI), system clock, 2^18 cycles (~14 ms). */
    ld a, 0x83
    out0 (0x93), a
    ld a, 0xA5
    out0 (0x94), a
    ld a, 0x5A
    out0 (0x94), a
    ld bc, 0
    ld d, 64
.Lwatchdog:                    /* ~1 s, far past the period; then MOS's entry as a fallback */
    djnz .Lwatchdog
    dec c
    jr nz, .Lwatchdog
    dec d
    jr nz, .Lwatchdog
    jp 0
.Lram:
    jp _image_start
.else
    jp 0
.endif
.section .bss
_platform_boot_source:
    .space 3
