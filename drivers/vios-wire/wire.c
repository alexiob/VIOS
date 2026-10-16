/* VIOS COBS frames: version, channel, seqLE16, lenLE16, payload, IEEE CRC32LE.
 * CRC covers every decoded byte preceding the CRC. No allocation/global state.
 */
#include "vios/wire.h"

uint32_t vios_wire_crc32(const void *data,size_t length) {
    const uint8_t *p=(const uint8_t *)data;
    uint32_t crc=0xFFFFFFFFUL;
    while(length--) {
        unsigned bit;
        crc^=*p++;
        for(bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xEDB88320UL:0);
    }
    return crc^0xFFFFFFFFUL;
}
static int channel_valid(uint8_t n) {return n>=VIOS_CHANNEL_CONSOLE&&n<=VIOS_CHANNEL_EVENT;}
size_t vios_wire_encode(const struct vios_wire_message *m,uint8_t *out,size_t capacity) {
    uint8_t raw[VIOS_WIRE_RAW_MAX];
    size_t size,i,code_at=0,used=1;
    uint8_t code=1;
    uint32_t crc;
    if(!m||!out||!channel_valid(m->channel)||m->length>VIOS_WIRE_PAYLOAD_MAX)return 0;
    size=(size_t)m->length+10;
    if(capacity<size+2)return 0;
    raw[0]=VIOS_WIRE_VERSION;raw[1]=m->channel;
    raw[2]=(uint8_t)m->sequence;raw[3]=(uint8_t)(m->sequence>>8);
    raw[4]=(uint8_t)m->length;raw[5]=(uint8_t)(m->length>>8);
    for(i=0;i<m->length;++i)raw[6+i]=m->payload[i];
    crc=vios_wire_crc32(raw,size-4);
    for(i=0;i<4;++i)raw[size-4+i]=(uint8_t)(crc>>(8*i));
    /* Raw maximum is <254, so COBS never needs a 0xFF run. */
    for(i=0;i<size;++i) {
        if(!raw[i]) {out[code_at]=code;code_at=used++;code=1;}
        else {out[used++]=raw[i];++code;}
    }
    out[code_at]=code;out[used++]=0;return used;
}
int vios_wire_decode(const uint8_t *frame,size_t length,struct vios_wire_message *m) {
    uint8_t raw[VIOS_WIRE_RAW_MAX];
    size_t read=0,used=0,i,payload;
    uint32_t expected=0;
    if(!frame||!m||!length||length>=VIOS_WIRE_FRAME_MAX)return 0;
    while(read<length) {
        unsigned code=frame[read++],count;
        if(!code)return 0;
        count=code-1;
        if(count>length-read||count>VIOS_WIRE_RAW_MAX-used)return 0;
        for(i=0;i<count;++i) {uint8_t byte=frame[read++];if(!byte)return 0;raw[used++]=byte;}
        if(code!=255&&read<length) {if(used==VIOS_WIRE_RAW_MAX)return 0;raw[used++]=0;}
    }
    if(used<10||raw[0]!=VIOS_WIRE_VERSION||!channel_valid(raw[1]))return 0;
    payload=(size_t)raw[4]|((size_t)raw[5]<<8);
    if(payload>VIOS_WIRE_PAYLOAD_MAX||used!=payload+10)return 0;
    for(i=0;i<4;++i)expected|=(uint32_t)raw[used-4+i]<<(8*i);
    if(vios_wire_crc32(raw,used-4)!=expected)return 0;
    m->channel=raw[1];m->sequence=(uint16_t)raw[2]|((uint16_t)raw[3]<<8);m->length=(uint16_t)payload;
    for(i=0;i<payload;++i)m->payload[i]=raw[6+i];
    return 1;
}
