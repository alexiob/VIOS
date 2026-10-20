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
        self.host_service=None  # vios_host_service.HostService answering channel 4
        # Link recovery (include/vios/wire.h): the ESP numbers its frames (v2).
        # A decode error or a gap asks it to resend from the first missing one;
        # frames after a gap wait here and are delivered in order. Only frames
        # the ESP no longer keeps, or no progress after LINK_TRIES requests,
        # lose integrity. link_stats counts what was recovered.
        self.link_expected=None;self.link_waiting={};self.link_asked=None;self.link_tries=0
        self.link_stats={'requests':0,'recovered':0,'gone':0,'epochs':0}

    def _send(self,channel,sequence,data):
        for offset in range(0,len(data),MAX_PAYLOAD):
            self.raw.write(encode(Packet(channel,sequence,data[offset:offset+MAX_PAYLOAD])))

    LINK_RETRY=0.3;LINK_TRIES=10;LINK_WINDOW=64

    def _link_request(self,force=False):
        if self.integrity_error:return   # nothing more to recover; the caller redeploys
        now=time.monotonic()
        if not force and self.link_asked is not None and now-self.link_asked<self.LINK_RETRY:return
        self.link_tries+=1;self.link_stats['requests']+=1;self.link_asked=now
        if self.link_tries>self.LINK_TRIES:self.integrity_error=True;return
        self.raw.write(encode(Packet(5,0,b'N'+struct.pack('<H',self.link_expected))))

    def _link_settled(self):
        self.link_asked=None;self.link_tries=0

    def _link_packet(self,packet):
        if self.link_expected is None:self.link_expected=packet.link
        ahead=(packet.link-self.link_expected)&0xFFFF
        if ahead>=32768:ahead-=65536
        if abs(ahead)>self.LINK_WINDOW:   # the ESP restarted: a new random start
            self.link_stats['epochs']+=1;self.link_waiting.clear();self.link_expected=packet.link;ahead=0;self._link_settled()
        if ahead<0 or packet.link in self.link_waiting:return   # a resent copy we already have
        if ahead>0:
            self.link_waiting[packet.link]=packet;self._link_request();return
        self._deliver(packet);self.link_expected=(self.link_expected+1)&0xFFFF
        while self.link_expected in self.link_waiting:
            self._deliver(self.link_waiting.pop(self.link_expected));self.link_expected=(self.link_expected+1)&0xFFFF
        if self.link_asked is not None:self.link_stats['recovered']+=1
        if not self.link_waiting:self._link_settled()

    def _link_reply(self,payload):
        if len(payload)!=3:return
        nxt=struct.unpack('<H',payload[1:])[0]
        if payload[:1]==b'G':
            if not self.integrity_error:self.link_stats['gone']+=1
            self.integrity_error=True
        elif payload[:1]==b'L' and nxt==self.link_expected and not self.link_waiting:self._link_settled()

    def _pump(self):
        errors=self.decoder.errors
        packets=self.decoder.feed(self.raw.read())
        damaged=self.decoder.errors!=errors
        for packet in packets:
            if packet.link is not None:self._link_packet(packet)
            elif packet.channel==5:self._link_reply(packet.payload)
            else:self._deliver(packet)
        if damaged:
            if self.link_expected is not None:self._link_request(force=True)
            elif self.protected:self.integrity_error=True   # before any numbered frame (old firmware)
        elif self.link_asked is not None:self._link_request()   # no answer yet: ask again after LINK_RETRY
        history=getattr(self.raw,'received',None)
        if history is not None and len(history)>65536:del history[:-65536]

    def _deliver(self,packet):
        if packet.channel==1:
            if packet.sequence==0:self.pending.extend(packet.payload)
            elif packet.sequence==self.sequence:self.line_ack=packet.payload
        elif packet.channel==2:
            # Only one serialized request is outstanding. Ignore stale IDs.
            if packet.sequence==self.sequence:
                self.replies.setdefault(packet.sequence,bytearray()).extend(packet.payload)
                if len(self.replies[packet.sequence])>1024:raise IOError('Oversized debug reply')
        elif packet.channel==4:
            # Board-initiated host-file request: answer immediately.
            reply=self.host_service.handle(packet.payload) if self.host_service else packet.payload[:1]+b'Ehost service not enabled'
            self._send(4,0,reply)
        else:
            self.events.append(packet.payload)
            if len(self.events)>128:del self.events[0]

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
