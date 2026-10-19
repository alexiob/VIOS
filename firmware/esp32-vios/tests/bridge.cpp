/* Execute the actual bridge logic against byte/register-level dependencies. */
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include <string>
#include "vios/wire.h"
#define PROGMEM
#define INPUT 0
#define OUTPUT 1
#define HIGH 1
#define ZDI_TCK 26
#define ZDI_TDI 27
#define ZDI_BRK_CTL 0x10
#define ZDI_IS2 0x23
#define ZDI_IS1 0x24
#define ZDI_IS0 0x25
#define REG_AF 0
#define REG_PC 7
#define REG_HL 3
#define REG_IX 4
#define REG_IY 5
#define SET_ADL 8
#define CS0_LBR 0xA8
#define CS0_UBR 0xA9
#define CS0_CTL 0xAA
#define CS0_BMC 0xF0
using rw_control_t=unsigned;
static uint32_t now;
static bool trace_usb;
static std::vector<unsigned> usb_actions;
static void note_usb(unsigned action);
static unsigned long millis(){return now;}
static void delay(unsigned value){if(trace_usb)note_usb(0x10000+value);now+=value;}
static void pinMode(int,int){}
static void digitalWrite(int,int){}
#define memcpy_P memcpy
#include "runtime.h"
#include "vios_stage0.h"
struct Serial {
    bool usb;
    explicit Serial(bool is_usb=false):usb(is_usb){}
    std::vector<uint8_t> output;
    std::deque<uint8_t> input;
    size_t write(const uint8_t *p,size_t n){if(usb&&trace_usb)note_usb(1);output.insert(output.end(),p,p+n);return n;}
    size_t write(uint8_t byte){output.push_back(byte);return 1;}
    int available(){return (int)input.size();}
    int read(){if(input.empty())return -1;int b=input.front();input.pop_front();return b;}
    void flush(){if(usb&&trace_usb)note_usb(2);}
} DBGSerial(true),VDPSerial;
static uint8_t memory[0x1000000],ports[256],status=0x98;
static uint32_t regs[8];
static bool bad_readback,bad_code_readback,bad_completion,bad_map;
static bool stage0_completed; // External RAM is written only after a native stage0 completion.
static unsigned native_runs,external_writes,memory_writes;
static unsigned discovery_port,discovery_samples;
static bool disagree_discovery;
static std::vector<unsigned> discovery;
static uint32_t internal_base(){return ((uint32_t)ports[0xB5]<<16)|0xE000;}
static void native_stage0(){
    ++native_runs;uint32_t start=internal_base(),pc=regs[7];
    for(unsigned steps=0;steps<64;++steps){
        assert(pc>=start&&pc<start+256);uint8_t op=memory[pc++];
        if(op==0xF3)continue;
        if(op==0x3E){regs[0]=(regs[0]&0xFFFF00)|memory[pc++];continue;}
        if(op==0xED){
            uint8_t sub=memory[pc++],port=memory[pc++];
            if(sub==0x39){assert(port==0xA8||port==0xA9||port==0xAA||port==0xF0);ports[port]=(uint8_t)regs[0];if(bad_map&&port==0xA8)ports[port]=5;}
            else {assert(sub==0x38);regs[0]=(regs[0]&0xFFFF00)|ports[port];}
            continue;
        }
        if(op==0x77){assert(regs[3]>=start+256&&regs[3]<=start+260);if(!(bad_completion&&regs[3]==start+260)){memory[regs[3]]=(uint8_t)regs[0];if(regs[3]==start+260&&regs[0]%256==0xA5)stage0_completed=true;}continue;}
        if(op==0x23){++regs[3];continue;}
        assert(op==0x18&&memory[pc]==0xFE);regs[7]=pc-1;return;
    }
    assert(false);
}
static bool bad_crc,crc_never_finishes;static unsigned crc_runs,crc_slices;
static const unsigned crc_slices_needed=3; // The real stage outlives one 100 ms slice.
/* Native model of arch/ez80/agon_crc.s (its real bytes are executed by
 * tests/test_agon_crc.py): CRC32 of count*256 bytes at IX, result at IY+5. */
static void native_crc(){
    uint32_t meta=regs[5],start=internal_base();
    if(regs[7]==start){++crc_runs;crc_slices=0;assert(meta==start+256&&regs[3]==start+0x400&&regs[4]==0x40000);}
    if(++crc_slices<crc_slices_needed||crc_never_finishes){regs[7]=start+8;return;} // Halted mid-loop.
    unsigned blocks=memory[meta+3]|(memory[meta+4]<<8);assert(blocks);
    uint32_t crc=vios_wire_crc32(memory+regs[4],blocks*256u);if(bad_crc)crc^=1;
    for(unsigned i=0;i<4;++i)memory[meta+5+i]=(uint8_t)(crc>>(8*i));
    memory[meta+9]=0xA5;
    regs[7]=start+vios_crc_stage_size-2;
}
static void inject(unsigned,uint8_t);
static void vios_zdi_write_register(unsigned r,uint8_t value){
    if(r==0x10){status=value?0x98:0x18;
        bool crc_loaded=!memcmp(memory+internal_base(),vios_crc_stage,vios_crc_stage_size);
        if(!value&&regs[7]==internal_base()){if(crc_loaded)native_crc();else native_stage0();}
        else if(!value&&crc_loaded&&crc_slices&&memory[internal_base()+265]!=0xA5){assert(regs[7]==internal_base()+8);native_crc();} // Must resume mid-loop.
    }else inject(r,value);
}
static uint8_t vios_zdi_status(){return status;}
static unsigned identity_reads;
static uint16_t vios_zdi_id(){++identity_reads;return 7;}
static uint8_t vios_zdi_revision(){return 0xAA;}
static uint32_t vios_zdi_cpu_read(unsigned r){return r<8?regs[r]:0;}
static void vios_zdi_cpu_write(unsigned r,uint32_t value){assert(r<8);regs[r]=value;}
static uint8_t instructions[3];
static void inject(unsigned r,uint8_t value){
    if(r<0x23||r>0x25)return;instructions[r-0x23]=value;if(r!=0x25)return;
    if(value==0x3E)regs[0]=(regs[0]&0xFFFF00)|instructions[1];
    if(value==0xED&&instructions[1]==0x38)regs[0]=(regs[0]&0xFFFF00)|ports[instructions[0]];
}
static uint8_t vios_zdi_boot_in0(uint8_t port){
    assert(port==0xB5||port==0xB4);discovery.push_back(port);
    if(port!=discovery_port){discovery_port=port;discovery_samples=0;}
    uint8_t value=discovery_samples++?ports[port]:(uint8_t)regs[0];
    if(disagree_discovery&&port==0xB5&&discovery_samples==3)value^=1;
    regs[0]=(regs[0]&0xFFFF00)|value;return value;
}
static void vios_zdi_di(){discovery_port=0;discovery_samples=0;}
static void vios_zdi_write_memory(uint32_t address,unsigned n,uint8_t *data){++memory_writes;assert((address>=0x40000&&address+n<=0xAF000)||(address>=internal_base()&&address+n<=internal_base()+0x2000));
    if(address<0xAF000){assert(native_runs&&stage0_completed);++external_writes;}
    memcpy(memory+address,data,n);regs[7]=address+n;}
static void vios_zdi_read_memory(uint32_t address,unsigned n,uint8_t *data){
    assert((address>=0x40000&&address+n<=0xC0000)||(address>=internal_base()&&address+n<=internal_base()+0x2000));
    memcpy(data,memory+address,n);if(bad_readback||(bad_code_readback&&address==internal_base()))data[0]^=1;regs[7]=address+n;
}
struct {bool restarted=false;void restart(){restarted=true;}} ESP;
#define VIOS_ZDI_EDGE_US 1 // Real transport lives in vios_zdi.h, faked above.
static int64_t esp_timer_get_time(){return (int64_t)now*1000;}
static bool g0_fake_busy;static int g0_fake_mode=-1;
static bool vios_g0_request(uint8_t m){if(g0_fake_busy)return false;g0_fake_busy=true;g0_fake_mode=m;return true;}
static bool vios_g0_busy(){return g0_fake_busy;}
static void vios_g0_reset_gap(){}
static uint32_t vios_g0_gap(){return 0;}
static void vios_frame_stats(uint32_t *f,uint32_t *s,uint32_t *w){*f=*s=*w=0;}
static unsigned gui_keys;
unsigned vios_gui_status(char *out,unsigned capacity){return (unsigned)snprintf(out,capacity,"GUI on windows 1 fake");}
bool vios_gui_key(uint8_t,uint8_t,uint8_t vk,uint8_t){++gui_keys;return vk==168;}
int vios_gui_pixel(unsigned x,unsigned y){return x<640&&y<480?(int)((x+y)&15):-1;}
static void vios_frame_stats_reset(){}
static const char *vios_g0_line(unsigned n){return n==0?"G0 mode 0 fake":"";}
static std::string console_fed;
static void vios_console_feed(const uint8_t *d,size_t n){console_fed.append((const char*)d,n);}
static unsigned vios_console_row(unsigned y,char *out,unsigned capacity){return (unsigned)snprintf(out,capacity,"row %u",y);}
#include "vios_bridge.h"
static void note_usb(unsigned action){assert(usb_tx_mutex&&usb_tx_mutex->held);usb_actions.push_back(action);}
static std::vector<vios_wire_message> output_frames(){
    std::vector<vios_wire_message> frames;size_t at=0;
    for(size_t i=0;i<DBGSerial.output.size();++i)if(!DBGSerial.output[i]){vios_wire_message message;assert(vios_wire_decode(DBGSerial.output.data()+at,i-at,&message));frames.push_back(message);at=i+1;}
    DBGSerial.output.clear();return frames;
}
static void incoming(const char *text,uint16_t sequence){vios_wire_message m{};m.channel=2;m.sequence=sequence;m.length=(uint16_t)strlen(text);assert(m.length<=240);memcpy(m.payload,text,m.length);usb_dispatch(&m);}
static void check_usb_console_pacing(){
    uint8_t data[600];for(unsigned i=0;i<sizeof data;++i)data[i]=(uint8_t)i;
    // The VDP-side write only queues (Stage 1a); the control-task drain does the paced USB work.
    trace_usb=true;usb_actions.clear();vios_usb_console_write(data,sizeof data);assert(usb_actions.empty()&&DBGSerial.output.empty());
    assert(console_out_drain(16)==3);trace_usb=false;
    const std::vector<unsigned> want={1,2,0x10001,1,2,0x10001,1,2,0x10001};
    assert(usb_actions==want); // Each complete frame drains, then yields under the frame lock.
    auto frames=output_frames();assert(frames.size()==3);
    size_t at=0;for(auto &frame:frames){assert(frame.channel==1&&frame.sequence==0);
        assert(frame.length==(at<480?240:120));assert(!memcmp(frame.payload,data+at,frame.length));at+=frame.length;}
    assert(at==sizeof data&&!usb_tx_mutex->held);
    // Control/event wire payloads and scheduling stay independent of bulk pacing.
    trace_usb=true;usb_actions.clear();control_reply(123,"ACK 2\r\n");bridge_event("event\r\n");trace_usb=false;
    assert(usb_actions==std::vector<unsigned>({1,1}));frames=output_frames();assert(frames.size()==2);
    assert(frames[0].channel==2&&frames[0].sequence==123&&frames[0].length==7&&!memcmp(frames[0].payload,"ACK 2\r\n",7));
    assert(frames[1].channel==3&&frames[1].sequence==0&&frames[1].length==7&&!memcmp(frames[1].payload,"event\r\n",7));
    assert(!usb_tx_mutex->held);
}
static bool saw_event(const std::vector<vios_wire_message> &frames,const char *text){
    for(auto &f:frames)if(f.channel==3&&f.length==strlen(text)&&!memcmp(f.payload,text,f.length))return true;return false;
}
static void ota_entry_at(unsigned sector,uint32_t seq,uint32_t state,const char *label){
    memset(otadata_flash+sector*4096,0xFF,4096);uint8_t *e=otadata_flash+sector*4096;uint32_t crc=ota_entry_crc(seq);
    memcpy(e,&seq,4);if(label)memcpy(e+4,label,strlen(label));memcpy(e+24,&state,4);memcpy(e+28,&crc,4);
}
static uint32_t ota_word(unsigned sector,unsigned at){uint32_t v;memcpy(&v,otadata_flash+sector*4096+at,4);return v;}
static void check_firmware_ab(){
    now=1000; // millis() counts from ESP start; the trial window is absolute.
    assert(ota_entry_crc(1)==0x4743989AUL); // The board's real otadata seq-1 entry.
    // app0 selected by an UNDEFINED-state entry (the board today): no trial, health never rewrites otadata.
    memset(otadata_flash,0xFF,sizeof otadata_flash);ota_entry_at(0,1,0xFFFFFFFFu,nullptr);
    running_partition=esp_partition_t{"app0",0x10};ota_rollbacks=otadata_erases=0;reset_reason=0;
    assert(vios_bridge_init()&&!firmware_trial);vios_bridge_handoff();cpu_paused=false;
    incoming("F\n",60);auto f=output_frames();assert(f.size()==1&&!memcmp(f[0].payload,"FIRMWARE app0 undefined\r\n",25));
    vios_boot_health(1);vios_bridge_poll();assert(otadata_erases==0);output_frames();
    // A marked VALID entry for app1 is a trial; health 0 never confirms, health 1 clears the marker.
    ota_entry_at(1,2,ESP_OTA_IMG_VALID,"VIOS-TRIAL");running_partition=esp_partition_t{"app1",0x11};
    assert(vios_bridge_init()&&firmware_trial&&firmware_trial_sector==1);vios_bridge_handoff();cpu_paused=false;
    incoming("F\n",61);f=output_frames();assert(f.size()==1&&!memcmp(f[0].payload,"FIRMWARE app1 valid trial\r\n",27));
    vios_boot_health(0);vios_bridge_poll();assert(firmware_trial&&otadata_erases==0);output_frames();
    vios_boot_health(1);vios_bridge_poll();f=output_frames();
    assert(!firmware_trial&&otadata_flash[4096+4]==0xFF&&ota_word(1,0)==2&&ota_word(1,24)==ESP_OTA_IMG_VALID&&ota_word(1,28)==ota_entry_crc(2));
    assert(saw_event(f,"FIRMWARE confirmed\r\n")&&saw_event(f,"FIRMWARE app1 valid\r\n")&&ota_rollbacks==0);
    assert(vios_bridge_init()&&!firmware_trial); // Confirmed images stay confirmed across restarts.
    // A confirm whose write does not land is reported and the trial stays armed.
    ota_entry_at(1,2,ESP_OTA_IMG_VALID,"VIOS-TRIAL");assert(vios_bridge_init()&&firmware_trial);vios_bridge_handoff();cpu_paused=false;
    otadata_write_fails=true;vios_boot_health(1);vios_bridge_poll();f=output_frames();otadata_write_fails=false;
    assert(firmware_trial&&saw_event(f,"ERR FIRMWARE confirm failed\r\n"));
    // A trial whose CPU misses the boot deadline falls back at once with a clean failure budget.
    ota_entry_at(1,2,ESP_OTA_IMG_VALID,"VIOS-TRIAL");ESP.restarted=false;assert(vios_bridge_init()&&firmware_trial);vios_bridge_handoff();cpu_paused=false;
    vios_boot_health(0);vios_bridge_poll();now+=30001;retain_failures(1);vios_bridge_poll();f=output_frames();
    assert(ota_rollbacks==1&&ota_word(1,24)==ESP_OTA_IMG_INVALID&&retained_count==0&&!ESP.restarted&&saw_event(f,"FIRMWARE rollback: CPU boot deadline\r\n"));
    // app1 running after the bootloader fell back past an INVALID entry is not "the selected image": no trial.
    assert(vios_bridge_init()&&!firmware_trial);
    // No CPU health at all within the trial window (bootstrap error paths) also falls back.
    ota_entry_at(1,4,ESP_OTA_IMG_VALID,"VIOS-TRIAL");now=1000;assert(vios_bridge_init()&&firmware_trial);vios_bridge_handoff();cpu_boot_ready=false;boot_deadline_armed=false;
    vios_bridge_poll();assert(ota_rollbacks==1);now=FIRMWARE_TRIAL_MS;vios_bridge_poll();f=output_frames();
    assert(ota_rollbacks==2&&saw_event(f,"FIRMWARE rollback: no CPU health\r\n"));
    // A standard NEW entry (working rollback bootloader) is a trial confirmed through the IDF call.
    ota_entry_at(1,6,ESP_OTA_IMG_NEW,nullptr);now=1000;assert(vios_bridge_init()&&firmware_trial&&firmware_trial_sector<0);vios_bridge_handoff();
    vios_boot_health(1);vios_bridge_poll();output_frames();assert(!firmware_trial&&ota_word(1,24)==ESP_OTA_IMG_VALID);
    // Host-requested rollback with no other valid slot keeps this image running.
    ota_other_valid=false;assert(vios_bridge_init()&&!firmware_trial);vios_bridge_handoff();
    incoming("FR\n",62);f=output_frames();
    assert(ota_rollbacks==2&&ota_word(1,24)==ESP_OTA_IMG_VALID&&!firmware_trial&&saw_event(f,"ERR FIRMWARE no other valid slot; trial ended, use esptool\r\n"));
    ota_other_valid=true;memset(otadata_flash,0xFF,sizeof otadata_flash);running_partition=esp_partition_t{"app0",0x10};
}
static std::string control_text(const char *command,uint16_t sequence){
    // A reply longer than one frame arrives as several frames on the same sequence.
    incoming(command,sequence);auto f=output_frames();assert(!f.empty());std::string text;
    for(auto &x:f){assert(x.channel==2&&x.sequence==sequence);text.append((const char*)x.payload,x.length);}
    assert(text.size()>=2&&text.compare(text.size()-2,2,"\r\n")==0);return text;
}
static void check_stage0_instrumentation(){
    assert(control_text("T0\n",70)=="STATS RESET\r\n");
    // Injection: FIFO order, bounded queue, hex validated; works while the CPU runs.
    assert(control_text("KI4142\n",71)=="INJECTED\r\n");uint8_t key=0;
    assert(vios_injected_key(&key)&&key==0x41&&vios_injected_key(&key)&&key==0x42&&!vios_injected_key(&key));
    assert(control_text("KI4G\n",72)=="ERR inject hex\r\n");
    char big[80]="KI";for(unsigned i=0;i<32;++i)strcat(big,"61");strcat(big,"\n");
    assert(control_text(big,73)=="INJECTED\r\n"&&control_text(big,74)=="INJECTED\r\n");
    assert(control_text("KI61\n",75)=="ERR inject queue full\r\n");while(vios_injected_key(&key)){}
    // Probe: only the armed character, one pending sample at a time, key -> first matching glyph.
    assert(control_text("PR\n",76)=="PROBE n=0\r\n");assert(control_text("PA41\n",77)=="PROBE ARMED\r\n");
    for(unsigned i=1;i<=20;++i){vios_probe_glyph('A');vios_probe_key('B');vios_probe_key('A');now+=i;vios_probe_key('A');vios_probe_glyph('x');vios_probe_glyph('A');vios_probe_glyph('A');}
    assert(control_text("PR\n",78)=="PROBE n=20 min=1000 med=11000 p95=20000 max=20000\r\n");
    std::string stats=control_text("T\n",79);
    assert(stats.rfind("STATS glyphs=80 ",0)==0&&stats.find(" injected=66 console_sync=0 console_queued=0 loop_gap_max=0 host_service_max=0 console_max=0 keyboard_max=0 frames=0 vblank_start_max=0 frame_work_max=0\r\n")!=std::string::npos);
    vios_usb_console_write((const uint8_t*)"abc",3);assert(console_out_drain(16)==1);output_frames();stats=control_text("T\n",80);
    assert(stats.find("console_bytes=3 console_writes=1 ")!=std::string::npos);
    assert(control_text("T0\n",81)=="STATS RESET\r\n"&&control_text("T\n",82).rfind("STATS glyphs=0 console_bytes=0",0)==0);
    assert(control_text("G000\n",85)=="G0 STARTED\r\n"&&g0_fake_mode==0&&control_text("G081\n",86)=="ERR G0 busy\r\n");
    assert(control_text("G0R0\n",87)=="G0 BUSY\r\n");g0_fake_busy=false;
    assert(control_text("G0R0\n",88)=="G0R G0 mode 0 fake\r\n"&&control_text("G0Z\n",89)=="G0 GAP RESET\r\n");
    assert(control_text("CR05\n",83)=="CROW 05 row 5\r\n"&&control_text("CR3B\n",84).rfind("ERR",0)==0);
    assert(control_text("GS\n",85)=="GUI on windows 1 fake\r\n");
    assert(control_text("GK0000A801\n",86)=="GK used\r\n"&&control_text("GK00009F01\n",87)=="GK unused\r\n"&&gui_keys==2);
    assert(control_text("GKZZ\n",88).rfind("ERR",0)==0&&control_text("GK000QA801\n",89).rfind("ERR",0)==0);
    assert(control_text("GP00A014\n",90)=="GP 14\r\n"&&control_text("GP280014\n",91)=="GP -1\r\n");
    probe_char.store(-1);
}
static void check_line_editing(){
    bool was_paused=cpu_paused.load();cpu_paused=false;
    console_host_open=false; // earlier raw console bytes left a host line open
    // Line mode off or keys routed to an application: nothing is consumed.
    vios_line_mode(0);assert(!vios_line_key('a',0,1));vios_line_mode(3);assert(!vios_line_key('a',0,1));
    // Awaiting a line: keys are edited and echoed on the ESP (VGA feed + USB console).
    vios_line_mode(1);console_fed.clear();xQueueReset(console_queue);
    assert(vios_line_key('h',0,1)&&vios_line_key('i',0,1)&&!vios_line_key('i',0,0));
    assert(console_fed=="hi"&&console_out_drain(16)==2);auto f=output_frames();assert(f.size()==2&&f[0].channel==1);
    // Enter erases the echo and queues "hi\r" as console input for the CPU.
    assert(vios_line_key('\r',143,1));ConsoleRecord r;assert(xQueueReceive(console_queue,&r,0)&&r.length==3&&!memcmp(r.data,"hi\r",3));
    console_out_drain(16);output_frames();
    // A host line still open holds a finished physical line until it closes.
    console_host_open=true;vios_line_key('x',0,1);vios_line_key('\r',143,1);assert(uxQueueMessagesWaiting(console_queue)==0);
    console_host_open=false;line_field.poll();assert(uxQueueMessagesWaiting(console_queue)==1);
    xQueueReset(console_queue);console_out_drain(16);output_frames();
    // CPU console output marks the shown line foreign; non-editing keys pass through.
    assert(!vios_line_key(0,159,1));vios_line_mode(0);console_fed.clear();
    cpu_paused=true;vios_line_mode(1);assert(!vios_line_key('a',0,1)); // a halted CPU edits nothing
    vios_line_mode(0);cpu_paused=was_paused;
}
int main(int argc,char **argv){
    if(argc==4&&!strcmp(argv[1],"--control-frame")){
        /* Expose production control replies to the real Python resident stack.
         * Only physical ZDI registers/UART/RTOS dependencies are simulated. */
        assert(vios_bridge_init());vios_bridge_handoff();
        status=0x98;cpu_paused=true;ram_ready=true;
        memcpy(memory+0x40040,"MOS\0\1",5);
        incoming(argv[2],(uint16_t)strtoul(argv[3],nullptr,10));
        assert(!DBGSerial.output.empty());
        assert(fwrite(DBGSerial.output.data(),1,DBGSerial.output.size(),stdout)==DBGSerial.output.size());
        return 0;
    }
    assert(vios_bridge_init());assert(vios_bridge_start());assert(started_task&&task_stack>=4096&&task_priority==3&&task_core==0);
    check_usb_console_pacing();
    incoming("B\n",90);auto startup_frames=output_frames();assert(startup_frames.size()==1&&!memcmp(startup_frames[0].payload,"BOOT STARTING",13));
    incoming("I\n",91);assert(identity_reads==0);startup_frames=output_frames();assert(startup_frames.size()==1&&!memcmp(startup_frames[0].payload,"ERR bootstrap",13));
    incoming("H\n",92);assert(identity_reads==0);output_frames();
    incoming("Z\n",93);assert(ESP.restarted&&retained_count==0);output_frames();ESP.restarted=false;
    memset(memory,0xA5,sizeof memory);ports[0xB5]=0xFF;ports[0xB4]=0x80;regs[0]=0x123400;regs[7]=0x45678;
    vios_bridge_boot();vios_bridge_handoff();assert(!cpu_paused&&ram_ready&&boot_deadline_armed);assert(!memcmp(memory+0x40000,vios_image,vios_image_size));assert(memory[0x3FFFF]==0xA5);assert(memory[0xAF000]==0xA5);auto startup_frames_ready=output_frames();
    assert(native_runs==1&&discovery==std::vector<unsigned>({0xB5,0xB5,0xB5,0xB4,0xB4,0xB4}));
    // The CRC stage replaced stage0 in SRAM; only its metadata (base+256..265) and table (base+0x400..) were written.
    assert(crc_runs==1&&!memcmp(memory+internal_base(),vios_crc_stage,vios_crc_stage_size));
    assert(memory[internal_base()-1]==0xA5&&memory[internal_base()+265]==0xA5&&memory[internal_base()+266]==0xA5&&memory[internal_base()+0x800]==0xA5);
    {unsigned padded=(vios_image_size+255)/256*256;for(unsigned i=vios_image_size;i<padded;++i)assert(memory[0x40000+i]==0);assert(memory[0x40000+padded]==0xA5);}
    {const char *want="VIOREADY RAM image verified CRC ";bool saw=false;
     for(auto &f:startup_frames_ready)if(f.length>strlen(want)&&!memcmp(f.payload,want,strlen(want)))saw=true;assert(saw);}
    incoming("B\n",1);auto boot_frames=output_frames();assert(boot_frames.size()==1&&!memcmp(boot_frames[0].payload,"BOOT STARTING\r\n",15));
    vios_wire_message too_early{};too_early.channel=1;too_early.length=1;too_early.payload[0]='K';usb_dispatch(&too_early);assert(uxQueueMessagesWaiting(console_queue)==0);output_frames();
    vios_boot_health(1);vios_bridge_poll();assert(cpu_boot_ready&&!boot_deadline_armed&&boot_failures==0);
    auto health_frames=output_frames();assert(health_frames.size()==1&&health_frames[0].channel==3&&!memcmp(health_frames[0].payload,"HEALTH flag 1\r\n",15));
    incoming("B\n",2);boot_frames=output_frames();assert(boot_frames.size()==1&&!memcmp(boot_frames[0].payload,"BOOT READY\r\n",12));
    vios_wire_message line{};line.channel=1;line.sequence=30;
    uint8_t transaction[262];memset(transaction+6,' ',255);transaction[261]='\r';
    transaction[0]=0;transaction[1]=1;uint32_t crc=vios_wire_crc32(transaction+6,256);
    for(unsigned i=0;i<4;++i)transaction[2+i]=(uint8_t)(crc>>(8*i));
    line.length=240;memcpy(line.payload,transaction,240);usb_dispatch(&line);assert(VDPSerial.output.empty());
    now+=2001;vios_bridge_poll();assert(source_used==0&&VDPSerial.output.empty());output_frames();
    line.length=22;memcpy(line.payload,transaction+240,22);usb_dispatch(&line);assert(VDPSerial.output.empty());source_used=0;output_frames();
    line.length=240;memcpy(line.payload,transaction,240);usb_dispatch(&line);
    line.length=22;memcpy(line.payload,transaction+240,22);
    uint8_t damaged[VIOS_WIRE_FRAME_MAX];size_t damaged_size=vios_wire_encode(&line,damaged,sizeof damaged);
    damaged[damaged_size-2]^=1;
    for(size_t i=0;i<damaged_size;++i)DBGSerial.input.push_back(damaged[i]);
    vios_bridge_poll();assert(VDPSerial.output.empty()&&source_used==240);assert(output_frames().empty());
    now+=2001;vios_bridge_poll();assert(!source_used&&VDPSerial.output.empty());output_frames();
    line.length=240;memcpy(line.payload,transaction,240);usb_dispatch(&line);
    line.length=22;memcpy(line.payload,transaction+240,22);line.payload[0]^=1;usb_dispatch(&line);assert(VDPSerial.output.empty());output_frames();
    line.length=240;memcpy(line.payload,transaction,240);usb_dispatch(&line);
    line.sequence=31;usb_dispatch(&line);assert(!source_used&&VDPSerial.output.empty());output_frames();line.sequence=30;
    usb_dispatch(&line);line.length=22;memcpy(line.payload,transaction+240,22);usb_dispatch(&line);
    auto ack=output_frames();assert(ack.size()==1&&ack[0].channel==1&&ack[0].sequence==30&&!memcmp(ack[0].payload,"LINE OK\r\n",9));
    assert(VDPSerial.output.empty());vios_bridge_service_vdp();
    assert(VDPSerial.output.size()==260&&VDPSerial.output[0]==0xFE&&VDPSerial.output[1]==240);
    assert(!memcmp(VDPSerial.output.data()+2,transaction+6,240));assert(VDPSerial.output[242]==0xFE&&VDPSerial.output[243]==16);
    assert(!memcmp(VDPSerial.output.data()+244,transaction+246,16));VDPSerial.output.clear();
    incoming("H\n",4);assert(cpu_paused);auto frames=output_frames();assert(frames.size()==1&&frames[0].sequence==4);
    incoming("B\n",3);boot_frames=output_frames();assert(boot_frames.size()==1&&!memcmp(boot_frames[0].payload,"BOOT HALTED\r\n",13));
    usb_dispatch(&line);assert(VDPSerial.output.empty());output_frames();
    line.sequence=0;usb_dispatch(&line);assert(VDPSerial.output.empty());output_frames();
    assert(vios_cpu_is_paused());
    uint32_t saved_pc=regs[7],saved_af=regs[0];
    incoming("J\n",22);frames=output_frames();assert(frames.size()==1&&!memcmp(frames[0].payload,"ERR unsupported",15));
    incoming("K\n",23);frames=output_frames();assert(frames.size()==1&&!memcmp(frames[0].payload,"ERR unsupported",15));
    size_t prior_discovery=discovery.size();incoming("D\n",5);assert(regs[7]==saved_pc&&regs[0]==saved_af);assert(discovery.size()==prior_discovery);output_frames();
    incoming("V0400000100\n",6);frames=output_frames();assert(frames.size()==3);for(auto &f:frames)assert(f.sequence==6&&f.channel==2);assert(regs[7]==saved_pc);
    uint8_t original=memory[0x40000];incoming("W040000000100000000FF\n",7);assert(memory[0x40000]==original);output_frames();
    char command[80];uint8_t byte=0x51;snprintf(command,sizeof command,"W0400000001%08lX51\n",(unsigned long)vios_wire_crc32(&byte,1));incoming(command,8);assert(memory[0x40000]==0x51&&regs[7]==saved_pc);output_frames();
    incoming("V040",9);incoming("0000001\n",10);assert(control_used==4);frames=output_frames();assert(frames.size()==1&&frames[0].sequence==10);incoming("0000001\n",9);assert(control_used==0);output_frames();
    health_mailbox.store(1);incoming("X\n",24);frames=output_frames();assert(frames.size()==1&&!memcmp(frames[0].payload,"GO 040000",9));
    const char go_reply[]="GO 040000 USB/UART bridge 115200\r\n";
    assert(frames[0].length==strlen(go_reply)&&!memcmp(frames[0].payload,go_reply,strlen(go_reply)));
    assert(!cpu_paused&&!cpu_boot_ready&&health_mailbox.load()==-1);incoming("H\n",25);output_frames();
    incoming("Y\n",11);assert(!cpu_paused);output_frames();
    vios_boot_health(0);vios_bridge_poll();now+=15000;incoming("H\n",12);output_frames();
    now+=50000;vios_bridge_poll();assert(boot_failures==0&&cpu_paused);
    incoming("Y\n",13);output_frames();vios_vdp_parser_busy(true);now+=15001;vios_bridge_poll();assert(boot_failures==1&&cpu_paused&&ESP.restarted&&retained_count==1);output_frames();
    reset_reason=ESP_RST_SW;assert(vios_bridge_init()&&boot_failures==1);vios_bridge_boot();vios_bridge_handoff();output_frames();
    now+=30001;vios_bridge_poll();assert(boot_failures==2&&cpu_paused&&retained_count==2);output_frames();
    assert(vios_bridge_init()&&boot_failures==2);vios_bridge_boot();vios_bridge_handoff();output_frames();
    now+=30001;vios_bridge_poll();assert(boot_failures==3&&cpu_paused&&!boot_deadline_armed&&retained_count==3);output_frames();
    unsigned runs_at_limit=native_runs;assert(vios_bridge_init());vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&native_runs==runs_at_limit);output_frames();
    vios_boot_health(1);vios_bridge_poll();assert(boot_failures==0);bad_readback=true;vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!cpu_boot_ready);output_frames();
    incoming("B\n",20);boot_frames=output_frames();assert(boot_frames.size()==1&&!memcmp(boot_frames[0].payload,"BOOT HALTED\r\n",13));
    bad_readback=false;
    unsigned writes_before=external_writes,runs_before=native_runs;
    bad_completion=true;vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!ram_ready&&native_runs==runs_before+1&&external_writes==writes_before);output_frames();bad_completion=false;
    bad_map=true;vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!ram_ready&&external_writes==writes_before);output_frames();bad_map=false;
    bad_code_readback=true;runs_before=native_runs;vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!ram_ready&&native_runs==runs_before&&external_writes==writes_before);output_frames();bad_code_readback=false;
    ports[0xB4]=0;vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!ram_ready&&external_writes==writes_before);output_frames();ports[0xB4]=0x80;
    ports[0xB5]=4;vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!ram_ready&&external_writes==writes_before);output_frames();
    ports[0xB5]=0xB7;disagree_discovery=true;unsigned before_mismatch=memory_writes;
    vios_bridge_boot();vios_bridge_handoff();assert(cpu_paused&&!ram_ready&&memory_writes==before_mismatch);output_frames();disagree_discovery=false;
    // A wrong RAM CRC or a stage that never completes leaves the CPU halted and never starts VIOS.
    regs[0]&=0xFFFF00;bad_crc=true;vios_bridge_boot();vios_bridge_handoff();bad_crc=false;assert(cpu_paused&&!boot_deadline_armed);
    {auto f=output_frames();bool saw=false;for(auto &x:f)if(x.length>16&&!memcmp(x.payload,"ERR VIOS RAM CRC",16))saw=true;assert(saw);}
    regs[0]&=0xFFFF00;crc_never_finishes=true;vios_bridge_boot();vios_bridge_handoff();crc_never_finishes=false;assert(cpu_paused&&!boot_deadline_armed);
    {auto f=output_frames();bool saw=false;for(auto &x:f)if(x.length>22&&!memcmp(x.payload,"ERR CRC stage did not",21))saw=true;assert(saw);}
    regs[0]&=0xFFFF00;vios_bridge_boot();vios_bridge_handoff();assert(ram_ready&&!cpu_paused);assert(!memcmp(memory+0xB7E000,vios_crc_stage,vios_crc_stage_size));
    frames=output_frames();bool saw_stale_discovery=false;
    const char *samples="DISCOVER RAM B5 00 B7 B7 B4 B7 80 80 BASE B7E000\r\n";
    for(auto &frame:frames)if(frame.length==strlen(samples)&&!memcmp(frame.payload,samples,strlen(samples)))saw_stale_discovery=true;
    assert(saw_stale_discovery);
    vios_boot_health(1);vios_bridge_poll();output_frames();
    // Queue saturation and a stuck VDP command never prevent independent debug/restart.
    vios_vdp_parser_busy(true);vios_wire_message queued{};queued.channel=1;queued.length=1;queued.payload[0]='K';
    for(unsigned i=0;i<8;++i)usb_dispatch(&queued);assert(console_queue->records.size()==8&&VDPSerial.output.empty());
    line.sequence=44;line.length=240;memcpy(line.payload,transaction,240);usb_dispatch(&line);
    line.length=22;memcpy(line.payload,transaction+240,22);usb_dispatch(&line);
    frames=output_frames();assert(frames.size()==1&&!memcmp(frames[0].payload,"LINE ERR queue full",19));
    incoming("H\n",40);assert(cpu_paused);output_frames();incoming("S\n",41);assert(output_frames().size()==1);
    incoming("V0400000001\n",42);assert(output_frames().size()==1);
    incoming(command,44);frames=output_frames();assert(frames.size()==1&&!memcmp(frames[0].payload,"WROTE ",6));
    incoming("X\n",43);frames=output_frames();assert(frames.size()==1&&!memcmp(frames[0].payload,"ERR VDP parser busy",19));
    incoming("Z\n",21);boot_frames=output_frames();assert(retained_count==0&&ESP.restarted&&boot_frames.size()==1&&!memcmp(boot_frames[0].payload,"RESTARTING\r\n",12));
    retained_count=2;retained_inverse=~2u;retained_magic=retained_signature;assert(vios_bridge_init()&&boot_failures==2);
    retained_inverse=0;assert(vios_bridge_init()&&boot_failures==0);
    retain_failures(2);reset_reason=0;assert(vios_bridge_init()&&boot_failures==0);
    // Concurrent console/status producers must never interleave bytes within a COBS frame.
    std::thread first([]{for(unsigned i=0;i<30;++i)bridge_event("event\r\n");});
    std::atomic<bool> producing(true);
    std::thread drainer([&]{while(producing.load()||uxQueueMessagesWaiting(console_out_queue))console_out_drain(2);});
    std::thread second([]{for(unsigned i=0;i<30;++i){while(uxQueueMessagesWaiting(console_out_queue)==16)std::this_thread::yield();vios_usb_console_write((const uint8_t*)"console",7);}});
    first.join();second.join();producing=false;drainer.join();assert(output_frames().size()==60);
    check_firmware_ab();
    check_stage0_instrumentation();
    check_line_editing();
    puts("bridge boot/control/ranges/CRC/fragment sequences/health checks passed");
}
