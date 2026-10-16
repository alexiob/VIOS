#ifndef VIOS_SD_SPI_PORT_H
#define VIOS_SD_SPI_PORT_H
/* Architecture boundary: byte result in A, 3-byte C stack slots on eZ80. */
void ez80_spi_init(unsigned char fast);
void ez80_spi_select(unsigned char active);
unsigned char ez80_spi_xfer(unsigned char transmit, unsigned char *receive);
#endif
