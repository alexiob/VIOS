"""Host wire codec and resident-session behavior, without serial hardware."""
import pathlib
import struct
import sys
import unittest
import zlib

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
try:
    from vios_wire import Packet, Decoder, encode, decode
    from vios_resident_transport import ResidentSession
except ImportError:
    Packet = Decoder = encode = decode = ResidentSession = None


class WireTests(unittest.TestCase):
    def setUp(self):
        self.assertIsNotNone(Packet, 'resident wire implementation missing')

    def test_all_byte_values_and_independent_crc_header(self):
        for payload in [b'',b'\0',bytes(range(240)),b'\0'*240,b'\xff'*240]:
            packet=Packet(2,0x1234,payload)
            framed=encode(packet)
            self.assertLessEqual(len(framed),252)
            self.assertEqual(framed[-1],0)
            self.assertNotIn(0,framed[:-1])
            self.assertEqual(decode(framed[:-1]),packet)
        # Independent COBS decoding pins header endianness and CRC coverage.
        encoded=encode(Packet(1,0,b'ABC\0XYZ'))[:-1]
        plain=bytearray();index=0
        while index<len(encoded):
            count=encoded[index];index+=1
            plain+=encoded[index:index+count-1];index+=count-1
            if count<255 and index<len(encoded):plain.append(0)
        self.assertEqual(plain[:6],struct.pack('<BBHH',1,1,0,7))
        self.assertEqual(struct.unpack('<I',plain[-4:])[0],zlib.crc32(plain[:-4]))

    def test_stream_fragmentation_corruption_and_oversize_resynchronize(self):
        decoder=Decoder();good=encode(Packet(3,4,b'healthy'))
        bad=bytearray(encode(Packet(1,0,b'bad')));bad[-3]^=1
        stream=b'boot noise\0'+bad+b'X'*400+b'\0'+good
        results=[]
        for byte in stream:results+=decoder.feed(bytes([byte]))
        self.assertEqual(results,[Packet(3,4,b'healthy')])
        self.assertGreaterEqual(decoder.errors,3)
        for invalid in [b'',b'\0',b'\x05ab']:
            with self.assertRaises(ValueError):decode(invalid)

    def test_invalid_packet_bounds(self):
        for packet in [Packet(6,0,b''),Packet(1,-1,b''),Packet(1,65536,b''),Packet(1,0,b'x'*241)]:
            with self.assertRaises(ValueError):encode(packet)

    def test_atomic_source_lines_and_fail_closed_output(self):
        class Peer:
            def __init__(self):self.pending=bytearray();self.decoder=Decoder();self.line=bytearray();self.executed=[];self.drop=False
            def write(self,data):
                for packet in self.decoder.feed(data):
                    if self.drop and self.line:continue
                    self.line+=packet.payload
                    if len(self.line)>=6:
                        size,crc=struct.unpack('<HI',self.line[:6])
                        if len(self.line)==size+6:
                            body=bytes(self.line[6:]);assert zlib.crc32(body)==crc
                            self.executed.append(body);self.line.clear()
                            self.pending+=encode(Packet(1,packet.sequence,b'LINE OK\r\n'))
            def read(self):data=bytes(self.pending);self.pending.clear();return data
            def close(self):pass
        raw=Peer();session=ResidentSession(raw)
        self.assertTrue(hasattr(session,'send_line'),'atomic line transport missing')
        source='@12345678 '+('1 2 + DROP'.rjust(245))
        session.send_line(source,timeout=.1)
        self.assertEqual(raw.executed,[(source+'\r').encode()])
        bad=bytearray(encode(Packet(1,0,b'0')));bad[-3]^=1
        raw.pending+=bad+encode(Packet(1,0,b'1\r\nok\r\n'))
        with self.assertRaises(IOError):session.read()
        with self.assertRaises(IOError):session.read()
        peer=Peer();peer.drop=True;session=ResidentSession(peer)
        with self.assertRaises(TimeoutError):session.send_line(source,timeout=.01)
        self.assertEqual(peer.executed,[])

    def test_console_and_fragmented_debug_keep_channels_separate(self):
        class Raw:
            def __init__(self):
                self.pending=bytearray();self.decoder=Decoder();self.command=bytearray();self.closed=False;self.packets=[]
            def write(self,data):
                for p in self.decoder.feed(data):
                    self.packets.append(p)
                    if p.channel==1:
                        self.pending+=encode(Packet(1,0,p.payload))
                    elif p.channel==2:
                        self.command+=p.payload
                        if self.command.endswith(b'\n'):
                            command=bytes(self.command[:-1]);self.command.clear()
                            reply=b'REGS 98 state\r\n' if command==b'H' else b'RESUME USB/UART bridge 115200\r\n' if command==b'Y' else b'OK '+command+b'\r\n'
                            self.pending+=encode(Packet(3,0,b'event'))
                            for offset in range(0,len(reply),240):
                                self.pending+=encode(Packet(2,p.sequence,reply[offset:offset+240]))
            def read(self):
                result=bytes(self.pending[:17]);del self.pending[:17];return result
            def close(self):self.closed=True
        raw=Raw();session=ResidentSession(raw)
        source=b'@12345678 '+b' '*240+b'1 2 +\r'
        session.write(source)
        self.assertEqual(session.until(source,1),source)
        self.assertEqual([len(p.payload) for p in raw.packets],[240,16])
        session.write(b'\x1d'*3)
        self.assertIn(b'DEBUG\r\n',session.until(b'DEBUG\r\n',1))
        command=b'W'+b'A'*530
        session.write(command)
        self.assertEqual(session.until(b'\r\n',1),b'OK '+command+b'\r\n')
        session.write(b'Y')
        self.assertIn(b'RESUME',session.until(b'\r\n',1))
        self.assertFalse(session.paused)
        self.assertEqual(session.events[-1],b'event')
        session.close();self.assertTrue(raw.closed)


class LinkRecoveryTests(unittest.TestCase):
    """The ESP numbers frames (v2) and resends on request; the host recovers bytes the USB link lost."""
    class Esp:
        RING=24
        def __init__(self,start=0xFFF0):
            self.next=start;self.ring={};self.out=bytearray();self.decoder=Decoder();self.mute_replies=0;self.requests=0
        def send(self,channel,payload,cut=None):
            frame=encode(Packet(channel,0,payload,self.next));self.ring[self.next]=frame
            self.ring.pop((self.next-self.RING)&0xFFFF,None);self.next=(self.next+1)&0xFFFF
            self.out+=frame if cut is None else frame[:cut]+frame[cut+3:]   # the link loses 3 bytes
        def write(self,data):
            for p in self.decoder.feed(data):
                assert p.channel==5 and p.payload[:1]==b'N';self.requests+=1
                first=struct.unpack('<H',p.payload[1:])[0];behind=(self.next-first)&0xFFFF
                if behind>len(self.ring):kind=b'G'
                else:
                    for k in range(behind):self.out+=self.ring[(first+k)&0xFFFF]
                    kind=b'L'
                if self.mute_replies:self.mute_replies-=1;continue
                self.out+=encode(Packet(5,0,kind+struct.pack('<H',self.next)))
        def read(self):
            data=bytes(self.out);self.out.clear();return data
    def pump(self,session,times=3):
        for _ in range(times):session._pump()
    def test_damaged_frames_are_resent_and_delivered_in_order(self):
        esp=self.Esp();esp.RING=64;session=ResidentSession(esp);session.protected=True
        for i in range(30):esp.send(1,b'line %02d\r\n'%i,cut=8 if i in (5,17) else None)   # wraps 0xFFFF
        self.pump(session)
        self.assertFalse(session.integrity_error)
        self.assertEqual(bytes(session.pending),b''.join(b'line %02d\r\n'%i for i in range(30)))
        self.assertGreaterEqual(esp.requests,1);self.assertGreaterEqual(session.link_stats['recovered'],1)
    def test_a_damaged_last_frame_is_recovered_without_later_traffic(self):
        esp=self.Esp(3);session=ResidentSession(esp);session.protected=True
        esp.send(1,b'ok\r\n');esp.send(1,b'vios> ',cut=6)
        self.pump(session)
        self.assertEqual(bytes(session.pending),b'ok\r\nvios> ');self.assertFalse(session.integrity_error)
    def test_a_lost_reply_is_asked_again_and_frames_no_longer_kept_lose_integrity(self):
        esp=self.Esp(100);session=ResidentSession(esp);session.protected=True;session.LINK_RETRY=0
        esp.send(1,b'a');esp.mute_replies=1;esp.send(1,b'bcdefghijk',cut=7)
        self.pump(session,6)
        self.assertEqual(bytes(session.pending),b'abcdefghijk');self.assertFalse(session.integrity_error)
        session.LINK_RETRY=ResidentSession.LINK_RETRY;esp.send(1,b'x',cut=7)
        for i in range(40):esp.ring.clear() if i==0 else None;esp.send(1,b'y')
        self.pump(session,4)
        self.assertTrue(session.integrity_error);self.assertEqual(session.link_stats['gone'],1)
    def test_an_esp_restart_starts_a_new_sequence(self):
        esp=self.Esp(500);session=ResidentSession(esp)
        esp.send(3,b'one');self.pump(session,1)
        esp.next=40000;esp.ring.clear();esp.send(3,b'two');self.pump(session,1)
        self.assertEqual(session.events,[b'one',b'two']);self.assertEqual(session.link_stats['epochs'],1)

if __name__=='__main__':unittest.main()
