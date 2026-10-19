/* VIOS full-VDP USB transport and ZDI RAM boot. MIT.
 * Included after the upstream VDP/zdi implementation in video.ino.
 * After bootstrap, an independent USB task owns UART0/control/ZDI. The VDP
 * task alone owns UART2 and its parser; bounded records cross between them.
 */
#pragma once
#include "vios_bridge_api.h"
#include "vios_image.h"
#include "vios_bootserve.h"
#ifndef VIOS_GUI_BOOT
#define VIOS_GUI_BOOT 0
#endif
#include "vios_line.h"
#include "vios_stage0.h"
#include "vios/internal_uart.h"
#include <atomic>
#ifndef VIOS_BRIDGE_TEST_RUNTIME
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
// Arduino would mark a pending image valid before setup(); VIOS confirms it
// only after the CPU it boots reports healthy (see firmware_confirm).
extern "C" bool verifyRollbackLater(){return true;}
#endif
static SemaphoreHandle_t usb_tx_mutex,vdp_tx_mutex;
struct ConsoleRecord {uint16_t length;uint8_t data[256];};
static QueueHandle_t console_queue,host_queue,console_out_queue;
static std::atomic<bool> parser_busy(false);
static std::atomic<bool> full_control(false);
static std::atomic<int> health_mailbox(-1);
RTC_NOINIT_ATTR static uint32_t retained_magic,retained_count,retained_inverse;
static const uint32_t retained_signature=0x56494F53;
static void retain_failures(unsigned count){retained_count=count;retained_inverse=~count;retained_magic=retained_signature;}
/* Host console input can split a line across records; a physical line from
 * the ESP editor waits until the host line is closed (CR, LF or Ctrl-C). */
static std::atomic<bool> console_host_open(false);
static bool enqueue_console(const uint8_t *data,size_t length){
    if(!console_queue||!length||length>256)return false;
    ConsoleRecord record;record.length=(uint16_t)length;memcpy(record.data,data,length);
    if(xQueueSend(console_queue,&record,0)!=pdTRUE)return false;
    uint8_t last=data[length-1];console_host_open=!(last=='\r'||last=='\n'||last==3);return true;
}

static uint8_t usb_frame[VIOS_WIRE_FRAME_MAX];
static size_t usb_used;
static bool usb_discard;
static unsigned long usb_started;
static char control_line[601];
static size_t control_used;
static uint16_t control_sequence;
static unsigned long control_started;
static uint8_t source_line[262];
static size_t source_used;
static uint16_t source_sequence;
static unsigned long source_started;
static std::atomic<bool> cpu_paused(false);
static bool ram_ready;
static uint8_t native_ram_map[4];
static bool boot_deadline_armed;
/* User mode: no ZDI wire, MOS boots the eZ80 and its stub loads the resident
 * from this firmware (E8). The ESP can neither halt nor reset the CPU. */
static bool boot_user_mode;
static std::atomic<bool> mos_phase(true);
void vios_line_mode(uint8_t flags);
bool vios_boot_user_mode(){return boot_user_mode;}
bool vios_mos_phase(){return mos_phase.load();}
void vios_mos_phase_end(){mos_phase.store(false);}
/* MOS's general poll: the CPU (re)started into MOS. Whatever the previous
 * resident set up on this side is gone with it: windows, line mode, route. */
void vios_mos_phase_begin(){mos_phase.store(true);vios_line_mode(0);vios_gui_cpu_reset();}
static bool restart_notice; // user mode after a non-power-on ESP restart: tell a running resident
static bool cpu_boot_ready;
static unsigned long boot_started,boot_paused_at;
static std::atomic<unsigned> boot_failures(0);

static void usb_send(uint8_t channel,uint16_t sequence,const uint8_t *data,size_t length) {
    do {
        struct vios_wire_message message;
        uint8_t frame[VIOS_WIRE_FRAME_MAX];
        message.channel=channel;message.sequence=sequence;
        message.length=(uint16_t)(length>VIOS_WIRE_PAYLOAD_MAX?VIOS_WIRE_PAYLOAD_MAX:length);
        memcpy(message.payload,data,message.length);
        size_t encoded=vios_wire_encode(&message,frame,sizeof frame);
        if(encoded){
            if(usb_tx_mutex)xSemaphoreTake(usb_tx_mutex,portMAX_DELAY);
            DBGSerial.write(frame,encoded);
            if(channel==VIOS_CHANNEL_CONSOLE){
                // Bound bulk USB output to one physically drained frame plus
                // one scheduler tick; leave a gap before the next frame.
                DBGSerial.flush();delay(1);
            }
            if(usb_tx_mutex)xSemaphoreGive(usb_tx_mutex);
        }
        data+=message.length;length-=message.length;
    }while(length);
}
/* GUI Stage 0 instrumentation. Times are esp_timer microseconds (uint32,
 * differences only). Producers: VDP task (glyphs, parser, USB console writes,
 * probe), control task (injection, reset). Readers tolerate torn snapshots. */
static std::atomic<uint32_t> stat_glyphs(0),stat_console_bytes(0),stat_console_writes(0),stat_injected(0);
static std::atomic<uint32_t> stat_usb_block_us(0),stat_usb_block_max(0),stat_parser_us(0),stat_parser_max(0);
static std::atomic<uint32_t> stat_console_sync(0); // 50 ms waits on a full console queue
static std::atomic<int> probe_char(-1);
static std::atomic<bool> probe_pending(false);
static std::atomic<uint32_t> probe_t0(0);
static const unsigned PROBE_SAMPLES=128;
static uint32_t probe_samples[PROBE_SAMPLES];
static std::atomic<unsigned> probe_count(0);
static uint8_t inject_ring[64];
static std::atomic<unsigned> inject_head(0),inject_tail(0);
static uint32_t stage0_us(){return (uint32_t)esp_timer_get_time();}
static void stat_max(std::atomic<uint32_t> &slot,uint32_t value){uint32_t old=slot.load();while(value>old&&!slot.compare_exchange_weak(old,value)){}}
bool vios_injected_key(uint8_t *ascii){
    unsigned tail=inject_tail.load();if(tail==inject_head.load())return false;
    *ascii=inject_ring[tail%sizeof inject_ring];inject_tail.store(tail+1);stat_injected.fetch_add(1);return true;
}
void vios_probe_key(uint8_t ascii){if((int)ascii==probe_char.load()&&!probe_pending.load()){probe_t0.store(stage0_us());probe_pending.store(true);}}
void vios_probe_glyph(char c){
    stat_glyphs.fetch_add(1);
    if(probe_pending.load()&&(int)(uint8_t)c==probe_char.load()){
        uint32_t latency=stage0_us()-probe_t0.load();unsigned n=probe_count.load();
        if(n<PROBE_SAMPLES){probe_samples[n]=latency;probe_count.store(n+1);}
        probe_pending.store(false);
    }
}
static std::atomic<uint32_t> stat_section_max[3];
extern volatile uint32_t vios_vga_isr_gap_max, vios_vga_isr_late; // VGA16 line ISR timing (vga16controller.cpp)
void vios_loop_section(unsigned index,uint32_t started_us){if(index<3)stat_max(stat_section_max[index],stage0_us()-started_us);}
void vios_probe_glyphs(const char *text,size_t count){
    if(probe_pending.load()){for(size_t i=0;i<count;++i)if((int)(uint8_t)text[i]==probe_char.load()){vios_probe_glyph(text[i]);stat_glyphs.fetch_add((uint32_t)(count-1));return;}}
    stat_glyphs.fetch_add((uint32_t)count);
}
void vios_parser_time(uint32_t us){stat_parser_us.fetch_add(us);stat_max(stat_parser_max,us);}
static void stats_reset(){
    stat_glyphs=0;stat_console_bytes=0;stat_console_writes=0;stat_injected=0;
    stat_usb_block_us=0;stat_usb_block_max=0;stat_parser_us=0;stat_parser_max=0;stat_console_sync=0;vios_g0_reset_gap();for(auto &m:stat_section_max)m=0;vios_frame_stats_reset();vios_vga_isr_gap_max=0;vios_vga_isr_late=0;
}
static void probe_report(char *line,size_t size){
    unsigned n=probe_count.load();uint32_t sorted[PROBE_SAMPLES];
    for(unsigned i=0;i<n;++i)sorted[i]=probe_samples[i];
    for(unsigned i=1;i<n;++i){uint32_t v=sorted[i];unsigned j=i;while(j&&sorted[j-1]>v){sorted[j]=sorted[j-1];--j;}sorted[j]=v;}
    if(!n){snprintf(line,size,"PROBE n=0\r\n");return;}
    snprintf(line,size,"PROBE n=%u min=%lu med=%lu p95=%lu max=%lu\r\n",n,(unsigned long)sorted[0],(unsigned long)sorted[n/2],
        (unsigned long)sorted[(n*95)/100<n?(n*95)/100:n-1],(unsigned long)sorted[n-1]);
}
/* CPU console output (VDU 23,0,E0) is queued by the VDP task and drained to
 * USB by the control task (GUI Stage 1a), so glyph drawing never waits for
 * the 115200 USB link. A full queue blocks the parser in 50 ms steps until
 * the control task frees a slot: output is never dropped or reordered. */
static void console_out_write(const uint8_t *data,size_t length){usb_send(VIOS_CHANNEL_CONSOLE,0,data,length);}
/* Stage 2a: the cooked line editor runs on the VDP task (keys arrive through
 * send_packet). Its echo goes to the VGA console and USB like CPU output; a
 * finished line joins the console input queue that also carries host lines. */
static std::atomic<bool> line_awaiting(false),line_routed(false);
static bool line_echoing;
struct LineSink : vios_line::Sink {
    void echo(const uint8_t *bytes,size_t n) override{line_echoing=true;vios_console_feed(bytes,n);vios_usb_console_write(bytes,n);line_echoing=false;}
    bool submit(const uint8_t *line,size_t n) override{return !console_host_open.load()&&enqueue_console(line,n);}
};
static LineSink line_sink;
static vios_line::Field line_field(&line_sink);
void vios_line_mode(uint8_t flags){line_awaiting=(flags&1)!=0;line_routed=(flags&2)!=0;}
bool vios_line_key(uint8_t ascii,uint8_t vk,uint8_t down){
    if(!line_awaiting.load()||line_routed.load()||cpu_paused.load())return false;
    if(!down)return false; // the CPU ignores key-ups in line mode
    return line_field.key(ascii,vk);
}
void vios_usb_console_write(const uint8_t *data,size_t length) {
    if(!line_echoing)line_field.foreign_output();
    uint32_t start=stage0_us();
    while(length) {
        ConsoleRecord record;record.length=(uint16_t)(length>VIOS_WIRE_PAYLOAD_MAX?VIOS_WIRE_PAYLOAD_MAX:length);memcpy(record.data,data,record.length);
        if(!console_out_queue)console_out_write(record.data,record.length);
        else while(xQueueSend(console_out_queue,&record,pdMS_TO_TICKS(50))!=pdTRUE)stat_console_sync.fetch_add(1);
        data+=record.length;length-=record.length;stat_console_bytes.fetch_add(record.length);stat_console_writes.fetch_add(1);
    }
    uint32_t spent=stage0_us()-start;stat_usb_block_us.fetch_add(spent);stat_max(stat_usb_block_max,spent);
}
/* Control task: send at most `budget` queued console records (each paced). */
static unsigned console_out_drain(unsigned budget){
    unsigned sent=0;ConsoleRecord record;
    while(sent<budget&&console_out_queue&&xQueueReceive(console_out_queue,&record,0)==pdTRUE){console_out_write(record.data,record.length);++sent;}
    return sent;
}
/* Host service requests from the CPU go to the Mac unpaced (small, rare). */
void vios_usb_host_write(const uint8_t *data,size_t length) {usb_send(VIOS_CHANNEL_HOST,0,data,length);}
static void control_reply(uint16_t sequence,const char *line) {usb_send(VIOS_CHANNEL_CONTROL,sequence,(const uint8_t *)line,strlen(line));}
static void bridge_event(const char *line) {usb_send(VIOS_CHANNEL_EVENT,0,(const uint8_t *)line,strlen(line));}

/* ESP firmware A/B (app0/app1). The Mac installs into the slot that is not
 * running and adds an otadata entry with state VALID plus a "VIOS-TRIAL"
 * marker in the unused label field: this board's rollback bootloader erases
 * NEW/PENDING_VERIFY entries without rewriting them, but never touches VALID.
 * A marked image (or a NEW/PENDING one under a working rollback bootloader)
 * is on trial until the CPU it booted reports health 1. A CPU boot failure,
 * or no health within FIRMWARE_TRIAL_MS of ESP start, marks it INVALID (an
 * app-side write, which lands) and boots the other slot. */
static const unsigned long FIRMWARE_TRIAL_MS=180000;
static const char firmware_trial_label[]="VIOS-TRIAL";
struct OtaEntry {uint32_t seq;uint8_t label[20];uint32_t state;uint32_t crc;};
static bool firmware_trial;
static int firmware_trial_sector=-1; // otadata sector holding our marked entry
static uint32_t ota_entry_crc(uint32_t seq){
    uint32_t crc=0;for(unsigned i=0;i<4;++i){crc^=(seq>>(8*i))&0xFF;for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xEDB88320UL:0);}
    return ~crc; // esp_rom_crc32_le(UINT32_MAX,&seq,4), as the bootloader checks it.
}
static bool ota_entry_valid(const OtaEntry &e){return e.seq!=0xFFFFFFFFUL&&e.crc==ota_entry_crc(e.seq)&&e.state!=ESP_OTA_IMG_INVALID&&e.state!=ESP_OTA_IMG_ABORTED;}
static const esp_partition_t *otadata_partition(){return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,nullptr);}
static bool otadata_read(unsigned sector,OtaEntry *e){const esp_partition_t *p=otadata_partition();return p&&esp_partition_read(p,sector*4096,e,sizeof *e)==ESP_OK;}
static bool otadata_write(unsigned sector,const OtaEntry *e){
    const esp_partition_t *p=otadata_partition();OtaEntry check;
    return p&&esp_partition_erase_range(p,sector*4096,4096)==ESP_OK&&esp_partition_write(p,sector*4096,e,sizeof *e)==ESP_OK
        &&otadata_read(sector,&check)&&!memcmp(&check,e,sizeof check);
}
static const char *firmware_state_name(esp_ota_img_states_t state){
    switch(state){case ESP_OTA_IMG_NEW:return "new";case ESP_OTA_IMG_PENDING_VERIFY:return "pending";
    case ESP_OTA_IMG_VALID:return "valid";case ESP_OTA_IMG_INVALID:return "invalid";
    case ESP_OTA_IMG_ABORTED:return "aborted";default:return "undefined";}
}
static void firmware_status(char *line,size_t size){
    const esp_partition_t *running=esp_ota_get_running_partition();esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
    if(!running||esp_ota_get_state_partition(running,&state)!=ESP_OK)state=ESP_OTA_IMG_UNDEFINED;
    snprintf(line,size,"FIRMWARE %s %s%s\r\n",running?running->label:"?",firmware_state_name(state),firmware_trial?" trial":"");
}
static void firmware_fallback(const char *reason);
/* A trial image that crashes before its CPU can report health (a fault in
 * setup, a panic or watchdog loop) never reaches the deadline checks above:
 * count its unexpected restarts in RTC memory, keyed by its otadata entry, and
 * fall back on the FIRMWARE_TRIAL_CRASHES-th. Power-on and deliberate restarts
 * neither count nor clear; confirming clears. Runs first in setup. */
static const unsigned FIRMWARE_TRIAL_CRASHES=3;
RTC_NOINIT_ATTR static uint32_t trial_magic,trial_seq,trial_crashes,trial_inverse;
static const uint32_t trial_signature=0x5654524C;
static void trial_record(uint32_t seq,uint32_t crashes){trial_seq=seq;trial_crashes=crashes;trial_inverse=~(seq^crashes);trial_magic=trial_signature;}
static bool reset_was_crash(int reason){
    return reason==ESP_RST_PANIC||reason==ESP_RST_INT_WDT||reason==ESP_RST_TASK_WDT||reason==ESP_RST_WDT||reason==ESP_RST_BROWNOUT;
}
static void firmware_trial_start(uint32_t seq){
    bool kept=trial_magic==trial_signature&&trial_seq==seq&&trial_inverse==~(seq^trial_crashes);
    uint32_t crashes=kept?trial_crashes:0;
    if(reset_was_crash(esp_reset_reason()))++crashes;
    trial_record(seq,crashes);
    if(crashes>=FIRMWARE_TRIAL_CRASHES){
        char reason[64];snprintf(reason,sizeof reason,"trial restarted %u times by crashes",(unsigned)crashes);
        trial_record(seq,0);firmware_fallback(reason);
    }
}
static void firmware_init(){
    firmware_trial=false;firmware_trial_sector=-1;
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(!running||running->subtype<ESP_PARTITION_SUBTYPE_APP_OTA_MIN||running->subtype>=ESP_PARTITION_SUBTYPE_APP_OTA_MAX)return;
    unsigned slot=running->subtype-ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    OtaEntry entries[2];int active=-1;
    for(unsigned i=0;i<2;++i)if(otadata_read(i,&entries[i])&&ota_entry_valid(entries[i])&&(active<0||entries[i].seq>entries[active].seq))active=(int)i;
    if(active<0||(entries[active].seq-1)%2!=slot)return; // Booted by fallback, not by the selecting entry.
    const OtaEntry &e=entries[active];
    if(!memcmp(e.label,firmware_trial_label,sizeof firmware_trial_label-1)){firmware_trial=true;firmware_trial_sector=active;}
    else firmware_trial=e.state==ESP_OTA_IMG_NEW||e.state==ESP_OTA_IMG_PENDING_VERIFY;
    if(firmware_trial)firmware_trial_start(e.seq);
}
static void firmware_confirm(){
    if(!firmware_trial)return;
    bool ok;
    if(firmware_trial_sector>=0){
        OtaEntry e;ok=otadata_read((unsigned)firmware_trial_sector,&e);
        if(ok){memset(e.label,0xFF,sizeof e.label);e.state=ESP_OTA_IMG_VALID;ok=otadata_write((unsigned)firmware_trial_sector,&e);}
    }else ok=esp_ota_mark_app_valid_cancel_rollback()==ESP_OK;
    if(!ok){bridge_event("ERR FIRMWARE confirm failed\r\n");return;}
    trial_magic=0;firmware_trial=false;firmware_trial_sector=-1;char line[64];firmware_status(line,sizeof line);bridge_event("FIRMWARE confirmed\r\n");bridge_event(line);
}
static void firmware_fallback(const char *reason){
    char line[96];snprintf(line,sizeof line,"FIRMWARE rollback: %s\r\n",reason);bridge_event(line);DBGSerial.flush();delay(50);
    retain_failures(0); // The fallback image starts with a clean CPU boot budget.
    if(esp_ota_mark_app_invalid_rollback_and_reboot()==ESP_OK)return; // Reboots on hardware.
    // Only returns when no other valid slot exists; keep running for USB recovery.
    firmware_trial=false;bridge_event("ERR FIRMWARE no other valid slot; trial ended, use esptool\r\n");
}
void vios_legacy_hexload_notice(){bridge_event("ERR raw hexload replaced by framed RAM control\r\n");}
static bool cpu_halt() {
    if(vdp_tx_mutex&&xSemaphoreTake(vdp_tx_mutex,pdMS_TO_TICKS(100))!=pdTRUE)return false;
    VDPSerial.flush();
    vios_zdi_write_register(ZDI_BRK_CTL,0x80);
    unsigned long start=millis();
    do {if(vios_zdi_status()&0x80){if(!cpu_paused&&boot_deadline_armed)boot_paused_at=millis();cpu_paused=true;if(vdp_tx_mutex)xSemaphoreGive(vdp_tx_mutex);return true;}}while(millis()-start<200);
    if(vdp_tx_mutex)xSemaphoreGive(vdp_tx_mutex);return false;
}
bool vios_cpu_is_paused() {return cpu_paused;}
void vios_unsupported(const char *feature) {bridge_event(feature);}
static bool cpu_is_paused() {return cpu_paused&&(vios_zdi_status()&0x90)==0x90;}
#if VIOS_ZDI_EARLY_PROBE
static uint8_t cpu_in(uint8_t port) {
    uint32_t af=vios_zdi_cpu_read(REG_AF),pc=vios_zdi_cpu_read(REG_PC);
    uint8_t op[3]={port,0x38,0xED};
    vios_zdi_write_registers(ZDI_IS2,3,op);
    uint8_t value=(uint8_t)vios_zdi_cpu_read(REG_AF);
    vios_zdi_cpu_write(REG_AF,af);vios_zdi_cpu_write(REG_PC,pc);return value;
}
#endif
static bool parse_hex(const char *s,size_t n,uint32_t *value) {
    *value=0;
    for(size_t i=0;i<n;++i) {
        unsigned char c=s[i];unsigned d;
        if(c>='0'&&c<='9')d=c-'0';else if(c>='A'&&c<='F')d=c-'A'+10;
        else if(c>='a'&&c<='f')d=c-'a'+10;else return false;
        *value=(*value<<4)|d;
    }
    return true;
}
static bool ram_range(uint32_t address,uint32_t count,uint32_t end) {return count&&count<=256&&address>=0x40000&&address<end&&count<=end-address;}
static void register_reply(uint16_t sequence) {
    char line[100];size_t n=(size_t)snprintf(line,sizeof line,"REGS %02X",vios_zdi_status());
    for(unsigned reg=0;reg<8;++reg)n+=(size_t)snprintf(line+n,sizeof(line)-n," %06lX",(unsigned long)vios_zdi_cpu_read((rw_control_t)reg));
    snprintf(line+n,sizeof(line)-n,"\r\n");control_reply(sequence,line);
}
static void run_cpu(bool fresh,uint16_t sequence) {
    if(!cpu_is_paused()){control_reply(sequence,"ERR require halted ADL CPU\r\n");return;}
    if(fresh) {
        if(parser_busy.load()||VDPSerial.available()){control_reply(sequence,"ERR VDP parser busy; use Z restart\r\n");return;}
        if(console_queue)xQueueReset(console_queue);
        health_mailbox.store(-1); // A prior image readiness message cannot authorize a fresh CPU.
        uint8_t header[5];uint32_t pc=vios_zdi_cpu_read(REG_PC);
        vios_zdi_read_memory(0x40040,5,header);vios_zdi_cpu_write(REG_PC,pc);
        if(!ram_ready||memcmp(header,"MOS\0\1",5)){control_reply(sequence,"ERR RAM image header\r\n");return;}
        vios_zdi_cpu_write(REG_PC,0x40000);
    }
    // Frozen recovery ACKs are consumed by the shared raw/resident host API.
    // Their historical 115200 identifies USB; internal rate is build metadata.
    control_reply(sequence,fresh?"GO 040000 USB/UART bridge 115200\r\n":"RESUME USB/UART bridge 115200\r\n");
    DBGSerial.flush();
    if(fresh){cpu_boot_ready=false;boot_started=millis();boot_deadline_armed=true;}
    else if(boot_deadline_armed)boot_started+=millis()-boot_paused_at;
    vios_zdi_write_register(ZDI_BRK_CTL,0);cpu_paused=false;
    pinMode(ZDI_TCK,INPUT);pinMode(ZDI_TDI,INPUT);
}
static void line_reply(uint16_t sequence,const char *text);
static void control_command(uint16_t sequence,const char *command,size_t length) {
    char reply[600];
    if(length==1&&command[0]=='Z') {
        // Force restart abandons queued UART bytes; do not wait on a wedged VDP lock.
        // During bootstrap main still owns ZDI, so the restricted supervisor only resets ESP.
        if(full_control.load()){vios_zdi_write_register(ZDI_BRK_CTL,0x80);cpu_paused=true;}
        retain_failures(0);boot_failures=0;control_reply(sequence,"RESTARTING\r\n");DBGSerial.flush();delay(50);ESP.restart();return;
    }
    if(!full_control.load()){
        control_reply(sequence,length==1&&command[0]=='B'?"BOOT STARTING\r\n":"ERR bootstrap owns CPU; only B/Z available\r\n");return;
    }
    if(length==1&&command[0]=='2'){control_reply(sequence,"ACK 2\r\n");return;}
    if(length==1&&command[0]=='F'){firmware_status(reply,sizeof reply);control_reply(sequence,reply);return;}
    // Stage 0 instrumentation; safe while the CPU runs.
    uint32_t stage0_value;
    if(length==1&&command[0]=='T'){
        uint32_t frame_stats[3];vios_frame_stats(frame_stats,frame_stats+1,frame_stats+2);
        snprintf(reply,sizeof reply,"STATS glyphs=%lu console_bytes=%lu console_writes=%lu usb_block_us=%lu usb_block_max=%lu parser_us=%lu parser_max=%lu injected=%lu console_sync=%lu console_queued=%u loop_gap_max=%lu host_service_max=%lu console_max=%lu keyboard_max=%lu frames=%lu vblank_start_max=%lu frame_work_max=%lu vga_isr_gap_max_us=%lu vga_isr_late=%lu\r\n",
            (unsigned long)stat_glyphs.load(),(unsigned long)stat_console_bytes.load(),(unsigned long)stat_console_writes.load(),
            (unsigned long)stat_usb_block_us.load(),(unsigned long)stat_usb_block_max.load(),(unsigned long)stat_parser_us.load(),
            (unsigned long)stat_parser_max.load(),(unsigned long)stat_injected.load(),(unsigned long)stat_console_sync.load(),
            console_out_queue?(unsigned)uxQueueMessagesWaiting(console_out_queue):0u,(unsigned long)vios_g0_gap(),
            (unsigned long)stat_section_max[0].load(),(unsigned long)stat_section_max[1].load(),(unsigned long)stat_section_max[2].load(),
            (unsigned long)frame_stats[0],(unsigned long)frame_stats[1],(unsigned long)frame_stats[2],
            (unsigned long)(vios_vga_isr_gap_max/240u),(unsigned long)vios_vga_isr_late);
        control_reply(sequence,reply);return;
    }
    if(length==2&&command[0]=='T'&&command[1]=='0'){stats_reset();control_reply(sequence,"STATS RESET\r\n");return;}
    /* GUI: GS status; GK<ascii><mods><vk><down> a key for the window manager;
     * GP<x:3><y:3> a framebuffer pixel's colour index (all hex). */
    if(length==2&&command[0]=='G'&&command[1]=='S'){
        char text[160];vios_gui_status(text,sizeof text);snprintf(reply,sizeof reply,"%s\r\n",text);control_reply(sequence,reply);return;
    }
    if(length==10&&command[0]=='G'&&command[1]=='K'){
        uint32_t k[4];for(unsigned i=0;i<4;++i)if(!parse_hex(command+2+2*i,2,&k[i])){control_reply(sequence,"ERR GK hex\r\n");return;}
        control_reply(sequence,vios_gui_key((uint8_t)k[0],(uint8_t)k[1],(uint8_t)k[2],(uint8_t)k[3])?"GK used\r\n":"GK unused\r\n");return;
    }
    if(length==8&&command[0]=='G'&&command[1]=='P'){
        uint32_t x,y;if(!parse_hex(command+2,3,&x)||!parse_hex(command+5,3,&y)){control_reply(sequence,"ERR GP hex\r\n");return;}
        snprintf(reply,sizeof reply,"GP %d\r\n",vios_gui_pixel((unsigned)x,(unsigned)y));control_reply(sequence,reply);return;
    }
    if(length==4&&command[0]=='P'&&command[1]=='A'&&parse_hex(command+2,2,&stage0_value)){
        probe_char.store(-1);probe_pending.store(false);probe_count.store(0);probe_char.store((int)stage0_value);
        control_reply(sequence,"PROBE ARMED\r\n");return;
    }
    if(length==2&&command[0]=='P'&&command[1]=='R'){probe_report(reply,sizeof reply);control_reply(sequence,reply);return;}
    // GUI gate G0 probe: G0<mm> runs it on the VDP task, G0R<n> reads result line n, G0Z resets the loop gap.
    if(length==4&&command[0]=='G'&&command[1]=='0'&&parse_hex(command+2,2,&stage0_value)){
        control_reply(sequence,vios_g0_request((uint8_t)stage0_value)?"G0 STARTED\r\n":"ERR G0 busy\r\n");return;
    }
    if(length==3&&command[0]=='G'&&command[1]=='0'&&command[2]=='Z'){vios_g0_reset_gap();control_reply(sequence,"G0 GAP RESET\r\n");return;}
    if(length==4&&command[0]=='G'&&command[1]=='0'&&command[2]=='R'&&command[3]>='0'&&command[3]<='5'){
        if(vios_g0_busy()){control_reply(sequence,"G0 BUSY\r\n");return;}
        snprintf(reply,sizeof reply,"G0R %s\r\n",vios_g0_line((unsigned)(command[3]-'0')));control_reply(sequence,reply);return;
    }
    // Exact console text from the ESP terminal (no pixel matching), row hh 00..3A.
    if(length==4&&command[0]=='C'&&command[1]=='R'&&parse_hex(command+2,2,&stage0_value)&&stage0_value<=58){
        char text[96];vios_console_row((unsigned)stage0_value,text,sizeof text);
        snprintf(reply,sizeof reply,"CROW %02X %s\r\n",(unsigned)stage0_value,text);control_reply(sequence,reply);return;
    }
    if(length>=4&&length<=2+2*32&&length%2==0&&command[0]=='K'&&command[1]=='I') {
        uint8_t keys[32];unsigned count=(unsigned)(length-2)/2;
        for(unsigned i=0;i<count;++i){if(!parse_hex(command+2+2*i,2,&stage0_value)){control_reply(sequence,"ERR inject hex\r\n");return;}keys[i]=(uint8_t)stage0_value;}
        unsigned head=inject_head.load();
        if(head+count-inject_tail.load()>sizeof inject_ring){control_reply(sequence,"ERR inject queue full\r\n");return;}
        for(unsigned i=0;i<count;++i)inject_ring[(head+i)%sizeof inject_ring]=keys[i];
        inject_head.store(head+count);control_reply(sequence,"INJECTED\r\n");return;
    }
    if(length==2&&command[0]=='F'&&command[1]=='R'){control_reply(sequence,"FIRMWARE ROLLBACK\r\n");firmware_trial=true;firmware_fallback("host request");return;}
    if(length==1&&command[0]=='I') {
        snprintf(reply,sizeof reply,"ID %04X REV %02X STAT %02X\r\n",vios_zdi_id(),vios_zdi_revision(),vios_zdi_status());control_reply(sequence,reply);return;
    }
    if(length==1&&command[0]=='B') {
        control_reply(sequence,boot_user_mode&&!cpu_paused&&!cpu_boot_ready&&!boot_deadline_armed?"BOOT USER waiting for the MOS stub\r\n":
            cpu_paused||(!cpu_boot_ready&&!boot_deadline_armed)?"BOOT HALTED\r\n":boot_deadline_armed?"BOOT STARTING\r\n":"BOOT READY\r\n");return;
    }
    if(length==1&&command[0]=='H') {
        if(source_used){line_reply(source_sequence,"LINE ERR halted\r\n");source_used=0;}
        if(!cpu_halt())control_reply(sequence,"ERR CPU halt timeout\r\n");else register_reply(sequence);return;
    }
    if(!cpu_is_paused()){control_reply(sequence,"ERR require halted ADL CPU\r\n");return;}
#if VIOS_ZDI_EARLY_PROBE
    if(length==1&&(command[0]=='J'||command[0]=='K')) {
        uint32_t saved[8];for(unsigned reg=0;reg<8;++reg)saved[reg]=vios_zdi_cpu_read((rw_control_t)reg);
        bool split=command[0]=='K';
        uint8_t load[2]={0x42,0x3E},input[3]={CS0_LBR,0x38,0xED};
        uint32_t nop=0,preload=0,in_nop=0,in_preload=0;
        if(split){
            vios_zdi_write_register(ZDI_IS0,0);nop=vios_zdi_cpu_read(REG_AF);
            vios_zdi_write_register(ZDI_IS1,load[0]);preload=vios_zdi_cpu_read(REG_AF);
            vios_zdi_write_register(ZDI_IS0,load[1]);
        }else vios_zdi_write_registers(ZDI_IS1,2,load);
        delayMicroseconds(10);uint32_t loaded=vios_zdi_cpu_read(REG_AF);
        if(split){
            vios_zdi_write_register(ZDI_IS0,0);in_nop=vios_zdi_cpu_read(REG_AF);
            vios_zdi_write_register(ZDI_IS2,input[0]);vios_zdi_write_register(ZDI_IS1,input[1]);
            in_preload=vios_zdi_cpu_read(REG_AF);vios_zdi_write_register(ZDI_IS0,input[2]);
        }else vios_zdi_write_registers(ZDI_IS2,3,input);
        delayMicroseconds(10);uint32_t read=vios_zdi_cpu_read(REG_AF);
        for(unsigned reg=0;reg<8;++reg)vios_zdi_cpu_write((rw_control_t)reg,saved[reg]);
        if(split)snprintf(reply,sizeof reply,"INJECT SPLIT NOP %06lX PRE %06lX LD %06lX INNOP %06lX INPRE %06lX IN %06lX\r\n",
            (unsigned long)nop,(unsigned long)preload,(unsigned long)loaded,(unsigned long)in_nop,(unsigned long)in_preload,(unsigned long)read);
        else snprintf(reply,sizeof reply,"INJECT LD %06lX IN %06lX\r\n",(unsigned long)loaded,(unsigned long)read);
        control_reply(sequence,reply);return;
    }
#endif
    if(length==1&&command[0]=='S'){register_reply(sequence);return;}
    if(length==1&&command[0]=='D') {
#if VIOS_ZDI_EARLY_PROBE
        uint8_t map[4]={cpu_in(CS0_LBR),cpu_in(CS0_UBR),cpu_in(CS0_CTL),cpu_in(CS0_BMC)};
#else
        if(!ram_ready){control_reply(sequence,"ERR no verified native RAM mapping\r\n");return;}
        const uint8_t *map=native_ram_map;
#endif
        snprintf(reply,sizeof reply,"CS0 %02X %02X %02X %02X\r\n",map[0],map[1],map[2],map[3]);control_reply(sequence,reply);return;
    }
    if(length==1&&(command[0]=='X'||command[0]=='Y')){run_cpu(command[0]=='X',sequence);return;}
    uint32_t address,count,crc;
    if(length>=11&&(command[0]=='V'||command[0]=='W')&&parse_hex(command+1,6,&address)&&parse_hex(command+7,4,&count)) {
        bool write=command[0]=='W';
        if(!ram_ready||!ram_range(address,count,write?0xAF000:0xC0000)){control_reply(sequence,"ERR RAM range/mapping\r\n");return;}
        uint8_t data[256];uint32_t pc=vios_zdi_cpu_read(REG_PC);
        if(write) {
            if(length!=19+count*2||!parse_hex(command+11,8,&crc)){control_reply(sequence,"ERR syntax\r\n");return;}
            for(size_t i=0;i<count;++i){uint32_t byte;if(!parse_hex(command+19+i*2,2,&byte)){control_reply(sequence,"ERR hex\r\n");return;}data[i]=(uint8_t)byte;}
            if(vios_wire_crc32(data,count)!=crc){control_reply(sequence,"ERR CRC\r\n");return;}
            vios_zdi_write_memory(address,count,data);vios_zdi_cpu_write(REG_PC,pc);
            snprintf(reply,sizeof reply,"WROTE %06lX %04lX %08lX\r\n",(unsigned long)address,(unsigned long)count,(unsigned long)crc);
        }else {
            if(length!=11){control_reply(sequence,"ERR syntax\r\n");return;}
            vios_zdi_read_memory(address,(uint16_t)count,data);vios_zdi_cpu_write(REG_PC,pc);
            crc=vios_wire_crc32(data,count);
            size_t used=(size_t)snprintf(reply,sizeof reply,"MEM %06lX %04lX %08lX ",(unsigned long)address,(unsigned long)count,(unsigned long)crc);
            static const char hex[]="0123456789ABCDEF";
            for(size_t i=0;i<count;++i){reply[used++]=hex[data[i]>>4];reply[used++]=hex[data[i]&15];}
            reply[used++]='\r';reply[used++]='\n';reply[used]=0;
        }
        control_reply(sequence,reply);return;
    }
    control_reply(sequence,"ERR unsupported command\r\n");
}
unsigned long vios_vdp_wait_running(){
    unsigned long start=millis();while(cpu_paused.load())delay(1);return millis()-start;
}
void vios_vdp_parser_busy(bool busy){parser_busy.store(busy);}
void vios_vdp_tx_begin(){
    for(;;){vios_vdp_wait_running();if(vdp_tx_mutex)xSemaphoreTake(vdp_tx_mutex,portMAX_DELAY);
        if(!cpu_paused.load())return;if(vdp_tx_mutex)xSemaphoreGive(vdp_tx_mutex);}
}
/* No flush here: the UART TX ring keeps writers in order, and waiting for the
 * CPU's flow control would stall the VDP loop. cpu_halt() flushes under this
 * lock before it halts, so a halt never strands queued bytes. */
void vios_vdp_tx_end(){if(vdp_tx_mutex)xSemaphoreGive(vdp_tx_mutex);}
void vios_bridge_service_vdp(){
    if(cpu_paused.load())return;
    if(restart_notice){   // MOS and the stub skip unknown packets; a resident resyncs
        restart_notice=false;uint8_t reason=(uint8_t)esp_reset_reason();
        vios_vdp_tx_begin();VDPSerial.write((uint8_t)VIOS_PACKET_ESP_RESTART);VDPSerial.write((uint8_t)1);VDPSerial.write(reason);vios_vdp_tx_end();
    }
    line_field.poll(); // a finished physical line waiting for queue room or a closed host line
    bool console=console_queue&&uxQueueMessagesWaiting(console_queue),host=host_queue&&uxQueueMessagesWaiting(host_queue);
    if(!console&&!host)return;
    ConsoleRecord record;
    vios_vdp_tx_begin();
    if(console&&xQueueReceive(console_queue,&record,0)==pdTRUE){
        size_t at=0;while(at<record.length){size_t count=record.length-at;if(count>240)count=240;
            VDPSerial.write((uint8_t)VIOS_PACKET_CONSOLE);VDPSerial.write((uint8_t)count);
            VDPSerial.write(record.data+at,count);at+=count;}
    }
    /* One host reply is one packet (<=240 bytes), never split. */
    if(host&&xQueueReceive(host_queue,&record,0)==pdTRUE&&record.length<=240){
        VDPSerial.write((uint8_t)VIOS_PACKET_HOST);VDPSerial.write((uint8_t)record.length);VDPSerial.write(record.data,record.length);
    }
    vios_vdp_tx_end();
}
static void line_reply(uint16_t sequence,const char *text) {
    usb_send(VIOS_CHANNEL_CONSOLE,sequence,(const uint8_t *)text,strlen(text));
}
static void source_fragment(const struct vios_wire_message *message) {
    uint16_t sequence=message->sequence;
    if(!full_control.load()||(!cpu_boot_ready&&health_mailbox.load()!=1)){source_used=0;line_reply(sequence,"LINE ERR boot starting\r\n");return;}
    if(cpu_paused){source_used=0;line_reply(sequence,"LINE ERR paused\r\n");return;}
    if(source_used&&source_sequence!=sequence) {
        line_reply(source_sequence,"LINE ERR interleaved\r\n");source_used=0;
        line_reply(sequence,"LINE ERR interleaved\r\n");return;
    }
    if(!source_used){source_sequence=sequence;source_started=millis();}
    if(!message->length||message->length>sizeof(source_line)-source_used) {
        source_used=0;line_reply(sequence,"LINE ERR length\r\n");return;
    }
    memcpy(source_line+source_used,message->payload,message->length);source_used+=message->length;
    if(source_used<6)return;
    size_t length=source_line[0]|((size_t)source_line[1]<<8);
    if(!length||length>256||source_used>length+6) {
        source_used=0;line_reply(sequence,"LINE ERR length\r\n");return;
    }
    if(source_used<length+6)return;
    uint32_t crc=(uint32_t)source_line[2]|((uint32_t)source_line[3]<<8)|((uint32_t)source_line[4]<<16)|((uint32_t)source_line[5]<<24);
    source_used=0;
    if(source_line[length+5]!='\r'||vios_wire_crc32(source_line+6,length)!=crc) {
        line_reply(sequence,"LINE ERR CRC/terminator\r\n");return;
    }
    // A single CR-terminated ASCII line: embedded terminators cannot execute a prefix.
    for(size_t i=6;i<length+5;++i)if(source_line[i]<32||source_line[i]>126) {
        line_reply(sequence,"LINE ERR ASCII\r\n");return;
    }
    if(!enqueue_console(source_line+6,length)){line_reply(sequence,"LINE ERR queue full\r\n");return;}
    line_reply(sequence,"LINE OK\r\n");
}
static void usb_dispatch(const struct vios_wire_message *message) {
    if(message->channel==VIOS_CHANNEL_CONSOLE) {
        if(message->sequence){source_fragment(message);return;}
        if(!full_control.load()||(!cpu_boot_ready&&health_mailbox.load()!=1)){bridge_event("ERR console boot starting\r\n");return;}
        if(cpu_paused){bridge_event("ERR console while paused\r\n");return;}
        if(source_used){line_reply(source_sequence,"LINE ERR interleaved\r\n");source_used=0;return;}
        if(!enqueue_console(message->payload,message->length))bridge_event("ERR console queue full\r\n");return;
    }
    if(message->channel==VIOS_CHANNEL_HOST) {
        if(!host_queue||!message->length){bridge_event("ERR host reply invalid\r\n");return;}
        ConsoleRecord record;record.length=message->length;memcpy(record.data,message->payload,message->length);
        if(xQueueSend(host_queue,&record,0)!=pdTRUE)bridge_event("ERR host queue full\r\n");return;
    }
    if(message->channel!=VIOS_CHANNEL_CONTROL)return;
    if(control_used&&control_sequence!=message->sequence){control_reply(message->sequence,"ERR command sequence busy\r\n");return;}
    if(!control_used){control_sequence=message->sequence;control_started=millis();}
    for(size_t i=0;i<message->length;++i) {
        uint8_t byte=message->payload[i];
        if(byte=='\n') {control_line[control_used]=0;control_command(control_sequence,control_line,control_used);control_used=0;}
        else if(byte!='\r') {
            if(byte<32||byte>126||control_used==600){control_used=0;control_reply(message->sequence,"ERR command length/ASCII\r\n");return;}
            control_line[control_used++]=(char)byte;
        }
    }
}
void vios_bridge_poll() {
    if(source_used&&millis()-source_started>2000){line_reply(source_sequence,"LINE ERR timeout\r\n");source_used=0;}
    if(full_control.load()){
    int health=health_mailbox.exchange(-1);
    if(health>=0){
        char event[32];snprintf(event,sizeof event,"HEALTH flag %u\r\n",(unsigned)health);bridge_event(event);
        if(health==0){cpu_boot_ready=false;boot_started=millis();boot_deadline_armed=true;if(cpu_paused)boot_paused_at=boot_started;}
        else {boot_deadline_armed=false;cpu_boot_ready=true;boot_failures=0;retain_failures(0);firmware_confirm();}
    }
    if(boot_deadline_armed&&!cpu_paused&&millis()-boot_started>=30000) {
        boot_deadline_armed=false;cpu_boot_ready=false;
        if(boot_user_mode){   // the ESP cannot reload the CPU: report, or leave a trial image
            if(firmware_trial){firmware_fallback("CPU boot deadline");return;}
            bridge_event("ERR CPU boot deadline in user mode; reset the board\r\n");return;
        }
        ++boot_failures;retain_failures(boot_failures);
        bool halted=cpu_halt();
        if(firmware_trial){firmware_fallback("CPU boot deadline");return;}
        if(boot_failures>=3)bridge_event(halted?"ERR three failed CPU boots; halted, USB recovery available\r\n":"ERR three failed CPU boots; halt failed, use Z\r\n");
        else {char event[80];snprintf(event,sizeof event,"BOOT failure %u/3; restarting ESP recovery\r\n",boot_failures.load());bridge_event(event);DBGSerial.flush();delay(50);ESP.restart();}
    }
    }
    if(firmware_trial&&!cpu_boot_ready&&millis()>=FIRMWARE_TRIAL_MS){firmware_fallback("no CPU health");return;}
    console_out_drain(2); // Bounded so USB input and deadlines keep being serviced.
    if(control_used&&millis()-control_started>2000){control_reply(control_sequence,"ERR command timeout\r\n");control_used=0;}
    if(usb_used&&millis()-usb_started>2000){usb_used=0;usb_discard=true;}
    unsigned usb_budget=1024; // Yield/check deadlines even under continuous host input.
    while(usb_budget&&DBGSerial.available()) {
        --usb_budget;uint8_t byte=(uint8_t)DBGSerial.read();
        if(!byte) {
            struct vios_wire_message message;
            if(!usb_discard&&usb_used&&vios_wire_decode(usb_frame,usb_used,&message))usb_dispatch(&message);
            usb_used=0;usb_discard=false;
        }else if(!usb_discard) {
            if(!usb_used)usb_started=millis();
            if(usb_used==sizeof usb_frame){usb_used=0;usb_discard=true;}
            else usb_frame[usb_used++]=byte;
        }
    }
}
void vios_boot_health(uint8_t flag) {
    if(flag<=1)health_mailbox.store(flag);
    else bridge_event("ERR boot health flag\r\n");
}
bool vios_bridge_init(){
    health_mailbox.store(-1);parser_busy.store(false);full_control.store(false);
    usb_used=control_used=source_used=0;usb_discard=false;
    usb_tx_mutex=xSemaphoreCreateMutex();vdp_tx_mutex=xSemaphoreCreateMutex();
    console_queue=xQueueCreate(8,sizeof(ConsoleRecord));host_queue=xQueueCreate(2,sizeof(ConsoleRecord));
    if(!console_out_queue)console_out_queue=xQueueCreate(16,sizeof(ConsoleRecord));
    if(esp_reset_reason()!=ESP_RST_SW||retained_magic!=retained_signature||retained_count>3||retained_inverse!=~retained_count)retain_failures(0);
    boot_failures=retained_count;firmware_init();
    return usb_tx_mutex&&vdp_tx_mutex&&console_queue&&host_queue&&console_out_queue;
}
static void control_task(void*){for(;;){vios_bridge_poll();vTaskDelay(pdMS_TO_TICKS(1));}}
void vios_bridge_handoff(){full_control.store(true);}
bool vios_bridge_start(){return xTaskCreatePinnedToCore(control_task,"vios-usb",6144,nullptr,3,nullptr,0)==pdPASS;}
void vios_bridge_probe() {
    pinMode(ZDI_TCK,OUTPUT);digitalWrite(ZDI_TCK,HIGH);pinMode(ZDI_TDI,INPUT);
    bridge_event("PROBE early Arduino ZDI; no VDP/audio/PS2/CS0 writes/RAM upload\r\n");
    if(!cpu_halt()){bridge_event("ERR probe halt\r\n");return;}
    vios_zdi_cpu_read(SET_ADL);vios_zdi_di();
}
static uint32_t boot_crc_update(uint32_t crc,const uint8_t *data,size_t n){
    while(n--){crc^=*data++;for(unsigned b=0;b<8;++b)crc=(crc>>1)^((crc&1)?0xEDB88320UL:0);}return crc;
}
/* Run the native CRC stage over `blocks` uploaded RAM blocks and compare with
 * the CRC the ESP computed while sending them. Stage code, table and metadata
 * live in the on-chip SRAM window stage0 proved; each is readback-verified. */
static bool boot_crc_check(uint32_t internal,unsigned blocks,uint32_t expected){
    static uint8_t table[1024];uint8_t verify[256];
    for(unsigned n=0;n<256;++n){uint32_t c=n;for(unsigned b=0;b<8;++b)c=(c>>1)^((c&1)?0xEDB88320UL:0);for(unsigned k=0;k<4;++k)table[256*k+n]=(uint8_t)(c>>(8*k));}
    for(unsigned k=0;k<4;++k){
        vios_zdi_write_memory(internal+0x400+256*k,256,table+256*k);vios_zdi_read_memory(internal+0x400+256*k,256,verify);
        if(memcmp(table+256*k,verify,256)){bridge_event("ERR CRC table readback; CPU left halted\r\n");return false;}
    }
    uint8_t stage[256],metadata[10];memset(metadata,0,sizeof metadata);metadata[3]=(uint8_t)blocks;metadata[4]=(uint8_t)(blocks>>8);
    memcpy_P(stage,vios_crc_stage,vios_crc_stage_size);
    vios_zdi_write_memory(internal,vios_crc_stage_size,stage);vios_zdi_read_memory(internal,vios_crc_stage_size,verify);
    if(memcmp(stage,verify,vios_crc_stage_size)){bridge_event("ERR CRC stage readback; CPU left halted\r\n");return false;}
    vios_zdi_write_memory(internal+256,sizeof metadata,metadata);vios_zdi_read_memory(internal+256,sizeof metadata,verify);
    if(memcmp(metadata,verify,sizeof metadata)){bridge_event("ERR CRC metadata readback; CPU left halted\r\n");return false;}
    vios_zdi_cpu_write(REG_IX,0x40000);vios_zdi_cpu_write(REG_IY,internal+256);vios_zdi_cpu_write(REG_HL,internal+0x400);vios_zdi_cpu_write(REG_PC,internal);
    // ~0.4 s at 18.432 MHz (bound 6 s). Halt, look for the A5 completion, resume if not yet done.
    for(unsigned tries=0;;++tries) {
        vios_zdi_write_register(ZDI_BRK_CTL,0);cpu_paused=false;delay(100);
        if(!cpu_halt()){bridge_event("ERR CRC stage halt timeout\r\n");return false;}
        // ZDI memory reads go through PC: save it so an unfinished stage resumes where it stopped.
        uint32_t pc=vios_zdi_cpu_read(REG_PC);
        vios_zdi_read_memory(internal+256,sizeof metadata,metadata);
        if(metadata[9]==0xA5)break;
        vios_zdi_cpu_write(REG_PC,pc);
        if(tries==60){bridge_event("ERR CRC stage did not finish; CPU left halted\r\n");return false;}
    }
    uint32_t got=(uint32_t)metadata[5]|((uint32_t)metadata[6]<<8)|((uint32_t)metadata[7]<<16)|((uint32_t)metadata[8]<<24);
    if(got!=expected){
        char line[80];snprintf(line,sizeof line,"ERR VIOS RAM CRC %08lX expected %08lX; CPU left halted\r\n",(unsigned long)got,(unsigned long)expected);bridge_event(line);return false;
    }
    return true;
}
unsigned vios_boot_manifest_reply(uint8_t *out){
    if(boot_user_mode&&!boot_deadline_armed){cpu_boot_ready=false;boot_started=millis();boot_deadline_armed=true;bridge_event("BOOT stub loading the resident over the UART\r\n");}
    return vios_boot_manifest(out,vios_image_size,vios_image_crc,VIOS_GUI_BOOT,VIOS_INTERNAL_BAUD);
}
unsigned vios_boot_block_reply(unsigned index,uint8_t *out){return vios_boot_block(out,vios_image,vios_image_size,index);}
void vios_bridge_boot() {
    cpu_boot_ready=false;boot_deadline_armed=false;
    ram_ready=false;memset(native_ram_map,0,sizeof native_ram_map);
    pinMode(ZDI_TCK,OUTPUT);digitalWrite(ZDI_TCK,HIGH);pinMode(ZDI_TDI,INPUT);
    {char boot_event[48];snprintf(boot_event,sizeof boot_event,"BOOT VIOS VDP 1.04 mux-v1 zdi-edge %uus\r\n",(unsigned)VIOS_ZDI_EDGE_US);bridge_event(boot_event);}
    char retry_event[48];snprintf(retry_event,sizeof retry_event,"BOOT retained failures %u\r\n",boot_failures.load());bridge_event(retry_event);
    firmware_status(retry_event,sizeof retry_event);bridge_event(retry_event);
    if(vios_zdi_id()!=0x0007||vios_zdi_revision()!=0xAA){   // no wire: user mode
        boot_user_mode=true;pinMode(ZDI_TCK,INPUT);pinMode(ZDI_TDI,INPUT);
        restart_notice=esp_reset_reason()!=ESP_RST_POWERON;
        bridge_event("BOOT user mode: no ZDI; MOS and its stub load the resident\r\n");return;
    }
    boot_user_mode=false;
    if(!cpu_halt()){bridge_event("ERR CPU identity/halt; recovery control available\r\n");return;}
    if(boot_failures>=3){bridge_event("ERR retained boot failure limit; halted, use Z\r\n");return;}
    vios_zdi_cpu_read(SET_ADL);vios_zdi_di();
    // F92 hardware: the first IN0 after a port/opcode change returns prior A.
    // Prime each port, then require two matching samples before any RAM write.
    uint8_t upper_read[3],control_read[3];
    for(unsigned i=0;i<3;++i)upper_read[i]=vios_zdi_boot_in0(0xB5);
    for(unsigned i=0;i<3;++i)control_read[i]=vios_zdi_boot_in0(0xB4);
    vios_zdi_write_register(ZDI_IS0,0); // Neutralize discovery before CPU register/memory access.
    uint8_t upper=upper_read[2],control=control_read[2];
    uint32_t internal=((uint32_t)upper<<16)|0xE000;
    char discovery_event[120];snprintf(discovery_event,sizeof discovery_event,
        "DISCOVER RAM B5 %02X %02X %02X B4 %02X %02X %02X BASE %06lX\r\n",
        upper_read[0],upper_read[1],upper_read[2],control_read[0],control_read[1],control_read[2],(unsigned long)internal);bridge_event(discovery_event);
    if(upper_read[1]!=upper_read[2]||control_read[1]!=control_read[2]){
        bridge_event("ERR unstable internal RAM discovery; no RAM writes\r\n");return;
    }
    if(!(control&0x80)||(internal<0xAF000&&internal+0x2000>0x40000)) {
        bridge_event("ERR internal RAM mapping; recovery control available\r\n");return;
    }
    uint8_t stage[256],stage_verify[256],metadata[5]={0};
    memcpy_P(stage,vios_stage0,vios_stage0_size);
    vios_zdi_write_memory(internal+256,sizeof metadata,metadata);
    vios_zdi_read_memory(internal+256,sizeof metadata,stage_verify);
    if(memcmp(metadata,stage_verify,sizeof metadata)){bridge_event("ERR stage0 metadata clear readback\r\n");return;}
    vios_zdi_write_memory(internal,vios_stage0_size,stage);
    vios_zdi_read_memory(internal,vios_stage0_size,stage_verify);
    if(memcmp(stage,stage_verify,vios_stage0_size)){bridge_event("ERR stage0 code readback\r\n");return;}
    vios_zdi_cpu_write(REG_HL,internal+256);vios_zdi_cpu_write(REG_PC,internal);
    vios_zdi_write_register(ZDI_BRK_CTL,0);cpu_paused=false;
    delay(2);
    if(!cpu_halt()){bridge_event("ERR native stage0 halt timeout\r\n");return;}
    vios_zdi_read_memory(internal+256,sizeof metadata,metadata);
    static const uint8_t expected[5]={0x04,0x0B,0x08,0x01,0xA5};
    if(memcmp(metadata,expected,sizeof expected)){bridge_event("ERR native stage0 mapping/completion\r\n");return;}
    memcpy(native_ram_map,metadata,sizeof native_ram_map);ram_ready=true;
    char native_event[100];snprintf(native_event,sizeof native_event,"NATIVE RAM %06lX CS0 %02X %02X %02X %02X DONE %02X\r\n",(unsigned long)internal,metadata[0],metadata[1],metadata[2],metadata[3],metadata[4]);bridge_event(native_event);
    // Upload whole 256-byte blocks (the tail zero-padded) without per-block
    // readback; the CPU then CRCs the RAM it received (arch/ez80/agon_crc.s).
    uint8_t block[256];uint32_t crc=0xFFFFFFFFUL;unsigned blocks=0;
    for(unsigned offset=0;offset<vios_image_size;offset+=sizeof block,++blocks) {
        unsigned count=vios_image_size-offset;if(count>sizeof block)count=sizeof block;
        memcpy_P(block,vios_image+offset,count);memset(block+count,0,sizeof block-count);
        vios_zdi_write_memory(0x40000+offset,sizeof block,block);crc=boot_crc_update(crc,block,sizeof block);
        if(blocks%16==15)delay(1); // Let same-core audio/idle tasks run.
    }
    crc=~crc;
    if(!boot_crc_check(internal,blocks,crc))return;
    while(VDPSerial.available())VDPSerial.read();
    vios_zdi_cpu_write(REG_PC,0x40000);
    {char ready[48];snprintf(ready,sizeof ready,"VIOREADY RAM image verified CRC %08lX\r\n",(unsigned long)crc);bridge_event(ready);}
    boot_started=millis();boot_deadline_armed=true;
    vios_zdi_write_register(ZDI_BRK_CTL,0);cpu_paused=false;
    pinMode(ZDI_TCK,INPUT);pinMode(ZDI_TDI,INPUT);
}
