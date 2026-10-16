"""Run rate-sensitive Make and PlatformIO pre-build artifacts without firmware/hardware."""
from pathlib import Path
import os
import runpy
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from test_uart0_native import ROOT, BankedUART


class InternalRateBuildTests(unittest.TestCase):
    def test_resident_rate_changes_rebuild_initializer_while_raw_stays_115200(self):
        prefix=str(Path(os.environ.get('AGONDEV',str(ROOT/'.tools/agondev')))/'bin/ez80-none-elf')
        with tempfile.TemporaryDirectory() as temp:
            binary=Path(temp)/'init.bin'
            for rate, divisor in ((384000,3),(1152000,1),(115200,10),(1152000,1)):
                target=f'build/resident/arch/ez80/uart0_raw-{rate}.o'
                result=subprocess.run(['make',target,f'VIOS_INTERNAL_BAUD={rate}'],cwd=ROOT,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                subprocess.run([prefix+'-objcopy','-O','binary','-j','.text',str(ROOT/target),str(binary)],check=True,capture_output=True)
                uart=BankedUART({0xdb:255,0xa3:0,0xa4:255,0xa5:0},lcr=255)
                uart.execute(binary.read_bytes())
                self.assertEqual((uart.dll,uart.dlm),(divisor,0))
            subprocess.run(['make','build/raw/arch/ez80/uart0_raw.o','VIOS_INTERNAL_BAUD=1152000'],cwd=ROOT,check=True,capture_output=True)
            subprocess.run([prefix+'-objcopy','-O','binary','-j','.text',str(ROOT/'build/raw/arch/ez80/uart0_raw.o'),str(binary)],check=True,capture_output=True)
            uart=BankedUART({0xdb:255,0xa3:0,0xa4:255,0xa5:0});uart.execute(binary.read_bytes())
            self.assertEqual((uart.dll,uart.dlm),(10,0))

    def run_embed(self, root, requested, embedded):
        project=root/'firmware/esp32-vios';(project/'include').mkdir(parents=True,exist_ok=True)
        (root/'build').mkdir(exist_ok=True)
        (root/'build/vios-resident.bin').write_bytes(b'\0'*64+b'MOS\0\1')
        (root/'build/vios-resident.baud').write_text(str(embedded)+'\n')
        (root/'build/agon-stage0.bin').write_bytes(b'\xf3')
        class Env:
            definitions=[]
            def subst(self,text):return str(project) if text=='$PROJECT_DIR' else text
            def Append(self,**kwargs):self.definitions.extend(kwargs.get('CPPDEFINES',[]))
        env=Env();env.definitions=[]
        with patch.dict(os.environ,{'VIOS_INTERNAL_BAUD':str(requested),'VIOS_IMAGE':str(root/'build/vios-resident.bin')}):
            runpy.run_path(str(ROOT/'firmware/esp32-vios/scripts/embed_vios.py'),init_globals={'env':env,'Import':lambda _:None})
        return env,project

    def test_esp_prebuild_rejects_mismatched_embedded_resident_rate(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(RuntimeError,'baud'):
                self.run_embed(Path(temp),1152000,384000)

    def test_esp_prebuild_passes_selected_rate_to_compiler_and_image_metadata(self):
        import json
        with tempfile.TemporaryDirectory() as temp:
            env,project=self.run_embed(Path(temp),384000,384000)
            self.assertIn(('VIOS_INTERNAL_BAUD',384000),env.definitions)
            self.assertEqual(json.loads((project/'include/vios_image.json').read_text())['internal_baud'],384000)

if __name__=='__main__':unittest.main()
