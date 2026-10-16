/* VIOS FatFs block adapter. The card owns the entire physical device;
 * FatFs discovers its MBR partition. No formatting or partition-writing API.
 */
#include "ff.h"
#include "diskio.h"
#include "drivers/sd-spi/sd.h"
static DSTATUS state=STA_NOINIT;
DSTATUS disk_status(BYTE drive) {return drive?STA_NOINIT:state;}
DSTATUS disk_initialize(BYTE drive) {
 if(drive)return STA_NOINIT;
 state=sd_init()?0:STA_NOINIT;return state;
}
static int range(LBA_t sector, UINT count) {
 uint32_t total=sd_sector_count();return count&&sector<total&&count<=total-sector;
}
DRESULT disk_read(BYTE drive,BYTE *buffer,LBA_t sector,UINT count) {
 if(drive||!buffer||!range(sector,count))return RES_PARERR;
 if(state)return RES_NOTRDY;
 while(count--) {if(!sd_read_sector(sector++,buffer)){state=STA_NOINIT;return RES_ERROR;}buffer+=512;}
 return RES_OK;
}
DRESULT disk_write(BYTE drive,const BYTE *buffer,LBA_t sector,UINT count) {
 if(drive||!buffer||!range(sector,count))return RES_PARERR;
 if(state)return RES_NOTRDY;
 while(count--) {if(!sd_write_sector(sector++,buffer)){state=STA_NOINIT;return RES_ERROR;}buffer+=512;}
 return RES_OK;
}
DRESULT disk_ioctl(BYTE drive,BYTE command,void *buffer) {
 if(drive)return RES_PARERR;
 if(state)return RES_NOTRDY;
 if(command==CTRL_SYNC) {
  if(sd_sync())return RES_OK;
  state=STA_NOINIT;return RES_ERROR;
 }
 if(!buffer)return RES_PARERR;
 if(command==GET_SECTOR_COUNT){*(LBA_t *)buffer=sd_sector_count();return RES_OK;}
 if(command==GET_SECTOR_SIZE){*(WORD *)buffer=512;return RES_OK;}
 if(command==GET_BLOCK_SIZE){*(DWORD *)buffer=1;return RES_OK;}
 return RES_PARERR;
}
