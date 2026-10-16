import pathlib,sys,unittest
from unittest.mock import patch
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import vios_hardware
class TransportTests(unittest.TestCase):
 def test_long_running_word_finishes_before_nonce_enters_sixteen_byte_fifo(self):
  class Port:
   def __init__(self):self.busy=False;self.overflow=False;self.queued=b''
   def command(self,text):
    if text=='BENCH':self.busy=True
    else:
     packet=text.encode()+b'\r'
     if self.busy and len(packet)>16:self.overflow=True;packet=packet[:16]
     self.queued=packet
   def until(self,marker,timeout=10):
    if self.busy and marker==b'\r\nvios> ':
     self.busy=False;return b'BENCH\r\nBENCH DONE\r\nok\r\nvios> '
    if self.overflow or not self.queued.endswith(b'\r'):raise TimeoutError('16-byte hardware FIFO lost command terminator')
    self.busy=False;return marker
  port=Port()
  with patch.object(vios_hardware.secrets,'randbelow',return_value=123456):
   result=vios_hardware.command(port,'BENCH')
  self.assertIn(b'BENCH DONE',result)
  self.assertFalse(port.overflow)
