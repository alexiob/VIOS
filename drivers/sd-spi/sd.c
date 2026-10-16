/* VIOS SDHC/SDXC SPI block device. Pure C protocol, architecture I/O below.
 * Single-sector commands keep state/resources bounded; no MOS/VDP dependency.
 */
#include "sd.h"
#include "spi.h"
#include "platform/timer.h"
#include <stdint.h>
static uint32_t sectors;
static int ready;
static const char *error_text="card not initialized";
struct deadline {unsigned short previous;uint32_t elapsed,limit,polls;};
static void begin(struct deadline *d,unsigned milliseconds) {
 d->previous=platform_timer_read();d->elapsed=0;
 d->limit=(uint32_t)milliseconds*288UL;d->polls=(uint32_t)milliseconds*1024UL+1;
}
static int expired(struct deadline *d) {
 unsigned short now=platform_timer_read();
 d->elapsed+=(unsigned short)(d->previous-now);d->previous=now;
 if(!d->polls)return 1;--d->polls;
 return d->elapsed>=d->limit;
}
static int fail(const char *text){error_text=text;ready=0;return 0;}
static int exchange(unsigned char tx,unsigned char *rx) {
 if(!ez80_spi_xfer(tx,rx))return fail("SPI transfer timeout or fault");return 1;
}
static int release(void) {unsigned char discard;ez80_spi_select(0);return exchange(255,&discard);}
static int wait_ready(unsigned milliseconds) {
 struct deadline timer;unsigned char value;begin(&timer,milliseconds);
 do {if(!exchange(255,&value))return 0;if(value==255)return 1;}while(!expired(&timer));
 return fail("SD busy timeout");
}
static int command(unsigned char op,uint32_t argument,unsigned char crc,unsigned char *response) {
 unsigned char discard;unsigned i;
 if(!release())return 0;ez80_spi_select(1);
 if(!wait_ready(100))return 0;
 if(!exchange((unsigned char)(0x40|op),&discard))return 0;
 for(i=0;i<4;i++){if(!exchange((unsigned char)(argument>>24),&discard))return 0;argument<<=8;}
 if(!exchange(crc,&discard))return 0;
 for(i=0;i<10;i++){if(!exchange(255,response))return 0;if(!(*response&0x80))return 1;}
 return fail("SD command response timeout");
}
static uint16_t crc16(uint16_t crc,unsigned char value) {
 unsigned i;crc^=(uint16_t)value<<8;
 for(i=0;i<8;i++)crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));
 return crc;
}
static int read_data(unsigned char *buffer,unsigned count) {
 struct deadline timer;unsigned char value,hi,lo;uint16_t crc=0;unsigned i;begin(&timer,200);
 do {if(!exchange(255,&value))return 0;if(value!=255)break;}while(!expired(&timer));
 if(value!=0xfe)return fail("SD data token error or timeout");
 for(i=0;i<count;i++){if(!exchange(255,&buffer[i]))return 0;crc=crc16(crc,buffer[i]);}
 if(!exchange(255,&hi)||!exchange(255,&lo))return 0;
 if(crc!=((uint16_t)hi<<8|lo))return fail("SD data CRC mismatch");
 return 1;
}
int sd_init(void) {
 struct deadline timer;unsigned char response,ocr[4],csd[16],discard;unsigned i;
 ready=0;sectors=0;error_text="card initialization failed";ez80_spi_init(0);ez80_spi_select(0);
 /* CS and MOSI high for at least 80 startup clocks, at 288 kHz. */
 for(i=0;i<10;i++)if(!exchange(255,&discard))return 0;
 begin(&timer,1000);
 do {
  if(!command(0,0,0x95,&response))goto error;
  if(!release())return 0;
  if(response==1)break;
 }while(!expired(&timer));
 if(response!=1){fail("SD idle timeout");goto error;}
 if(!command(8,0x1aa,0x87,&response))goto error;
 if(response!=1){fail("SD v2 card required");goto error;}
 for(i=0;i<4;i++)if(!exchange(255,&ocr[i]))goto error;
 if(!release())return 0;
 if(ocr[2]!=1||ocr[3]!=0xaa){fail("SD voltage/echo mismatch");goto error;}
 begin(&timer,1000);
 do {
  if(!command(55,0,255,&response))goto error;
  if(!release())return 0;
  if(response>1){fail("SD APP command rejected");goto error;}
  if(!command(41,0x40000000UL,255,&response))goto error;
  if(!release())return 0;
  if(response==0)break;
  if(response!=1){fail("SD initialization response error");goto error;}
 }while(!expired(&timer));
 if(response){fail("SD initialization timeout");goto error;}
 if(!command(58,0,255,&response))goto error;
 if(response){fail("SD OCR rejected");goto error;}
 for(i=0;i<4;i++)if(!exchange(255,&ocr[i]))goto error;
 if(!release())return 0;
 if((ocr[0]&0xc0)!=0xc0||!(ocr[1]&0x30)){fail("SDHC/SDXC 3.3V card required");goto error;}
 ez80_spi_init(1);
 if(!command(9,0,255,&response))goto error;
 if(response||!read_data(csd,16)){if(response)fail("SD CSD rejected");goto error;}
 if(!release())return 0;
 if((csd[0]&0xc0)!=0x40){fail("unsupported SD CSD");goto error;}
 sectors=(((uint32_t)(csd[7]&63)<<16)|((uint32_t)csd[8]<<8)|csd[9])+1;
 if(sectors>=0x400000UL){sectors=0;fail("SD capacity exceeds 32-bit sector count");goto error;}
 sectors*=1024UL;ready=1;error_text="ok";return 1;
error:ez80_spi_select(0);return 0;
}
uint32_t sd_sector_count(void){return sectors;}
const char *sd_error(void){return error_text;}
int sd_read_sector(uint32_t sector,unsigned char *buffer) {
 unsigned char response;int ok;
 if(!ready)return fail("card not initialized");
 if(!buffer||sector>=sectors){error_text="invalid SD read range";return 0;}
 if(!command(17,sector,255,&response)){ez80_spi_select(0);return 0;}
 ok=!response&&read_data(buffer,512);
 if(response)fail("SD read command rejected");
 if(!release())return 0;
 if(ok)error_text="ok";return ok;
}
int sd_write_sector(uint32_t sector,const unsigned char *buffer) {
 unsigned char response,discard;uint16_t crc=0;unsigned i;int ok=0;
 if(!ready)return fail("card not initialized");
 if(!buffer||sector>=sectors){error_text="invalid SD write range";return 0;}
 if(!command(24,sector,255,&response))goto done;
 if(response){fail("SD write command rejected");goto done;}
 if(!exchange(0xfe,&discard))goto done;
 for(i=0;i<512;i++){crc=crc16(crc,buffer[i]);if(!exchange(buffer[i],&discard))goto done;}
 if(!exchange((unsigned char)(crc>>8),&discard)||!exchange((unsigned char)crc,&discard))goto done;
 if(!exchange(255,&response))goto done;
 if((response&0x1f)!=5){fail("SD write data rejected");goto done;}
 if(!wait_ready(500))goto done;
 ok=1;error_text="ok";
done:if(!release())return 0;return ok;
}
int sd_sync(void) {
 int ok;if(!ready)return fail("card not initialized");ez80_spi_select(1);
 ok=wait_ready(500);if(!release())return 0;if(ok)error_text="ok";return ok;
}
