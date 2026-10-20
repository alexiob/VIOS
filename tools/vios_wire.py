"""VIOS v1 wire codec; JSON and all dynamic allocation remain on the host."""
from dataclasses import dataclass
import struct
import zlib

MAX_PAYLOAD = 240
MAX_FRAME = 254
CHANNELS = (1, 2, 3, 4, 5)  # 5: link recovery (include/vios/wire.h VIOS_CHANNEL_LINK)


@dataclass(frozen=True)
class Packet:
    channel: int
    sequence: int
    payload: bytes
    link: int | None = None   # v2 frames (ESP to host): the ESP's link sequence


def encode(packet):
    if packet.channel not in CHANNELS or type(packet.sequence) is not int or not 0<=packet.sequence<=65535 or not isinstance(packet.payload,bytes) or len(packet.payload)>MAX_PAYLOAD:
        raise ValueError('Invalid VIOS packet')
    if packet.link is None:
        raw=struct.pack('<BBHH',1,packet.channel,packet.sequence,len(packet.payload))+packet.payload
    elif type(packet.link) is int and 0<=packet.link<=65535:
        raw=struct.pack('<BBHHH',2,packet.channel,packet.sequence,len(packet.payload),packet.link)+packet.payload
    else:
        raise ValueError('Invalid VIOS packet')
    raw+=struct.pack('<I',zlib.crc32(raw))
    output=bytearray([0]);code_index=0;code=1
    for value in raw:
        if value:
            output.append(value);code+=1
            if code<255:continue
        output[code_index]=code;code_index=len(output);output.append(0);code=1
    output[code_index]=code
    return bytes(output)+b'\0'


def decode(encoded):
    if not encoded or len(encoded)>=MAX_FRAME or 0 in encoded:
        raise ValueError('Invalid COBS frame')
    raw=bytearray();index=0
    while index<len(encoded):
        code=encoded[index];index+=1
        if index+code-1>len(encoded):raise ValueError('Truncated COBS frame')
        raw+=encoded[index:index+code-1];index+=code-1
        if code<255 and index<len(encoded):raw.append(0)
    if len(raw)<10:raise ValueError('Short VIOS frame')
    version,channel,sequence,length=struct.unpack('<BBHH',raw[:6])
    header=8 if version==2 else 6
    if version not in (1,2) or channel not in CHANNELS or length>MAX_PAYLOAD or len(raw)!=length+header+4:
        raise ValueError('Invalid VIOS header')
    if zlib.crc32(raw[:-4])!=struct.unpack('<I',raw[-4:])[0]:
        raise ValueError('VIOS CRC mismatch')
    link=struct.unpack('<H',raw[6:8])[0] if version==2 else None
    return Packet(channel,sequence,bytes(raw[header:-4]),link)


class Decoder:
    def __init__(self):
        self.buffer=bytearray();self.oversize=False;self.errors=0
    def feed(self,data):
        packets=[]
        for value in data:
            if value==0:
                if self.buffer and not self.oversize:
                    try:packets.append(decode(bytes(self.buffer)))
                    except ValueError:self.errors+=1
                self.buffer.clear();self.oversize=False
            elif not self.oversize:
                self.buffer.append(value)
                if len(self.buffer)>=MAX_FRAME:
                    self.errors+=1;self.oversize=True;self.buffer.clear()
        return packets
