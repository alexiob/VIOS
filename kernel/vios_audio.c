/* Sound words: compact VDU 23,0,E9 commands for the ESP audio engine
 * (docs/superpowers/specs/2026-10-20-vios-audio.md). Only SOUND? waits for a
 * reply; the ESP range-checks, so arguments are only truncated here. */
#include "vios/runtime.h"
#include "vios/wire.h"
#include "platform/vdp.h"
static unsigned char cmd[8]={23,0,VIOS_VDU_AUDIO};
/* Pop the arguments into one frame: size payload bytes after op; bit i of wide
 * marks the i-th popped (last-pushed) argument as a little-endian u16. */
static int frame(unsigned char op,unsigned size,unsigned wide){
 unsigned char *p=cmd+4+size;
 while(p>cmd+4){fcell v=fvm_pop();if(wide&1)*--p=(unsigned char)(v>>8);*--p=(unsigned char)v;wide>>=1;}
 if(fvm_error)return 0;cmd[3]=op;platform_vdp_send(cmd,4+size);return 1;
}
static int tone(void){return frame(0,4,3);}
static int tune(void){
 fcell n=fvm_depth>1?fvm_ds[fvm_depth-2]:0,a;
 if(n>255)return forth_error("TUNE text over 255 bytes");
 if(!frame(1,2,0))return 0;a=fvm_pop();return !fvm_error&&vios_vdp_copy(a,n);
}
static int play(void){return frame(2,2,0);}
static int stop(void){return frame(3,1,0);}
static int volume(void){return frame(4,1,0);}
static int busy(void){
 static const unsigned char query[]={23,0,VIOS_VDU_AUDIO,5};unsigned char reply[4];unsigned n;
 if(!platform_vdp_request(query,sizeof query,VIOS_PACKET_AUDIO,reply,sizeof reply,&n)||n!=4)return forth_error("VDP audio timeout");
 return fvm_push(reply[0]);
}
int vios_audio_init(void){
 static const struct {const char *name;int (*fn)(void);} words[]={{"TONE",tone},{"TUNE",tune},{"PLAY",play},{"STOP",stop},{"VOLUME",volume},{"SOUND?",busy}};
 for(unsigned i=0;i<sizeof words/sizeof words[0];++i)if(!forth_register_native(words[i].name,words[i].fn,FORTH_NATIVE_TASK))return 0;
 return 1;
}
