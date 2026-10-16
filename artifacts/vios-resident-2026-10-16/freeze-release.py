from pathlib import Path
from datetime import datetime,timezone
import json,hashlib,tarfile
root=Path.cwd();out=root/'artifacts/vios-resident-2026-10-16'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def describe(p):return {'path':str(p.relative_to(root)),'size':p.stat().st_size,'sha256':digest(p)}
source_hashes=json.loads((out/'source-sha256.json').read_text())
for name,sha in source_hashes.items():assert digest(root/name)==sha,name
fw=json.loads((out/'build-manifest-accepted.json').read_text())
assert digest(out/'firmware-accepted.bin')==fw['sha256']
assert digest(out/'vios-accepted.bin')==fw['embedded_vios']['sha256']
assert digest(out/'agon-stage0.bin')==fw['native_stage0']['sha256']
for name,sha in fw['build_source_sha256'].items():assert digest(root/name)==sha,name
names=['vios-resident-accepted-run3','vios-resident-recovery-run1','vios-resident-power-cycle','vios-resident-mcp-stdio','vios-resident-mcp-tcp','vios-resident-mcp-stdio-proxy','vios-resident-sd-startup-backup','vios-resident-live-rollback']
reports=[]
for name in names:
 p=root/'docs/bring-up'/f'{name}.json';r=json.loads(p.read_text());assert r['status']=='passed',name
 assert all(c['status']=='passed' for c in r['checks']),name
 reports.append({**describe(p),'checks_passed':len(r['checks'])})
(out/'README.md').write_text('''# VIOS accepted resident milestone — 2026-10-16

Use `firmware-accepted.bin` and `vios-accepted.bin`. Earlier `firmware*.bin` and `vios-resident.bin` files in this directory are preserved development/diagnostic candidates. `manifest.json` identifies every file and its digest.

The accepted ESP32 application is 630,544 bytes; update only app0 at 0x10000 after stopping the serial-owning harness. It embeds the 72,292-byte CPU image and 46-byte native SRAM bootstrap. CPU flash was not programmed. The original ESP32 bootloader, partition table and app1 were preserved. The application-only restore image remains at `backups/esp32/pre-vios-resident-app0-2026-10-16.bin`; the runbook supplies its verified command.

`source-code.tar.gz` contains 280 build/source files, including the pinned VDP/library source needed to build, metadata and licenses. `source-sha256.json` verifies those workspace paths. Toolchains and host environments are external dependencies with pinned versions. `documentation-evidence.tar.gz` preserves documentation, test/build logs and serial evidence, including negative attempts. Native linked-code checks can be reproduced with `make test verify-forth-engine`.

`sd-startup/VIOS` is a read-only host backup of the accepted demonstration startup: five exact-source packages, both source bundles and both journal records. Each format CRC was verified before saving; SHA256 values are in the manifest and backup report. Preserve any existing SD contents before using these files for recovery. The accepted demonstration is slot A (11/12); slot B contains v2 (22/23).

The running harness uses `--backend resident --tcp 127.0.0.1:8765`; the human CLI is `.venv/bin/python tools/vios_cli.py`. A standard MCP stdio client can launch `/usr/bin/nc 127.0.0.1 8765` to share that owner. See the live-session record and harness runbook. No source module or file writes are needed for attachment.

Actual hardware checks cover source integrity, all engines, graphics replies, SD save/reload, live rebinding/LIFO rollback, A/B confirmation/revert, physical cold power-on, stuck-KEY/VDP recovery and one automatic watchdog reset. Three consecutive watchdog failures are fixture-tested only. Display/audio protocol readback does not establish physical legibility or audible output. A/B source bundles share one FAT filesystem; ESP firmware A/B rollback is not implemented.

The selected text-first hybrid workbench remains a design proposal. No UI panes/editor are included in this milestone.
''')
manifest={'schema':1,'project':'VIOS','platform':'Agon Light 2 / eZ80F92 / ESP32-PICO-D4','created_utc':datetime.now(timezone.utc).isoformat(),'status':'accepted_on_hardware','accepted_firmware':describe(out/'firmware-accepted.bin'),'accepted_cpu':describe(out/'vios-accepted.bin'),'native_stage0':describe(out/'agon-stage0.bin'),'cpu_layout':{'load_address':0x40000,'text_bytes':72282,'data_bytes':10,'bss_bytes':190082,'bss_end':0x800e6,'stack_bottom':0xaf000,'stack_top':0xb0000,'reserved_stack_bytes':4096,'remaining_before_stack_bytes':192282},'esp32':{'app0_offset':0x10000,'app0_limit':0x140000,'static_ram_bytes':37948,'program_bytes':630173,'runtime_allocations_additional':True},'toolchains':{'agondev':'v0.22','clang':'15.0.7 / AgonPlatform agon-llvm-project c76386c0083e6a6236ff774275227e2389f85538','platformio':'espressif32@6.3.0','arduino_esp32':'2.0.9','xtensa_pio':'8.4.0+2021r2-patch5','ESP32Time':'2.0.0'},'host_native_verification':{'unittest_tests':135,'linked_engine_execution_probes':146,'log':'artifacts/vios-resident-2026-10-16/host-native-checks.txt'},'hardware_reports':reports,'source_files':len(source_hashes),'source_manifest':'artifacts/vios-resident-2026-10-16/source-sha256.json','current_live_session':'docs/bring-up/vios-resident-live-session.json','ui_status':'Selected hybrid text-first workbench; panes/editor not yet implemented','limitations':['No eZ80 flash programming; original MOS remains, kernel uses no MOS calls','A/B source startup shares one FAT32 card; not media-failure isolation or ESP32 firmware rollback','Physical VGA legibility and audible sound unobserved','Three consecutive watchdog failures covered by fixtures; hardware observed first failure and retained count','PS/2 mouse GPIO26/27 reserved for permanent ZDI wires; keyboard-only profile','MCP/JSON/TCP on host; USB is CH340 serial, not native Ethernet'], 'original_backups':[describe(root/p) for p in ['backups/esp32/original-2026-10-16.bin','backups/ez80/original-2026-10-16.bin','backups/esp32/pre-vios-resident-2026-10-16.bin','backups/esp32/pre-vios-resident-app0-2026-10-16.bin']]}
manifest['files']=[describe(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name!='manifest.json']
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Verified source files',len(source_hashes),'hardware reports',len(reports),'artifact files',len(manifest['files']))
