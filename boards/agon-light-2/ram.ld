ENTRY(_image_start)
MEMORY { RAM (rwx) : ORIGIN = 0x040000, LENGTH = 0x070000 }
SECTIONS {
    .init : { KEEP(*(.init)) } > RAM
    .text : { *(.text .text.*) } > RAM
    .rodata : { *(.rodata .rodata.*) } > RAM
    .data : { *(.data .data.*) } > RAM
    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.* COMMON)
        __bss_end = .;
    } > RAM
    /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
__bss_size = __bss_end - __bss_start;
__stack_top = 0x0B0000;
__stack_bottom = __stack_top - 0x1000;
ASSERT(__bss_end <= __stack_bottom, "payload overlaps the 4 KiB stack")
ASSERT(__bss_size <= LENGTH(RAM) - 0x1000, "BSS exceeds RAM below the reserved stack")
