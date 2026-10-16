#ifndef EZ80_UART0_H
#define EZ80_UART0_H
unsigned char ez80_uart0_status(void);
unsigned char ez80_uart0_modem(void);
unsigned char ez80_uart0_read(void);
void ez80_uart0_write(unsigned char c);
/* At most 32 bytes; stops immediately when CTS or THRE is inactive. */
unsigned ez80_uart0_tx_burst(const unsigned char *buffer, unsigned length);
void ez80_uart0_polling(void);
void ez80_uart0_rx_pause(void);
void ez80_uart0_rx_resume(void);
/* Pause RTS and collect at most 32 bytes before the C parser runs. */
unsigned ez80_uart0_rx_drain(unsigned char *buffer);
/* LSR overrun observations (LSR reads clear the hardware indication). */
extern volatile unsigned ez80_uart0_rx_overruns;
#endif
