import pathlib,re,subprocess,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class VdpTransportTests(unittest.TestCase):
 def test_native_packet_parser(self):
  self.assertTrue((ROOT/'drivers/agon-vdp/transport.c').exists(),'VDP transport missing')
  binary=ROOT/'build/tests/vdp-transport';binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude','tests/vdp_transport_native.c','drivers/agon-vdp/transport.c','-o',str(binary)],cwd=ROOT,check=True)
  subprocess.run([str(binary)],check=True,capture_output=True,timeout=5)

 def test_key_constants_match_pinned_fabgl(self):
  source=(ROOT/'firmware/esp32-vios/vendor/vdp-gl/src/fabutils.h').read_text().split('enum VirtualKey {',1)[1].split('};',1)[0]
  source=re.sub(r'/\*.*?\*/','',source,flags=re.S);source=re.sub(r'//[^\n]*','',source)
  values={name.strip():i for i,name in enumerate(source.split(','))}
  header=(ROOT/'include/platform/input.h').read_text()
  for name,value in re.findall(r'PLATFORM_KEY_([A-Z0-9_]+)=(\d+)',header):
   self.assertEqual(values['VK_'+name],int(value),name)
