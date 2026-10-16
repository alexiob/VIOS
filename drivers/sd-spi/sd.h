#ifndef VIOS_SD_SPI_H
#define VIOS_SD_SPI_H
#include <stdint.h>
/* Native 32-bit block addresses: never narrow an LBA to a Forth cell. */
int sd_init(void);
uint32_t sd_sector_count(void);
int sd_read_sector(uint32_t sector, unsigned char *buffer);
int sd_write_sector(uint32_t sector, const unsigned char *buffer);
int sd_sync(void);
const char *sd_error(void);
#endif
