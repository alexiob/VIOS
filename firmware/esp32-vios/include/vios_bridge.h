/* VIOS full-VDP USB transport and ZDI RAM boot. MIT.
 * Included after the upstream VDP/zdi implementation in video.ino.
 * After bootstrap, an independent USB task owns UART0/control/ZDI. The VDP
 * task alone owns UART2 and its parser; bounded records cross between them.
 */
#pragma once
#include "vios_bridge_api.h"
#include "vios_image.h"
#include "vios_stage0.h"
#include "vios/internal_uart.h"
#include <atomic>
#ifndef VIOS_BRIDGE_TEST_RUNTIME
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#endif
static SemaphoreHandle_t usb_tx_mutex,vdp_tx_mutex;
struct ConsoleRecord {uint16_t length;uint8_t data[256];};
static QueueHandle_t console_queue,host_queue;
static std::atomic<bool> parser_busy(false);
static std::atomic<bool> full_control(false);
static std::atomic<int> health_mailbox(-1);
RTC_NOINIT_ATTR static uint32_t retained_magic,retained_count,retained_inverse;
static const uint32_t retained_signature=0x56494F53;
static void retain_failures(unsigned count){retained_count=count;retained_inverse=~count;retained_magic=retained_signature;}
static bool enqueue_console(const uint8_t *data,size_t length){
    if(!console_queue||!length||length>256)return false;
    ConsoleRecord record;record.length=(uint16_t)length;memcpy(record.data,data,length);
    return xQueueSend(console_queue,&record,0)==pdTRUE;
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
void vios_usb_console_write(const uint8_t *data,size_t length) {usb_send(VIOS_CHANNEL_CONSOLE,0,data,length);}
/* Host service requests from the CPU go to the Mac unpaced (small, rare). */
void vios_usb_host_write(const uint8_t *data,size_t length) {usb_send(VIOS_CHANNEL_HOST,0,data,length);}
static void control_reply(uint16_t sequence,const char *line) {usb_send(VIOS_CHANNEL_CONTROL,sequence,(const uint8_t *)line,strlen(line));}
static void bridge_event(const char *line) {usb_send(VIOS_CHANNEL_EVENT,0,(const uint8_t *)line,strlen(line));}
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
    if(length==1&&command[0]=='I') {
        snprintf(reply,sizeof reply,"ID %04X REV %02X STAT %02X\r\n",vios_zdi_id(),vios_zdi_revision(),vios_zdi_status());control_reply(sequence,reply);return;
    }
    if(length==1&&command[0]=='B') {
        control_reply(sequence,cpu_paused||(!cpu_boot_ready&&!boot_deadline_armed)?"BOOT HALTED\r\n":boot_deadline_armed?"BOOT STARTING\r\n":"BOOT READY\r\n");return;
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
void vios_vdp_tx_end(){VDPSerial.flush();if(vdp_tx_mutex)xSemaphoreGive(vdp_tx_mutex);}
void vios_bridge_service_vdp(){
    if(cpu_paused.load())return;
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
        else {boot_deadline_armed=false;cpu_boot_ready=true;boot_failures=0;retain_failures(0);}
    }
    if(boot_deadline_armed&&!cpu_paused&&millis()-boot_started>=30000) {
        boot_deadline_armed=false;cpu_boot_ready=false;++boot_failures;retain_failures(boot_failures);
        bool halted=cpu_halt();
        if(boot_failures>=3)bridge_event(halted?"ERR three failed CPU boots; halted, USB recovery available\r\n":"ERR three failed CPU boots; halt failed, use Z\r\n");
        else {char event[80];snprintf(event,sizeof event,"BOOT failure %u/3; restarting ESP recovery\r\n",boot_failures.load());bridge_event(event);DBGSerial.flush();delay(50);ESP.restart();}
    }
    }
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
    if(esp_reset_reason()!=ESP_RST_SW||retained_magic!=retained_signature||retained_count>3||retained_inverse!=~retained_count)retain_failures(0);
    boot_failures=retained_count;
    return usb_tx_mutex&&vdp_tx_mutex&&console_queue&&host_queue;
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
void vios_bridge_boot() {
    cpu_boot_ready=false;boot_deadline_armed=false;
    ram_ready=false;memset(native_ram_map,0,sizeof native_ram_map);
    pinMode(ZDI_TCK,OUTPUT);digitalWrite(ZDI_TCK,HIGH);pinMode(ZDI_TDI,INPUT);
    bridge_event("BOOT VIOS VDP 1.04 mux-v1\r\n");
    char retry_event[48];snprintf(retry_event,sizeof retry_event,"BOOT retained failures %u\r\n",boot_failures.load());bridge_event(retry_event);
    if(vios_zdi_id()!=0x0007||vios_zdi_revision()!=0xAA||!cpu_halt()){bridge_event("ERR CPU identity/halt; recovery control available\r\n");return;}
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
    uint8_t block[256],verify[256];
    for(unsigned offset=0;offset<vios_image_size;offset+=sizeof block) {
        unsigned count=vios_image_size-offset;if(count>sizeof block)count=sizeof block;
        memcpy_P(block,vios_image+offset,count);
        vios_zdi_write_memory(0x40000+offset,count,block);vios_zdi_read_memory(0x40000+offset,(uint16_t)count,verify);
        if(memcmp(block,verify,count)){bridge_event("ERR VIOS RAM readback; CPU left halted\r\n");return;}
        delay(1);
    }
    while(VDPSerial.available())VDPSerial.read();
    vios_zdi_cpu_write(REG_PC,0x40000);
    bridge_event("VIOREADY RAM image verified\r\n");
    boot_started=millis();boot_deadline_armed=true;
    vios_zdi_write_register(ZDI_BRK_CTL,0);cpu_paused=false;
    pinMode(ZDI_TCK,INPUT);pinMode(ZDI_TDI,INPUT);
}
