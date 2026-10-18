"""Run the production resident platform against a finite UART FIFO model."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class ResidentPlatformTests(unittest.TestCase):
    def run_probe(self,mode):
        with tempfile.TemporaryDirectory() as temporary:
            executable=Path(temporary)/'platform'
            base=[os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                  '-I'+str(ROOT/'include'),'-I'+str(ROOT)]
            transport=Path(temporary)/'transport.o'
            compiled=subprocess.run(base+['-Dvdp_transport_feed=actual_feed','-c',str(ROOT/'drivers/agon-vdp/transport.c'),'-o',str(transport)],capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            command=base+[
                     '-I'+str(ROOT/'include'),'-I'+str(ROOT),str(ROOT/'tests/platform_resident_native.c'),
                     str(ROOT/'boards/agon-light-2/platform_resident.c'),str(transport),'-o',str(executable)]
            compiled=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            result=subprocess.run([str(executable),mode],capture_output=True,text=True,timeout=5)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('resident ',result.stdout)
    def test_vdp_send_uses_bounded_native_bursts_without_per_byte_rx_work(self):self.run_probe('vdp-burst')
    def test_task_gate_admits_only_quiet_input(self):self.run_probe('gate')
    def test_cooked_line_echoes_once(self):self.run_probe('echo')
    def test_echo_follows_mid_line_edits(self):self.run_probe('edit-echo')
    def test_keyboard_idle_work_pauses_peer_until_explicit_yield(self):self.run_probe('keyboard-work')
    def test_keyboard_event_polling_preserves_console_and_rts(self):self.run_probe('keyboard')
    def test_console_flush_receives_full_duplex_input_without_fifo_loss(self):self.run_probe('normal')
    def test_cts_wait_drains_incoming_console_bytes(self):self.run_probe('cts')
    def test_tx_ready_wait_drains_incoming_console_bytes(self):self.run_probe('tx')
    def test_vdp_request_opens_rx_window_and_pauses_on_reply(self):self.run_probe('request')
    def test_vdp_request_timeout_leaves_rx_paused(self):self.run_probe('timeout')
if __name__=='__main__':unittest.main()
