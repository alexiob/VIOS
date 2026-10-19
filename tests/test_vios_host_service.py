"""Mac host-file service protocol and channel-4 dispatch. No serial."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from vios_host_service import HostService
from vios_resident_transport import ResidentSession
from vios_wire import Packet, encode, Decoder


class HostServiceTest(unittest.TestCase):
    def test_protocol_and_sandbox(self):
        with tempfile.TemporaryDirectory() as d:
            raw_svc = HostService(d)
            class Svc:
                n = 0
                def handle(self, body):
                    Svc.n = (Svc.n + 1) % 256
                    reply = raw_svc.handle(bytes([Svc.n]) + body)
                    assert reply[0] == Svc.n, 'request id is echoed'
                    return reply[1:]
                def close_all(self):
                    raw_svc.close_all()
            svc = Svc()
            h = svc.handle(b'Owsub/notes.txt')
            self.assertEqual(h[:1], b'K')
            self.assertEqual(svc.handle(b'W' + h[1:2] + b'from the agon'), b'K\x0d')
            self.assertEqual(svc.handle(b'C' + h[1:2]), b'K')
            self.assertEqual((Path(d) / 'sub/notes.txt').read_bytes(), b'from the agon')
            r = svc.handle(b'Orsub/notes.txt')
            self.assertEqual(svc.handle(b'R' + r[1:2] + bytes([200])), b'Kfrom the agon')
            self.assertEqual(svc.handle(b'R' + r[1:2] + bytes([200])), b'K')
            svc.handle(b'C' + r[1:2])
            for bad in (b'Or../etc/passwd', b'Or/etc/passwd', b'Ora/../../x', b'Or'):
                self.assertEqual(svc.handle(bad)[:1], b'E', bad)
            self.assertEqual(svc.handle(b'Ormissing.txt'), b'Eno such host file')
            self.assertEqual(svc.handle(b'R\x03\x10'), b'Ebad host handle')
            for i in range(4):
                svc.handle(b'Orsub/notes.txt')
            self.assertEqual(svc.handle(b'Orsub/notes.txt'), b'Etoo many host files')
            svc.close_all()


class Raw:
    def __init__(self, incoming):
        self.incoming, self.written = incoming, bytearray()

    def read(self, count=None):
        data, self.incoming = self.incoming, b''
        return data

    def write(self, data):
        self.written.extend(data)


class DispatchTest(unittest.TestCase):
    def test_channel4_request_gets_reply(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'a.txt').write_text('hi')
            raw = Raw(encode(Packet(4, 0, b'\x07Ora.txt')))
            session = ResidentSession(raw)
            session.host_service = HostService(d)
            session._pump()
            frames = Decoder().feed(bytes(raw.written))
            self.assertEqual([(f.channel, f.payload) for f in frames], [(4, b'\x07K\x00')])
            raw.incoming = encode(Packet(4, 0, b'\x08Ora.txt'))
            session.host_service = None
            raw.written.clear()
            session._pump()
            self.assertEqual(Decoder().feed(bytes(raw.written))[0].payload, b'\x08Ehost service not enabled')


if __name__ == '__main__':
    unittest.main()
