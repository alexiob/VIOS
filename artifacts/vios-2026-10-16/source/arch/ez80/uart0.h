#ifndef EZ80_UART0_H
#define EZ80_UART0_H
unsigned char ez80_uart0_status(void);
unsigned char ez80_uart0_modem(void);
unsigned char ez80_uart0_read(void);
void ez80_uart0_write(unsigned char c);
void ez80_uart0_polling(void);
#endif
