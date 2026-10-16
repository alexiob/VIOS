"""Freeze the already verified workbench milestone; never access hardware."""
from pathlib import Path
from datetime import datetime,timezone
import hashlib,json,shutil,tarfile,re
root=Path.cwd();out=root/'artifacts/vios-workbench-2026-10-16'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def desc(p):return {'path':str(p.relative_to(root)),'size':p.stat().st_size,'sha256':sha(p)}
assert not (out/'manifest.json').exists(),'Refusing to overwrite a frozen release'
fw=json.loads((root/'firmware/esp32-vios/build-manifest.json').read_text())
assert fw['sha256']=='643a3ef6517077f4910471b44719ada1d246160a9a00b401b169cd7c25ab31dc'
assert fw['embedded_vios']['sha256']=='36fb5f932f477a182a556ae45f033820d0ce250396718fa0a795d77323383bbe'
assert sha(root/fw['firmware'])==fw['sha256']
assert sha(root/'build/vios-resident.bin')==fw['embedded_vios']['sha256']
reports=[]
for name in ['vios-workbench-hardware-run4','vios-workbench-extra-acceptance','vios-workbench-resident-acceptance','vios-workbench-mcp-stdio-proxy']:
 p=root/'docs/bring-up'/f'{name}.json';r=json.loads(p.read_text());assert r['status']=='passed',name
 if 'checks'in r:assert all(x['passed'] for x in r['checks']),name
 reports.append(desc(p))
assert json.loads((root/'docs/bring-up/vios-workbench-cli-key.json').read_text())['status_result']=='passed'
checks=(root/'build/workbench-final-checks-passing.txt').read_text();assert 'Ran 163 tests'in checks and '\nOK\n'in checks
engine=(root/'build/workbench-engine-checks.txt').read_text();assert 'PASS: 146 total probes'in engine
copies={
 'build/vios-resident.bin':'vios-accepted.bin','build/vios-resident.elf':'vios-accepted.elf','build/vios-resident.map':'vios-accepted.map',
 fw['firmware']:'firmware-accepted.bin','firmware/esp32-vios/build-manifest.json':'build-manifest-accepted.json','build/agon-stage0.bin':'agon-stage0.bin',
 'build/workbench-final-checks-passing.txt':'host-native-checks.txt','build/workbench-engine-checks.txt':'native-engine-checks.txt',
 'build/workbench-esp-final-build.txt':'esp-build.txt','build/workbench-app0-backup.txt':'app0-backup.txt','build/workbench-app0-flash.txt':'app0-flash.txt',
 'docs/bring-up/vios-workbench-resident-live.serial.jsonl':'live-serial-at-release.jsonl',
 'docs/bring-up/vios-workbench-screen.json':'screen-model.json','docs/bring-up/vios-workbench-screen.png':'screen-model.png',
}
for src,dst in copies.items():
 dest=out/dst;assert not dest.exists(),dest;shutil.copy2(root/src,dest)
paths=[]
for directory in ['arch','boards','drivers','forth','include','kernel','bench','tools','tests','firmware/esp32-vios']:
 for p in (root/directory).rglob('*'):
  rel=p.relative_to(root)
  if p.is_file() and not any(x in {'.pio','__pycache__','.git','.DS_Store'} for x in rel.parts) and p.suffix!='.pyc':paths.append(p)
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
(out/'README.md').write_text('''# VIOS accepted text-workbench milestone — 2026-10-16

The accepted CPU/app0 pair is `vios-accepted.bin` (98,270 bytes) and `firmware-accepted.bin` (656,512 bytes). `manifest.json` checks all hashes. App0 embeds exactly the RAM-verified CPU and unchanged 46-byte native SRAM bootstrap. CPU flash, ESP bootloader/partition table/app1 and confirmed SD boot A generation 7 were preserved.

The 80×60 workbench provides browser, source editor and live inspector. CLI and 19 typed MCP workbench tools share one board owner. The default dictionary capacity is 512, configurable per build; 391 live words and high execution tokens were exercised on the physical CPU. Two screen planes, dirty-row index, source draft and undo are static; no target JSON/heap.

Measured native rendering: populated repaint 503.274ms/4123B, unchanged 0 B, one-character edit 4.215 ms / 32 B, selection 4.760 ms / 39 B, source scroll 50.809 ms / 370 B. These exclude composition; full composition remains 175.056 ms and is the next optimization. Render methods and host latency are separately recorded in docs/benchmarks/workbench-2026-10-16.md. The user confirmed visible VGA inspector and apparent keyboard operation.

Evidence includes 163 passing Python/native tests, 146 linked-engine execution probes, 355 real hardware requests, 8 framebuffer glyph comparisons, 50 maximal source requests during repaint,three-engine benchmarks,source persistence and 14 resident-startup checks. The actual MCP stdio proxy and pending KEY/USB continuation were checked with panes open. `screen-model.png` reconstructs captured WB-VIEW cells with the pinned Agon font; it is not a camera photograph.

`source-code.tar.gz` contains the exact build/source/vendor files, checked by `source-sha256.json`. `documentation-evidence.tar.gz` preserves positive and negative development evidence. `diagnostics` retains the failed RX-window candidate and the full-scan renderer baseline. Existing source/firmware archives remain untouched. Toolchains are external dependencies; versions are recorded in the manifest.

Previous app0 backup: backups/esp32/pre-vios-workbench-app0-2026-10-16.bin (630,544B), SHA256 0a064beaba2be05f07960a9e05da1bb5a161af8ac7799c55fc6f4c3459e2a129. The application-only rollback command is in docs/bring-up/vios-workbench.md. It does not erase SD source packages or source-boot bundles.

The running TCP harness is 127.0.0.1:8765; share it with `.venv/bin/python tools/vios_cli.py` or an MCP stdio client using `/usr/bin/nc 127.0.0.1 8765` . One process owns USB serial. See docs/bring-up/vios-workbench-live-session.json. Boot intentionally still selects original demo A; WBM1P is a saved package loaded into the current RAM session, not an implicit boot-slot update.
''')
manifest={'schema':1,'project':'VIOS','milestone':'first hybrid text workbench','created_utc':datetime.now(timezone.utc).isoformat(),'status':'accepted_on_hardware_and_resident','accepted_cpu':desc(out/'vios-accepted.bin'),'accepted_firmware':desc(out/'firmware-accepted.bin'),'native_stage0':desc(out/'agon-stage0.bin'),'cpu_layout':{'load_address':0x40000,'payload_bytes':98270,'bss_bytes':240828,'bss_end':0x92c9a,'stack_bottom':0xaf000,'stack_top':0xb0000,'remaining_before_stack_bytes':115558,'workbench_incremental_bss_bytes':29050,'dictionary_expansion_bss_bytes':21696},'esp32':{'app0_offset':0x10000,'app0_limit':0x140000,'static_ram_bytes':37948,'program_bytes':656141,'runtime_allocations_additional':True},'dictionary':{'capacity':512,'hash_buckets':128,'target_bytes_per_entry':55,'hardware_live_words_tested':391},'verification':{'tests_passed':163,'native_engine_probes':146,'hardware_requests':355,'framebuffer_glyph_comparisons':8,'maximum_source_stress_trials':50,'resident_startup_checks':14,'mcp_tools':48,'workbench_mcp_tools':19},'hardware_reports':reports,'user_visual_confirmation':desc(root/'docs/bring-up/vios-workbench-user-display-confirmation.json'),'benchmarks':desc(root/'docs/benchmarks/workbench-2026-10-16.json'),'toolchains':{'agondev':'v0.22','clang':'15.0.7 / AgonPlatform c76386c0083e6a6236ff774275227e2389f85538','platformio':'espressif32@6.3.0','arduino_esp32':'2.0.9','xtensa_pio':'8.4.0+2021r2-patch5','ESP32Time':'2.0.0'},'source_files':len(paths),'source_manifest':'source-sha256.json','previous_app0_backup':desc(root/'backups/esp32/pre-vios-workbench-app0-2026-10-16.bin'),'limitations':['Full-frame composition remains about 175 ms for the populated fixture; incremental pane/line composition is next','CLI evaluation workspace remains USB; no general local graphical workspace in this milestone','One 4096-byte RAM draft and one source undo; explicit stage/activate/save','PageUp/PageDown currently alias Home/End','Raw native/VDP operations remain trusted; independent supervisor recovery retained','A/B source boot shares one FAT32 card and is not ESP firmware A/B rollback'],'git_repository':False}
manifest['files']=[desc(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name!='manifest.json']
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
for name,h in hashes.items():assert sha(root/name)==h,name
for f in manifest['files']:assert sha(root/f['path'])==f['sha256'],f['path']
print('Frozen source files',len(paths),'artifact files',len(manifest['files']),'all hashes verified')
