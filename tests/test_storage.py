"""Host disk-image tests: no serial devices or mounted media are opened."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
def run_checked(arguments,**options):
    options.pop('check',None)
    result=subprocess.run(arguments,**options)
    assert result.returncode==0,result.stderr
    return result

def image(path,full=False):
    start,total,fat_sectors=2048,131072,1024
    reserved=32
    boot=bytearray(512)
    boot[:3]=b'\xeb\x58\x90';boot[3:11]=b'VIOS    '
    struct.pack_into('<HBHBHHBHHHII',boot,11,512,1,reserved,2,0,0,0xf8,0,63,255,start,total)
    struct.pack_into('<IHHIHH',boot,36,fat_sectors,0,0,2,1,6)
    boot[64]=0x80;boot[66]=0x29;struct.pack_into('<I',boot,67,0x12345678)
    boot[71:82]=b'VIOS TEST  ';boot[82:90]=b'FAT32   ';boot[510:]=b'\x55\xaa'
    mbr=bytearray(512);mbr[446+4]=0x0c;struct.pack_into('<II',mbr,446+8,start,total);mbr[510:]=b'\x55\xaa'
    info=bytearray(512);struct.pack_into('<I',info,0,0x41615252);struct.pack_into('<I',info,484,0x61417272)
    struct.pack_into('<II',info,488,total-reserved-2*fat_sectors-2,4);struct.pack_into('<I',info,508,0xaa550000)
    fat=bytearray(512);struct.pack_into('<IIII',fat,0,0x0ffffff8,0x0fffffff,0x0fffffff,0x0fffffff)
    if full:
        fat=bytearray(struct.pack('<I',0x0fffffff)*(fat_sectors*128))
        struct.pack_into('<I',fat,0,0x0ffffff8);struct.pack_into('<I',fat,16,0)
        struct.pack_into('<II',info,488,1,4)
    directory=bytearray(512);directory[:11]=b'BOOT    TXT';directory[11]=0x20
    struct.pack_into('<H',directory,26,3);struct.pack_into('<I',directory,28,14)
    with path.open('w+b') as f:
        f.truncate((start+total)*512)
        for lba,data in [(0,mbr),(start,boot),(start+1,info),(start+6,boot),(start+7,info),
                         (start+reserved,fat),(start+reserved+fat_sectors,fat),
                         (start+reserved+2*fat_sectors,directory),(start+reserved+2*fat_sectors+1,b'keep-existing\n')]:
            f.seek(lba*512);f.write(data)

class StorageTests(unittest.TestCase):
    def test_real_fat32_roundtrip_and_injected_io_failures(self):
        sources=['boards/agon-light-2/storage.c','drivers/fatfs/ff.c','drivers/fatfs/diskio.c']
        for s in sources:self.assertTrue((ROOT/s).is_file(),'native storage implementation is missing: '+s)
        with tempfile.TemporaryDirectory() as d:
            d=Path(d);disk=d/'card.img';image(disk)
            command=[os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
                     '-I'+str(ROOT/'include'),'-I'+str(ROOT),'-I'+str(ROOT/'drivers/fatfs'),
                     str(ROOT/'tests/storage_host.c'),*[str(ROOT/s) for s in sources],'-o',str(d/'probe')]
            run_checked(command,check=True,capture_output=True)
            run=run_checked([str(d/'probe'),str(disk)],check=True,capture_output=True,text=True)
            self.assertIn('storage real FAT32 roundtrip/failure probes passed',run.stdout)
            full=d/'full.img';image(full,full=True)
            run=run_checked([str(d/'probe'),str(full),'full'],check=True,capture_output=True,text=True)
            self.assertIn('full FAT32 volume handled',run.stdout)
            # Valid FAT16 superfloppy: mount succeeds in FatFs, policy must reject it.
            fat16=d/'fat16.img'
            boot=bytearray(512);boot[:3]=b'\xeb\x3c\x90';boot[3:11]=b'VIOS    '
            struct.pack_into('<HBHBHHBHHHII',boot,11,512,4,1,2,512,32768,0xf8,128,63,255,0,0)
            boot[510:]=b'\x55\xaa'
            with fat16.open('w+b') as f:
                f.truncate(32768*512);f.write(boot);f.seek(512);f.write(b'\xf8\xff\xff\xff')
                f.seek(129*512);f.write(b'\xf8\xff\xff\xff')
            run=run_checked([str(d/'probe'),str(fat16),'fat16'],check=True,capture_output=True,text=True)
            self.assertIn('FAT16 correctly rejected',run.stdout)
            # An independent directory check confirms the original boot file survived.
            with disk.open('rb') as f:
                f.seek((2048+32+2048)*512);root=f.read(512)
                self.assertEqual(root[:11],b'BOOT    TXT')
                f.seek((2048+32+2048+1)*512);self.assertEqual(f.read(14),b'keep-existing\n')
                f.seek((2048+32)*512);fat1=f.read(1024*512)
                f.seek((2048+32+1024)*512);self.assertEqual(f.read(1024*512),fat1,'FAT copies diverged')

class SdProtocolTests(unittest.TestCase):
    def test_real_sd_protocol_on_byte_level_card(self):
        source=ROOT/'drivers/sd-spi/sd.c'
        self.assertTrue(source.is_file(),'SD protocol implementation is missing')
        with tempfile.TemporaryDirectory() as d:
            executable=Path(d)/'sd'
            run_checked([os.environ.get('CC','cc'),'-std=c99','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
                            '-I'+str(ROOT/'include'),'-I'+str(ROOT),str(ROOT/'tests/sd_spi_host.c'),str(source),'-o',str(executable)],check=True,capture_output=True)
            result=run_checked([str(executable)],check=True,capture_output=True,text=True)
            self.assertIn('SD SPI protocol, address, response and timeout probes passed',result.stdout)

class NativeSpiTests(unittest.TestCase):
    def test_assembled_gpio_select_and_spi_transfer(self):
        from storage_spi_native import NativeSPI
        p=NativeSPI(ROOT,ROOT/'build/verification/storage')
        p.io={0xdb:255}
        p.call('init',[0]);self.assertEqual([p.io[n] for n in [0xdb,0x9a,0x9b,0x9c,0x9d,0xb8,0xb9,0xba]],[0xd7,0x14,0xc8,0,0xc8,32,0,0x30])
        p.call('select',[0]);self.assertEqual(p.io[0x9a],0x14,'inactive CS must stay high despite nonzero port read')
        p.call('select',[1]);self.assertEqual(p.io[0x9a],4)
        p.call('select',[0]);self.assertEqual(p.io[0x9a],0x14)
        p.call('init',[1]);self.assertEqual(p.io[0xb8],3)
        p.io.update({0x9a:0xa9,0x9b:0x55,0x9c:0xff,0x9d:0x35})
        p.call('init',[0]);self.assertEqual([p.io[n] for n in [0x9a,0x9b,0x9c,0x9d]],[0xbd,0xc9,0x23,0xe9])
        self.assertEqual(p.call('xfer',[0x5a,0x90000]),1);self.assertEqual(p.tx[-1],0x5a);self.assertEqual(p.memory[0x90000],0xa5)
        self.assertEqual(p.call('xfer',[0x5a,0x90000],status=0x40),0)
        self.assertEqual(p.call('xfer',[0x5a,0x90000],status=0),0)

if __name__=='__main__':unittest.main()
