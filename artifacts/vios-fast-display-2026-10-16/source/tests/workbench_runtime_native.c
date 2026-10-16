#define main original_runtime_main
#include "vios_runtime_host.c"
#undef main
#include "vios/workbench.h"
#include "vios/workbench_runtime.h"
#include "platform/input.h"
#include "vios/text_screen.h"
static struct vios_workbench_status status(void){struct vios_workbench_status s;vios_workbench_status(&s);return s;}
static void request(const char *format,uint32_t revision){char line[256];snprintf(line,sizeof line,format,(unsigned long)revision);eval(line);}
static void paint_bindings(void){
 reset();eval("WB-OPEN");assert(fvm_push(77));
 eval(": PAINT-WRAPPER WB-PAINT-DEFAULT ; ' PAINT-WRAPPER IS WB-PAINT");
 vios_workbench_idle(1);assert(timer_depth==0&&fvm_depth==1&&fvm_ds[0]==77);
 eval("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW");vios_workbench_idle(1);assert(timer_depth==1);
 /* A name match is not native identity. The old target remains callable,
  * but shadowing the public name conservatively retains the observer. */
 eval(": WB-PAINT-DEFAULT ;");vios_workbench_idle(1);assert(timer_depth==0&&fvm_depth==1&&fvm_ds[0]==77);
 eval("' WB-PAINT-DEFAULT IS WB-PAINT");unsigned calls=vdp_calls;vios_workbench_idle(1);assert(vdp_calls==calls&&vios_text_pending());
 eval(": WB-PAINT ;");calls=vdp_calls;vios_workbench_idle(1);assert(vdp_calls==calls&&fvm_depth==1&&fvm_ds[0]==77);
 reset();eval("WB-OPEN : SPIN-PAINT BEGIN AGAIN ; ' SPIN-PAINT IS WB-PAINT");assert(fvm_push(88));
 vios_workbench_idle(1);assert(fvm_depth==1&&fvm_ds[0]==88&&strstr(status().error,"budget"));
 eval("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW WB-FLUSH");assert(!status().error[0]&&!vios_text_pending()&&fvm_depth==1&&fvm_ds[0]==88);
}
int main(int argc,char **argv){
 assert(argc==2);strcpy(root,argv[1]);reset();assert(storage_mkdir("/VIOS"));
 assert(forth_find_xt("WB-PAINT-DEFAULT")&&forth_find_xt("WB-PAINT"));
 eval("WB-RENDER-STATS RESET WB-STATUS");
 eval("WB-OPEN");assert(vios_workbench_active()&&keyboard_route);
 static const unsigned char setup[]={22,1,17,3,17,128,12,23,1,0};
 assert(vdp_setup_length==sizeof setup&&!memcmp(vdp_setup,setup,sizeof setup));
 assert(fvm_push(77));forth_set_base(16);vios_workbench_idle(1);assert(fvm_depth==1&&fvm_ds[0]==77&&forth_get_base()==16);assert(vdp_max<=128);assert(!input_paused);
 /* The exact trusted native painter avoids observer stack copying. */
 assert(timer_depth==1);
 unsigned modes=vdp_modes;eval("WB-REPAINT WB-FLUSH");assert(vdp_modes==modes&&!vios_text_pending());
 unsigned fences=vdp_fences;eval("WB-FLUSH-FENCED");assert(vdp_fences==fences+1);
 vdp_fence_fail=1;assert(!forth_eval("WB-FLUSH-FENCED")&&strstr(status().error,"fence")&&fvm_depth==1&&fvm_ds[0]==77);vdp_fence_fail=0;
 eval("WB-REPAINT WB-FLUSH");assert(!status().error[0]&&vdp_modes==modes);
 eval("WB-ZOOM 1 WB-ZOOM?");assert(vios_workbench_zoomed());vios_workbench_idle(1);assert(vdp_modes==modes);
 assert(!forth_eval("WB-ZOOM 2")&&vios_workbench_zoomed());
 eval("WB-ZOOM 0 WB-REDRAW");assert(!vios_workbench_zoomed()&&vdp_modes==modes+1);
 /* FabGL Ctrl+Space is ASCII NUL, VirtualKey SPACE=1. Key-up must not toggle. */
 queued_key=(struct platform_key_event){0,PLATFORM_MOD_CTRL,1,0};have_key=1;vios_workbench_idle(0);assert(!vios_workbench_zoomed());
 queued_key.down=1;have_key=1;vios_workbench_idle(0);assert(vios_workbench_zoomed());
 have_key=1;vios_workbench_idle(0);assert(!vios_workbench_zoomed());
 eval("WB-COMPOSE");assert(fvm_depth==1&&fvm_ds[0]==77&&forth_get_base()==16);
 eval("WB-NEW demo 1 0");struct vios_workbench_status s=status();
 request("WB-PATCH %lu 0 0 3A2056414C5545203432203B",s.revision);s=status();assert(!strcmp(vios_workbench_source(),": VALUE 42 ;"));
 char stale[96];snprintf(stale,sizeof stale,"WB-PATCH %lu 0 0 78",(unsigned long)(s.revision-1));assert(!forth_eval(stale));assert(fvm_depth==1&&fvm_ds[0]==77);assert(!strcmp(vios_workbench_source(),": VALUE 42 ;"));
 eval("WB-SOURCE 0 80");eval("WB-BROWSE WORDS");eval("WB-SELECT WORD 0");eval("WB-VIEW 0 2");
 request("WB-STAGE %lu",s.revision);assert(status().staged);request("WB-ACTIVATE %lu",s.revision);assert(status().activated);tear_package=1;char save_line[96];snprintf(save_line,sizeof save_line,"WB-SAVE %lu",(unsigned long)s.revision);assert(!forth_eval(save_line));assert(fvm_depth==1&&fvm_ds[0]==77&&!status().saved);request("WB-SAVE %lu",s.revision);assert(status().saved);
 assert(forth_execute("VALUE"));assert(fvm_pop()==42&&fvm_pop()==77);
 eval("WB-FLUSH");eval("WB-RENDER-STATS");assert(!vios_text_pending());
 unsigned calls=vdp_calls;vios_workbench_idle(0);assert(vdp_calls==calls&&!input_paused);
 assert(fvm_push(55));eval(": BAD-PAINT HEX 9 ; ' BAD-PAINT IS WB-PAINT WB-REDRAW");vios_workbench_idle(1);assert(fvm_depth==1&&fvm_ds[0]==55&&forth_get_base()==16&&status().error[0]);calls=vdp_calls;vios_workbench_idle(0);assert(vdp_calls==calls);
 eval("' WB-PAINT-DEFAULT IS WB-PAINT WB-REDRAW WB-FLUSH");assert(!status().error[0]);assert(fvm_pop()==55);
 /* Shifted ASCII is already translated by VDP; key-up must never edit. */
 eval("WB-KEY 9 0");s=status();unsigned length=s.length;
 queued_key=(struct platform_key_event){'A',PLATFORM_MOD_SHIFT,0,0};have_key=1;vios_workbench_idle(0);assert(status().length==length);
 queued_key=(struct platform_key_event){'A',PLATFORM_MOD_SHIFT,0,1};have_key=1;vios_workbench_idle(0);assert(status().length==length+1);
 queued_key=(struct platform_key_event){0,0,PLATFORM_KEY_F8,1};have_key=1;vios_workbench_idle(0);assert(status().length==length);
 /* Function-key save follows the same exact activated-source guard. */
 queued_key=(struct platform_key_event){0,0,PLATFORM_KEY_F7,1};have_key=1;vios_workbench_idle(0);assert(status().saved);
 queued_key=(struct platform_key_event){0,0,PLATFORM_KEY_F10,1};have_key=1;vios_workbench_idle(0);assert(!vios_workbench_active()&&!keyboard_route&&!input_paused);
 eval("WB-RENDER-STATS RESET");
 reset();eval("WB-NEW unsaved 1 0");s=status();request("WB-PATCH %lu 0 0 3A20554E53415645442031203B",s.revision);s=status();request("WB-STAGE %lu",s.revision);request("WB-ACTIVATE %lu",s.revision);
 eval(": WB-SAVE-DEFAULT ;");assert(fvm_push(66));snprintf(save_line,sizeof save_line,"WB-SAVE %lu",(unsigned long)s.revision);assert(!forth_eval(save_line));assert(!status().saved&&fvm_depth==1&&fvm_ds[0]==66);
 paint_bindings();puts("Workbench runtime checks passed");return 0;
}
