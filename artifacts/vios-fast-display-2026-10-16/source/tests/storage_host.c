/* Real FatFs operates on a regular-file block device, never a live disk. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "platform/storage.h"
#include "drivers/sd-spi/sd.h"
static FILE *disk;
static uint32_t blocks;
static int fail_read, fail_write, fail_sync;
int sd_init(void) { return 1; }
uint32_t sd_sector_count(void) { return blocks; }
const char *sd_error(void) { return "injected block error"; }
int sd_read_sector(uint32_t sector, unsigned char *buffer) {
 if(fail_read||sector>=blocks||fseek(disk,(long)sector*512,SEEK_SET))return 0;
 return fread(buffer,1,512,disk)==512;
}
int sd_write_sector(uint32_t sector,const unsigned char *buffer) {
 if(fail_write||sector>=blocks||fseek(disk,(long)sector*512,SEEK_SET))return 0;
 return fwrite(buffer,1,512,disk)==512;
}
int sd_sync(void) { return !fail_sync&&fflush(disk)==0; }
int main(int argc,char **argv) {
 unsigned char output[20000],payload[17537];size_t size=999;
 assert(argc==2||argc==3);disk=fopen(argv[1],"r+b");assert(disk);
 assert(!fseek(disk,0,SEEK_END));blocks=(uint32_t)(ftell(disk)/512);
 if(argc==3&&!strcmp(argv[2],"fat16")){assert(!storage_init());assert(!strcmp(storage_error(),"not FAT32"));fclose(disk);puts("FAT16 correctly rejected");return 0;}
 assert(storage_init());
 if(argc==3&&!strcmp(argv[2],"full")){assert(storage_mkdir("VIOS"));assert(!storage_write_file("VIOS/FULL.FTH","x",1));assert(strstr(storage_error(),"short"));assert(storage_read_file("BOOT.TXT",output,sizeof output,&size)&&size==14);assert(storage_sync());fclose(disk);puts("full FAT32 volume handled");return 0;}
 assert(storage_read_file("BOOT.TXT",output,sizeof output,&size));assert(size==14&&!memcmp(output,"keep-existing\n",14));
 memset(output,0x55,sizeof output);size=999;
 assert(!storage_read_file("BOOT.TXT",output,13,&size));assert(size==0&&output[0]==0x55&&output[13]==0x55);
 assert(storage_error()[0]);
 assert(storage_mkdir("VIOS"));assert(storage_mkdir("VIOS"));
 for(size_t i=0;i<sizeof payload;i++)payload[i]=(unsigned char)(i*37u+i/7u);
 assert(storage_write_file("VIOS/SOURCE.FTH",payload,sizeof payload));
 memset(output,0,sizeof output);assert(storage_read_file("VIOS/SOURCE.FTH",output,sizeof output,&size));
 assert(size==sizeof payload&&!memcmp(payload,output,size));
 assert(storage_rename("VIOS/SOURCE.FTH","VIOS/SAVED.FTH"));
 assert(!storage_read_file("VIOS/SOURCE.FTH",output,sizeof output,&size)&&size==0);
 assert(storage_write_file("VIOS/SAVED.FTH","short\r\n",7));
 assert(storage_read_file("VIOS/SAVED.FTH",output,sizeof output,&size)&&size==7&&!memcmp(output,"short\r\n",7));
 assert(storage_write_file("VIOS/EMPTY.FTH",NULL,0));assert(storage_read_file("VIOS/EMPTY.FTH",NULL,0,&size)&&size==0);
 assert(!storage_mkdir("BOOT.TXT"));assert(!storage_read_file(NULL,output,sizeof output,&size)&&size==0);
 assert(!storage_write_file("VIOS/BAD.FTH",NULL,1));assert(!storage_read_file("BOOT.TXT",output,sizeof output,NULL));
 assert(storage_remove("VIOS/EMPTY.FTH"));assert(!storage_remove("VIOS/EMPTY.FTH"));
 fail_write=1;assert(!storage_write_file("VIOS/FAIL.FTH",payload,sizeof payload));assert(storage_error()[0]);fail_write=0;
 /* Failed media writes invalidate the mount; explicitly remount to recover. */
 assert(storage_init());
 fail_sync=1;assert(!storage_sync());assert(!strcmp(storage_error(),"injected block error"));fail_sync=0;assert(storage_init());
 assert(storage_sync());assert(storage_init());
 fail_read=1;assert(!storage_read_file("BOOT.TXT",output,sizeof output,&size)&&size==0);fail_read=0;
 assert(storage_init());assert(storage_read_file("BOOT.TXT",output,sizeof output,&size)&&size==14);
 assert(storage_read_file("VIOS/SAVED.FTH",output,sizeof output,&size)&&size==7);
 assert(storage_sync());assert(!fclose(disk));puts("storage real FAT32 roundtrip/failure probes passed");return 0;
}
