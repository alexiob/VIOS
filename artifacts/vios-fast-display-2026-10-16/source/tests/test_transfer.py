import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class TransferTests(unittest.TestCase):
    def setUp(self):
        path = ROOT / 'tools/serial_monitor.py'
        self.assertTrue(path.exists(), 'safe serial uploader is not implemented')
        spec = importlib.util.spec_from_file_location('serial_monitor', path)
        self.module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.module)

    def image(self, size=100):
        image = bytearray(size)
        image[64:69] = b'MOS\0\1'
        return bytes(image)

    def test_rejects_missing_header_and_unsafe_ram_ranges(self):
        for data, start in [(b'x' * 100, 0x40000), (self.image(), 0),
                            (self.image(), 0xB0000), (self.image(0x6F001), 0x40000)]:
            with self.assertRaises(ValueError):
                self.module.validate_image(data, start)
        self.module.validate_image(self.image(), 0x40000)

    def test_records_preserve_addresses_across_64k_boundary(self):
        records = list(self.module.ihex_records(b'ABCDE', 0x4FFFE, chunk_size=16))
        self.assertEqual(records, [b':020000040004F6', b':02FFFE0041427E',
                                   b':020000040005F5', b':0300000043444531',
                                   b':00000001FF'])

if __name__ == '__main__':
    unittest.main()

# These checks exercise the transport policy at its hardware boundary.
class SerialPolicyTests(unittest.TestCase):
    setUp = TransferTests.setUp
    image = TransferTests.image
    def test_pinned_legacy_sender_never_negotiates_an_extended_record(self):
        class Receiver:
            def __init__(self): self.received = bytearray(); self.writes = []
            def command(self, text): pass
            def until(self, marker, timeout=10):
                b = marker + b'\r\n'; self.received.extend(b); return b
            def listen(self, seconds): return b''
            def write(self, data): self.writes.append(data)
        from unittest.mock import patch
        receiver = Receiver()
        with patch.object(self.module.time, 'sleep'):
            self.module.upload(receiver, self.image())
        self.assertFalse(any(line[7:9] == b'FF' for line in receiver.writes),
                         'VDP 1.04 has no extended CRC negotiation')

    def test_command_echo_containing_prompt_does_not_finish_response(self):
        from unittest.mock import patch
        session = self.module.Session.__new__(self.module.Session)
        commands = []
        nonce = b'sync-0123456789abcdef'
        chunks = iter([b'echo agon> ', b'\r\nagon> \r\nagon> ',
                       b'echo ' + nonce + b'\r\n',
                       nonce + b'\r\nagon> '])
        session.command = commands.append
        session.read = lambda count=None: next(chunks, b'')
        with patch.object(self.module, 'command_nonce', return_value=nonce, create=True):
            if hasattr(session, 'run_command'):
                result = session.run_command('echo agon> ')
            else:
                session.command('echo agon> ')
                result = session.until(b'agon> ')
        self.assertEqual(result, b'echo agon> \r\nagon> \r\nagon> ')
        self.assertEqual(list(chunks), [], 'complete nonce response must be consumed')
