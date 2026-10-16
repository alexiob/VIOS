/* Explicit real-card diagnostic; touches only a new VIOSTEST.TMP file. */
#include "platform/console.h"
#include "platform/timer.h"
#include "platform/storage.h"
static void out(const char *s){while(*s)platform_console_putc((unsigned char)*s++);}
static int equal(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return *a==*b;}
void kernel_main(void) {
 platform_console_init();platform_timer_init();out("\r\nVIOS SD probe\r\n");
 char buffer[512];size_t size=0;
 if(!storage_init()){out("MOUNT ERROR ");out(storage_error());goto done;}
 out("MOUNT FAT32 OK\r\n");
 if(!storage_read_file("/autoexec.txt",buffer,sizeof buffer-1,&size)){out("READ ERROR ");out(storage_error());goto done;}
 buffer[size]=0;out("AUTOEXEC ");out(buffer);out("\r\n");
 if(storage_read_file("/VIOSTEST.TMP",buffer,sizeof buffer,&size)||!equal(storage_error(),"file not found")) {
  out("REFUSE existing or unreadable diagnostic file ");out(storage_error());goto done;
 }
 static const char content[]="VIOS native SD/FAT32 verified 2026-10-16\r\n";
 if(!storage_write_file("/VIOSTEST.TMP",content,sizeof content-1)){out("WRITE ERROR ");out(storage_error());goto done;}
 if(!storage_read_file("/VIOSTEST.TMP",buffer,sizeof buffer-1,&size)){out("READBACK ERROR ");out(storage_error());goto done;}
 buffer[size]=0;if(size!=sizeof content-1||!equal(content,buffer)){out("READBACK MISMATCH");goto done;}
 if(!storage_remove("/VIOSTEST.TMP")||!storage_sync()){out("REMOVE/SYNC ERROR ");out(storage_error());goto done;}
 out("NATIVE SD WRITE READBACK REMOVE PASS");
done:out("\r\nvios> ");for(;;){}
}
