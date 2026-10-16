"""Freeze verified incremental workbench release; no hardware access."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,shutil,tarfile
root=Path.cwd();out=root/'artifacts/vios-incremental-2026-10-16'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def desc(p):return {'path':str(p.relative_to(root)),'size':p.stat().st_size,'sha256':sha(p)}
assert not (out/'manifest.json').exists(),'Refusing to overwrite frozen release'
fw=json.loads((root/'firmware/esp32-vios/build-manifest.json').read_text())
assert fw['embedded_vios']['sha256']=='cf7660c708f01a7956b8ded85424cfb019ffd68f7870527a9dce02f46d81e1b4'
assert fw['sha256']=='b8d884a1733505de995ee94f5585abaa6ed9ee9986468529cb32dc9fc3fefb2f'
assert sha(root/fw['firmware'])==fw['sha256']
assert sha(root/'build/vios-resident.bin')==fw['embedded_vios']['sha256']
reports=[]
for rel in ['docs/bring-up/vios-incremental-resident-startup.json','docs/bring-up/vios-incremental-resident-hardware.json',
 'docs/bring-up/vios-incremental-focused-resident.json','docs/bring-up/vios-incremental-control.json',
 'docs/benchmarks/incremental-accepted-comparable-2026-10-16.json','docs/benchmarks/incremental-accepted-native-2026-10-16.json']:
 p=root/rel;assert json.loads(p.read_text())['status']=='passed',rel;reports.append(desc(p))
assert 'Ran 163 tests' in (root/'build/incremental-accepted-checks.txt').read_text()
assert '\nOK\n' in (root/'build/incremental-accepted-checks.txt').read_text()
assert 'PASS: 146 total probes' in (root/'build/incremental-engine-checks.txt').read_text()
assert 'Hash of data verified.' in (root/'build/incremental-app0-flash.txt').read_text()
backup=root/'backups/esp32/pre-vios-incremental-app0-2026-10-16.bin'
assert sha(backup)=='643a3ef6517077f4910471b44719ada1d246160a9a00b401b169cd7c25ab31dc'
copies={'build/vios-resident.bin':'vios-accepted.bin','build/vios-resident.elf':'vios-accepted.elf','build/vios-resident.map':'vios-accepted.map',
 fw['firmware']:'firmware-accepted.bin','firmware/esp32-vios/build-manifest.json':'build-manifest-accepted.json','build/agon-stage0.bin':'agon-stage0.bin',
 'build/incremental-accepted-checks.txt':'host-native-checks.txt','build/incremental-engine-checks.txt':'native-engine-checks.txt',
 'build/incremental-accepted-firmware-build.txt':'esp-build.txt','build/incremental-app0-backup.txt':'app0-backup.txt','build/incremental-app0-flash.txt':'app0-flash.txt',
 'docs/bring-up/vios-incremental-resident-live.serial.jsonl':'live-serial-at-release.jsonl'}
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
manifest={'schema':1,'project':'VIOS','milestone':'incremental text pane and line composition','created_utc':datetime.now(timezone.utc).isoformat(),
 'status':'accepted_on_hardware_and_resident','accepted_cpu':desc(out/'vios-accepted.bin'),'accepted_firmware':desc(out/'firmware-accepted.bin'),
 'native_stage0':desc(out/'agon-stage0.bin'),'source_files':len(paths),'source_manifest':'source-sha256.json','hardware_reports':reports,
 'cpu_layout':{'payload_bytes':105232,'text_bytes':105219,'data_bytes':13,'bss_bytes':254031,'bss_end':0x97b5f,'stack_bottom':0xaf000,'stack_top':0xb0000,'remaining_before_stack_bytes':0xaf000-0x97b5f},
 'esp32':{'program_bytes':663101,'static_ram_bytes':37948,'runtime_allocations_additional':True,'app0_offset':65536,'app0_limit':1310720},
 'verification':{'tests_passed':163,'native_engine_probes':146,'resident_workbench_hardware_requests':355,'resident_glyph_comparisons':8,'maximal_source_stress_trials_per_benchmark':50,'mcp_tools':48},
 'previous_app0_backup':desc(backup),'previous_release':'artifacts/vios-workbench-2026-10-16/manifest.json',
 'limitations':['Full source viewport changes still compose all visible source rows','Forced full redraw still has full composition and serial transmission cost','Single 4096-byte draft and one source undo','Trusted native callbacks and raw VDP require explicit redraw/invalidation','No Git repository; old immutable releases preserved']}
manifest['files']=[desc(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name!='manifest.json']
for name,h in hashes.items():assert sha(root/name)==h,name
for f in manifest['files']:assert sha(root/f['path'])==f['sha256'],f['path']
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Frozen',len(paths),'source files;',len(manifest['files']),'artifact entries; all hashes verified')
