#ifndef VIOS_INTERNAL_UART_H
#define VIOS_INTERNAL_UART_H
/* Internal eZ80/ESP link only. USB recovery always remains at 115200. */
#ifndef VIOS_INTERNAL_BAUD
#define VIOS_INTERNAL_BAUD 1152000
#endif
#if VIOS_INTERNAL_BAUD != 115200 && VIOS_INTERNAL_BAUD != 384000 && VIOS_INTERNAL_BAUD != 1152000
#error Unsupported VIOS internal UART baud
#endif
#endif
