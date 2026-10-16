"""Freeze verified incremental workbench release; no hardware access."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,shutil,tarfile
root=Path.cwd();out=root/'artifacts/vios-source-rows-2026-10-16'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def desc(p):return {'path':str(p.relative_to(root)),'size':p.stat().st_size,'sha256':sha(p)}
assert not (out/'manifest.json').exists(),'Refusing to overwrite frozen release'
fw=json.loads((root/'firmware/esp32-vios/build-manifest.json').read_text())
assert fw['embedded_vios']['sha256']=='7e47cda6905b69bcffadb77e53d02317b5a7b8af1e47c6ad049347880abc0753'
assert fw['sha256']=='e0ed0d20b2438e26e47404a152e9aeeeb98268f7c52113fcedec6854a8c845ae'
assert sha(root/fw['firmware'])==fw['sha256']
assert sha(root/'build/vios-resident.bin')==fw['embedded_vios']['sha256']
reports=[]
for rel in ['docs/bring-up/vios-source-rows-resident-startup.json','docs/bring-up/vios-source-rows-resident-hardware.json',
 'docs/bring-up/vios-source-rows-resident-focused.json','docs/bring-up/vios-source-rows-control.json',
 'docs/benchmarks/source-rows-resident-mixed-2026-10-16.json','docs/benchmarks/source-rows-resident-native-2026-10-16.json']:
 p=root/rel;assert json.loads(p.read_text())['status']=='passed',rel;reports.append(desc(p))
assert 'Ran 163 tests' in (root/'build/source-rows-final-checks.txt').read_text()
assert '\nOK\n' in (root/'build/source-rows-final-checks.txt').read_text()
assert 'PASS: 146 total probes' in (root/'build/source-rows-engine-checks.txt').read_text()
assert 'Hash of data verified.' in (root/'build/source-rows-app0-flash.txt').read_text()
backup=root/'backups/esp32/pre-vios-source-rows-app0-2026-10-16.bin'
assert sha(backup)=='b8d884a1733505de995ee94f5585abaa6ed9ee9986468529cb32dc9fc3fefb2f'
copies={'build/vios-resident.bin':'vios-accepted.bin','build/vios-resident.elf':'vios-accepted.elf','build/vios-resident.map':'vios-accepted.map',
 fw['firmware']:'firmware-accepted.bin','firmware/esp32-vios/build-manifest.json':'build-manifest-accepted.json','build/agon-stage0.bin':'agon-stage0.bin',
 'build/source-rows-final-checks.txt':'host-native-checks.txt','build/source-rows-engine-checks.txt':'native-engine-checks.txt',
 'build/source-rows-firmware-build.txt':'esp-build.txt','build/source-rows-app0-backup.txt':'app0-backup.txt','build/source-rows-app0-flash.txt':'app0-flash.txt',
 'docs/bring-up/vios-source-rows-resident-live.serial.jsonl':'live-serial-at-release.jsonl'}
for src,dst in copies.items():
 p=out/dst;assert not p.exists(),p;shutil.copy2(root/src,p)
paths=[]
for directory in ['arch','boards','drivers','forth','include','kernel','bench','tools','tests','firmware/esp32-vios']:
 for p in (root/directory).rglob('*'):
  if p.is_file() and not any(x in {'.pio','__pycache__','.git','.DS_Store'} for x in p.relative_to(root).parts) and p.suffix!='.pyc':paths.append(p)
for name in ['Makefile','README.md','.gitignore']:
 p=root/name
 if p.exists():paths.append(p)
paths=sorted(set(paths));hashes={str(p.relative_to(root)):sha(p) for p in paths}
(out/'source-sha256.json').write_text(json.dumps(hashes,indent=2)+'\n')
with tarfile.open(out/'source-code.tar.gz','w:gz') as t:
 for p in paths:t.add(p,arcname=str(p.relative_to(root)),recursive=False)
with tarfile.open(out/'documentation-evidence.tar.gz','w:gz') as t:
 for p in sorted((root/'docs').rglob('*')):
  if p.is_file():t.add(p,arcname=str(p.relative_to(root)),recursive=False)
manifest={'schema':1,'project':'VIOS','milestone':'source scrolling and newline composition','created_utc':datetime.now(timezone.utc).isoformat(),
 'status':'accepted_on_hardware_and_resident','accepted_cpu':desc(out/'vios-accepted.bin'),'accepted_firmware':desc(out/'firmware-accepted.bin'),
 'native_stage0':desc(out/'agon-stage0.bin'),'source_files':len(paths),'source_manifest':'source-sha256.json','hardware_reports':reports,
 'cpu_layout':{'payload_bytes':105706,'text_bytes':105693,'data_bytes':13,'bss_bytes':254031,'bss_end':0x97d39,'stack_bottom':0xaf000,'stack_top':0xb0000,'remaining_before_stack_bytes':0xaf000-0x97d39},
 'esp32':{'program_bytes':663581,'static_ram_bytes':37948,'runtime_allocations_additional':True,'app0_offset':65536,'app0_limit':1310720},
 'verification':{'tests_passed':163,'native_engine_probes':146,'resident_workbench_hardware_requests':355,'resident_glyph_comparisons':8,'stress_requests_per_benchmark':50,'maximum_235_byte_requests_per_benchmark':45,'redraw_requests_per_benchmark':5,'mcp_tools':48},
 'previous_app0_backup':desc(backup),'previous_release':'artifacts/vios-incremental-2026-10-16/manifest.json',
 'limitations':['Full source viewport changes still compose all visible source rows','Forced full redraw still has full composition and serial transmission cost','Single 4096-byte draft and one source undo','Trusted native callbacks and raw VDP require explicit redraw/invalidation','No Git repository; old immutable releases preserved']}
manifest['files']=[desc(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name!='manifest.json']
for name,h in hashes.items():assert sha(root/name)==h,name
for f in manifest['files']:assert sha(root/f['path'])==f['sha256'],f['path']
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Frozen',len(paths),'source files;',len(manifest['files']),'artifact entries; all hashes verified')
