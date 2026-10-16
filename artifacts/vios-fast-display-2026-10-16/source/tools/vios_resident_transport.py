"""Adapt the persistent VDP's framed UART to the existing VIOS board interface."""
import time
import struct
import zlib
from serial_monitor import Session
from vios_wire import Packet, Decoder, encode, MAX_PAYLOAD


class ResidentSession(Session):
    def __init__(self, raw):
        self.raw=raw;self.decoder=Decoder();self.received=bytearray()
        self.pending=bytearray();self.replies={};self.events=[]
        self.sequence=0;self.paused=False
        self.integrity_error=False;self.protected=False;self.line_ack=None

    def _send(self,channel,sequence,data):
        for offset in range(0,len(data),MAX_PAYLOAD):
            self.raw.write(encode(Packet(channel,sequence,data[offset:offset+MAX_PAYLOAD])))

    def _pump(self):
        errors=self.decoder.errors
        packets=self.decoder.feed(self.raw.read())
        if self.protected and self.decoder.errors!=errors:self.integrity_error=True
        for packet in packets:
            if packet.channel==1:
                if packet.sequence==0:self.pending.extend(packet.payload)
                elif packet.sequence==self.sequence:self.line_ack=packet.payload
            elif packet.channel==2:
                # Only one serialized request is outstanding. Ignore stale IDs.
                if packet.sequence==self.sequence:
                    self.replies.setdefault(packet.sequence,bytearray()).extend(packet.payload)
                    if len(self.replies[packet.sequence])>1024:raise IOError('Oversized debug reply')
            else:
                self.events.append(packet.payload)
                if len(self.events)>128:del self.events[0]
        history=getattr(self.raw,'received',None)
        if history is not None and len(history)>65536:del history[:-65536]

    def send_line(self,line,timeout=20):
        if not isinstance(line,str) or '\r' in line or '\n' in line:
            raise ValueError('Send one ASCII console line')
        data=(line+'\r').encode('ascii')
        if len(data)>256:raise ValueError('Console line exceeds 255 bytes')
        if self.paused or self.integrity_error:raise IOError('Console is paused or lost integrity; explicitly redeploy')
        self.protected=True;self.sequence=self.sequence%65535+1;self.line_ack=None
        self._send(1,self.sequence,struct.pack('<HI',len(data),zlib.crc32(data))+data)
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            self._pump()
            if self.integrity_error:raise IOError('VIOS wire integrity lost; result cannot be trusted')
            if self.line_ack is not None:
                if self.line_ack!=b'LINE OK\r\n':raise IOError('Source line rejected: '+repr(self.line_ack))
                return
        raise TimeoutError('No atomic source-line acknowledgement; do not retry automatically')

    def control(self,command,timeout=20):
        if not command or b'\r' in command or b'\n' in command or len(command)>600:
            raise ValueError('Invalid resident control command')
        self.sequence=self.sequence%65535+1
        self.replies.clear();self._send(2,self.sequence,command+b'\n')
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            self._pump()
            reply=self.replies.get(self.sequence,b'')
            if reply.endswith(b'\r\n'):
                return bytes(self.replies.pop(self.sequence))
        raise TimeoutError('Resident debug request timed out; mode may have changed')

    def write(self,data):
        if data==b'\x1d'*3:
            reply=self.control(b'H')
            if not reply.startswith(b'REGS 98 '):raise IOError('CPU pause failed: '+repr(reply))
            self.paused=True;self.pending.extend(b'\r\nDEBUG\r\n')
        elif self.paused:
            if data==b'X':
                self.pending.clear();self.integrity_error=False;self.protected=True
            reply=self.control(data)
            if data==b'Y':
                if not reply.startswith(b'RESUME '):raise IOError('CPU resume failed: '+repr(reply))
                self.paused=False
            elif data==b'X':
                if not reply.startswith(b'GO '):raise IOError('CPU start failed: '+repr(reply))
                self.paused=False
            # The RAM uploader sends the delay selector without awaiting a reply.
            if data!=b'2':self.pending.extend(reply)
        else:
            if b'\x1d' in data:raise ValueError('Ctrl-] is reserved for debug')
            self._send(1,0,data)

    def read(self,count=None):
        if self.integrity_error and not self.paused:raise IOError('VIOS wire integrity lost; explicitly redeploy')
        if not self.pending:self._pump()
        if self.integrity_error and not self.paused:raise IOError('VIOS wire integrity lost; explicitly redeploy')
        n=len(self.pending) if count is None else min(count,len(self.pending))
        data=bytes(self.pending[:n]);del self.pending[:n]
        self.received.extend(data)
        if len(self.received)>65536:del self.received[:-65536]
        return data

    def close(self):self.raw.close()
