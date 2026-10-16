/* Same RAM placement used by Espressif esptool's ESP32 RAM flasher stub. */
MEMORY {
 iram : ORIGIN = 0x400BE000, LENGTH = 0x2000
 dram : ORIGIN = 0x3ffcc000, LENGTH = 0x14000
}
ENTRY(stub_main)
SECTIONS {
 .text : ALIGN(4) { *(.literal .literal.*) *(.text .text.*) } > iram
 .bss (NOLOAD) : ALIGN(4) {
  _bss_start = ABSOLUTE(.); *(.bss .bss.* COMMON)
  . = ALIGN(4); _bss_end = ABSOLUTE(.);
 } > dram
 .data : ALIGN(4) { *(.data .data.*) *(.rodata .rodata.*) } > dram
 /DISCARD/ : { *(.comment .note* .eh_frame*) }
}
PROVIDE(ets_set_user_start = 0x4000687c);
PROVIDE(ets_delay_us = 0x40008534);
PROVIDE(gpio_pad_select_gpio = 0x40009fdc);
PROVIDE(gpio_matrix_in = 0x40009edc);
PROVIDE(gpio_matrix_out = 0x40009f0c);
