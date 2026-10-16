/* RAM-safe ZDI transport reused from tools/esp32-ram/zdi_probe.c (MIT).
 * Protocol adaptation originally from AgonPlatform/agon-recovery (MIT):
 * Copyright (c) 2024 Jeroen Venema. See vendor/agon-vdp/LICENSE and repository
 * tools/esp32-ram/NOTICE.txt. This path never programs CPU flash.
 */
#pragma once
#ifndef VIOS_GPIO_REG
#define VIOS_GPIO_REG(a) (*(volatile uint32_t *)(0x3FF44000u+(a)))
#endif
// FreeRTOS critical sections save/restore the prior interrupt mask and serialize
// a whole transaction; GPIO edges must not be preempted by VDP/audio IRQ work.
static portMUX_TYPE vz_transaction_mux=portMUX_INITIALIZER_UNLOCKED;
static const uint32_t vios_zdi_clock=1u<<26,vios_zdi_data=1u<<27;
static void vz_high(uint32_t bits){VIOS_GPIO_REG(8)=bits;}
static void vz_low(uint32_t bits){VIOS_GPIO_REG(12)=bits;}
static void vz_output(uint32_t bits){VIOS_GPIO_REG(0x24)=bits;}
static void vz_input(uint32_t bits){VIOS_GPIO_REG(0x28)=bits;}
static void vz_write_bit(unsigned bit) {
    vz_low(vios_zdi_clock);if(bit)vz_high(vios_zdi_data);else vz_low(vios_zdi_data);
    vz_output(vios_zdi_data);delayMicroseconds(10);vz_high(vios_zdi_clock);delayMicroseconds(10);
}
static unsigned vz_read_bit() {
    vz_low(vios_zdi_clock);vz_input(vios_zdi_data);delayMicroseconds(10);
    unsigned value=(VIOS_GPIO_REG(0x3C)>>27)&1;vz_high(vios_zdi_clock);delayMicroseconds(10);return value;
}
static void vz_start(unsigned reg,unsigned read) {
    vz_high(vios_zdi_data);vz_output(vios_zdi_data);vz_high(vios_zdi_clock);delayMicroseconds(2);
    vz_low(vios_zdi_data);delayMicroseconds(2);
    for(int bit=6;bit>=0;--bit)vz_write_bit(reg&(1u<<bit));vz_write_bit(read);
}
static void vz_read(unsigned reg,unsigned count,uint8_t *data) {
    portENTER_CRITICAL(&vz_transaction_mux);
    vz_output(vios_zdi_clock);vz_start(reg,1);
    while(count--){vz_write_bit(0);delayMicroseconds(3);unsigned value=0;for(unsigned bit=0;bit<8;++bit)value=(value<<1)|vz_read_bit();*data++=(uint8_t)value;}
    vz_write_bit(1);delayMicroseconds(3);
    portEXIT_CRITICAL(&vz_transaction_mux);
}
static void vz_write_registers(unsigned reg,unsigned count,const uint8_t *data) {
    portENTER_CRITICAL(&vz_transaction_mux);
    vz_output(vios_zdi_clock);vz_start(reg,0);
    while(count--){vz_write_bit(0);delayMicroseconds(3);uint8_t value=*data++;for(int bit=7;bit>=0;--bit)vz_write_bit(value&(1u<<bit));}
    vz_write_bit(1);delayMicroseconds(3);
    portEXIT_CRITICAL(&vz_transaction_mux);
}
/* F92 hardware diagnostics showed stale ED-prefixed execution while operand
 * stores were loaded. Neutralize IS0 before preload, avoid auto-address bursts
 * across instruction stores, and commit the opcode only after its operands.
 * Ordinary register and memory transfers retain their proven burst path. */
static void vios_zdi_write_registers(unsigned reg,unsigned count,const uint8_t *data) {
    if(count&&reg>=0x21&&reg<0x25&&count<=0x26-reg) {
        const uint8_t nop=0;
        vz_write_registers(0x25,1,&nop);
        for(unsigned i=0;i<count;++i)vz_write_registers(reg+i,1,data+i);
    }else vz_write_registers(reg,count,data);
}
static void vios_zdi_write_register(unsigned reg,uint8_t value){vios_zdi_write_registers(reg,1,&value);}
static uint8_t vios_zdi_status(){uint8_t value;vz_read(3,1,&value);return value;}
static uint16_t vios_zdi_id(){uint8_t data[2];vz_read(0,2,data);return (uint16_t)data[0]|((uint16_t)data[1]<<8);}
static uint8_t vios_zdi_revision(){uint8_t value;vz_read(2,1,&value);return value;}
static uint32_t vios_zdi_cpu_read(unsigned reg){uint8_t data[3];vios_zdi_write_register(0x16,(uint8_t)reg);vz_read(0x10,3,data);return data[0]|((uint32_t)data[1]<<8)|((uint32_t)data[2]<<16);}
static void vios_zdi_cpu_write(unsigned reg,uint32_t value){uint8_t data[3]={(uint8_t)value,(uint8_t)(value>>8),(uint8_t)(value>>16)};vios_zdi_write_registers(0x13,3,data);vios_zdi_write_register(0x16,(uint8_t)(reg|0x80));}
static void vios_zdi_read_memory(uint32_t address,unsigned count,uint8_t *data){vios_zdi_cpu_write(REG_PC,address);vz_read(0x20,count,data);}
static void vios_zdi_write_memory(uint32_t address,unsigned count,uint8_t *data){vios_zdi_cpu_write(REG_PC,address);vios_zdi_write_registers(0x30,count,data);}
static void vios_zdi_out0(uint8_t port,uint8_t value){uint8_t ld[2]={value,0x3E},out[3]={port,0x39,0xED};vios_zdi_write_registers(0x24,2,ld);vios_zdi_write_registers(0x23,3,out);}
static void vios_zdi_di(){vios_zdi_write_register(0x25,0xF3);}

/* Boot-only discovery using the original tested IN0 burst. No register state
 * is resumed afterward: caller clears IS0 and enters verified native stage0. */
static uint8_t vios_zdi_boot_in0(uint8_t port) {
    uint8_t instruction[3]={port,0x38,0xED};
    vz_write_registers(0x23,3,instruction);delayMicroseconds(10);
    return (uint8_t)vios_zdi_cpu_read(REG_AF);
}
