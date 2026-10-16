/* Bounded synchronous file API. The filesystem/SD implementations are separate
 * from this board-facing service and also run against the host image backend.
 */
#include "platform/storage.h"
#include "drivers/fatfs/ff.h"
#include "drivers/fatfs/diskio.h"
#include "drivers/sd-spi/sd.h"
#include <stdint.h>
typedef char storage_type_widths[(sizeof(BYTE)==1&&sizeof(WORD)==2&&sizeof(DWORD)==4&&sizeof(UINT)==4)?1:-1];
static FATFS volume;
static FIL file;
static int mounted;
static const char *last_error="not mounted";
static const char *const errors[]={
 "ok","disk I/O error","filesystem inconsistency","card not ready",
 "file not found","path not found","invalid name","access denied or full",
 "destination exists","invalid object","write protected","invalid drive",
 "not mounted","not FAT32","format error","filesystem timeout",
 "file locked","filesystem memory exhausted","too many files","invalid parameter"
};
static int fail(const char *message) {last_error=message;return 0;}
static int result(FRESULT code) {
 if(code==FR_OK){last_error="ok";return 1;}
 if(code==FR_DISK_ERR||code==FR_NOT_READY||code==FR_INT_ERR){mounted=0;f_mount(0,"",0);}
 if(code==FR_DISK_ERR||code==FR_NOT_READY)return fail(sd_error());
 return fail((unsigned)code<sizeof errors/sizeof errors[0]?errors[code]:"filesystem error");
}
static int valid(const char *path) {
 unsigned n=0;if(!path||!*path)return fail("invalid path");
 while(path[n]){unsigned char c=(unsigned char)path[n];if(n==127||c<33||c>126)return fail("invalid path");++n;}
 if(!mounted)return fail("not mounted");return 1;
}
int storage_init(void) {
 mounted=0;f_mount(0,"",0);
 if(!result(f_mount(&volume,"",1)))return 0;
 if(volume.fs_type!=FS_FAT32){f_mount(0,"",0);return fail("not FAT32");}
 mounted=1;last_error="ok";return 1;
}
const char *storage_error(void) {return last_error;}
int storage_read_file(const char *path,void *buffer,size_t capacity,size_t *size) {
 FRESULT status;UINT got=0;FSIZE_t length;
 if(size)*size=0;
 if(!size||(!buffer&&capacity))return fail("invalid buffer");
 if(!valid(path))return 0;
 status=f_open(&file,path,FA_READ);if(status!=FR_OK)return result(status);
 length=f_size(&file);
 if(length>capacity){f_close(&file);return fail("file exceeds buffer");}
 status=length?f_read(&file,buffer,(UINT)length,&got):FR_OK;
 {FRESULT close_status=f_close(&file);if(status==FR_OK)status=close_status;}
 if(!result(status))return 0;
 if(got!=length)return fail("short file read");
 *size=(size_t)length;return 1;
}
int storage_write_file(const char *path,const void *buffer,size_t size) {
 FRESULT status;UINT written=0;
 if(!buffer&&size)return fail("invalid buffer");
 if((uint32_t)size!=size)return fail("file exceeds 32-bit size");
 if(!valid(path))return 0;
 status=f_open(&file,path,FA_WRITE|FA_CREATE_ALWAYS);if(status!=FR_OK)return result(status);
 status=size?f_write(&file,buffer,(UINT)size,&written):FR_OK;
 if(status==FR_OK&&written==(UINT)size)status=f_sync(&file);
 {FRESULT close_status=f_close(&file);if(status==FR_OK)status=close_status;}
 if(!result(status))return 0;
 if(written!=(UINT)size)return fail("disk full: short file write");
 return 1;
}
int storage_rename(const char *old_path,const char *new_path) {
 if(!valid(old_path)||!valid(new_path))return 0;
 return result(f_rename(old_path,new_path));
}
int storage_remove(const char *path) {if(!valid(path))return 0;return result(f_unlink(path));}
int storage_mkdir(const char *path) {
 FRESULT status;FILINFO info;if(!valid(path))return 0;
 status=f_mkdir(path);
 if(status==FR_EXIST){status=f_stat(path,&info);if(status==FR_OK&&!(info.fattrib&AM_DIR))return fail("path is a file");}
 return result(status);
}
int storage_sync(void) {
 if(!mounted)return fail("not mounted");
 if(disk_ioctl(0,CTRL_SYNC,0)!=RES_OK){mounted=0;f_mount(0,"",0);return fail(sd_error());}
 last_error="ok";return 1;
}
