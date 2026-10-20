/* Execute the real SD protocol driver against a byte-level card model. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "drivers/sd-spi/sd.h"
#include "drivers/sd-spi/spi.h"
static unsigned char queue[2048],command[6],sector_data[512];
static unsigned qread,qwrite,used,selected,fast,write_left,write_pos,busy_active;
static int init_stuck,read_stuck,busy_stuck,transfer_failed,write_rejected,corrupt_crc;
static uint32_t last_address;static unsigned commands,slow_commands,timer_reads;static unsigned short clock_count;
static unsigned short crc16(unsigned short c,unsigned char b){c^=(unsigned short)b<<8;for(unsigned i=0;i<8;i++)c=(unsigned short)((c<<1)^((c&0x8000)?0x1021:0));return c;}
static void put(unsigned char b){assert(qwrite<sizeof queue);queue[qwrite++]=b;}
void ez80_spi_init(unsigned char value){fast=value;if(!value){qread=qwrite=used=selected=write_left=busy_active=0;}}
void ez80_spi_select(unsigned char active){selected=active;if(!active){used=0;qread=qwrite=0;}}
unsigned short platform_timer_read(void){++timer_reads;clock_count=(unsigned short)(clock_count-1000);return clock_count;}
unsigned char ez80_spi_xfer(unsigned char tx,unsigned char *rx);
unsigned char ez80_spi_read(unsigned char *rx,unsigned count){for(unsigned i=0;i<count;i++)if(!ez80_spi_xfer(255,&rx[i]))return 0;return 1;}
unsigned char ez80_spi_xfer(unsigned char tx,unsigned char *rx){
 if(transfer_failed)return 0;
 if(!selected){*rx=255;return 1;}
 if(qread<qwrite){*rx=queue[qread++];if(qread==qwrite)qread=qwrite=0;return 1;}
 if(write_left){
  if(write_left==515){if(tx==0xfe){--write_left;write_pos=0;}*rx=255;return 1;}
  if(write_left>2)sector_data[write_pos++]=tx;
  --write_left;if(!write_left){busy_active=1;put(write_rejected?0x0b:0x05);put(0);put(0);if(!busy_stuck)put(255);}
  *rx=255;return 1;
 }
 if(busy_stuck&&busy_active){*rx=0;return 1;}
 if(!used&&(tx&0xc0)!=0x40){*rx=255;return 1;}
 command[used++]=tx;*rx=255;
 if(used<6)return 1;
 used=0;++commands;if(!fast)++slow_commands;
 last_address=((uint32_t)command[1]<<24)|((uint32_t)command[2]<<16)|((uint32_t)command[3]<<8)|command[4];
 switch(command[0]&63){
 case 0:assert(command[5]==0x95);put(1);break;
 case 8:assert(last_address==0x1aa&&command[5]==0x87);put(1);put(0);put(0);put(1);put(0xaa);break;
 case 55:put(1);break;
 case 41:assert(last_address==0x40000000UL);put(init_stuck?1:0);break;
 case 58:put(0);put(0xc0);put(0xff);put(0x80);put(0);break;
 case 9:{unsigned char csd[16]={0x40};csd[7]=0x01;csd[8]=0xff;csd[9]=0xff;unsigned short crc=0;put(0);put(0xfe);for(unsigned i=0;i<16;i++){put(csd[i]);crc=crc16(crc,csd[i]);}put((unsigned char)(crc>>8));put((unsigned char)crc);break;}
 case 17:put(0);if(!read_stuck){unsigned short crc=0;put(0xfe);for(unsigned i=0;i<512;i++){put(sector_data[i]);crc=crc16(crc,sector_data[i]);}put((unsigned char)(crc>>8));put((unsigned char)(crc^corrupt_crc));}break;
 case 24:put(0);write_left=515;break;
 default:assert(!"unexpected command");
 }
 return 1;
}
int main(void){
 unsigned char buffer[512];unsigned short known_crc=0;
 for(unsigned i=0;i<9;i++)known_crc=crc16(known_crc,(unsigned char)"123456789"[i]);assert(known_crc==0x31c3);
 assert(sd_init());assert(fast&&slow_commands>=5);assert(sd_sector_count()==134217728UL);
 for(unsigned i=0;i<512;i++)sector_data[i]=(unsigned char)(i*3u);
 assert(sd_read_sector(0x01234567UL,buffer));assert(last_address==0x01234567UL&&!memcmp(buffer,sector_data,512));
 for(unsigned i=0;i<512;i++)buffer[i]=(unsigned char)(i*7u);
 assert(sd_write_sector(0x02345678UL,buffer));assert(last_address==0x02345678UL&&!memcmp(buffer,sector_data,512));
 assert(sd_sync());corrupt_crc=1;assert(!sd_read_sector(1,buffer));corrupt_crc=0;assert(sd_init());assert(!sd_read_sector(sd_sector_count(),buffer));assert(!sd_write_sector(0,NULL));
 read_stuck=1;assert(!sd_read_sector(1,buffer));assert(sd_error()[0]);read_stuck=0;assert(sd_init());
 write_rejected=1;assert(!sd_write_sector(1,buffer));write_rejected=0;assert(sd_init());
 {unsigned before=timer_reads;busy_stuck=1;assert(!sd_write_sector(1,buffer));assert(timer_reads-before>=146&&timer_reads-before<160);busy_stuck=0;}assert(sd_init());
 transfer_failed=1;assert(!sd_read_sector(1,buffer));transfer_failed=0;
 init_stuck=1;assert(!sd_init());init_stuck=0;assert(sd_init());
 puts("SD SPI protocol, address, response and timeout probes passed");return 0;
}
