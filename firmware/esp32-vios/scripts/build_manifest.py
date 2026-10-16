"""Record the exact built firmware and its embedded payload after linking."""
Import('env')
from pathlib import Path
import hashlib,json
project=Path(env.subst('$PROJECT_DIR'))
root=project.parents[1]
embedded=json.loads((project/'include/vios_image.json').read_text())
def write_manifest(source,target,env):
    binary=Path(str(target[0])).resolve()
    firmware=binary.read_bytes()
    if len(firmware)>0x140000:
        raise RuntimeError('Firmware exceeds preserved app0 slot')
    paths=[project/'platformio.ini',project/'scripts/embed_vios.py',project/'scripts/build_manifest.py',root/'include/vios/wire.h',root/'include/vios/internal_uart.h',root/'drivers/vios-wire/wire.c',root/'arch/ez80/agon_stage0.s']
    paths+=list((project/'include').glob('*.h'))
    paths+=list((project/'vendor/agon-vdp/video').glob('*'))
    # PlatformIO creates video.ino.cpp transiently and deletes it after building.
    # Hash the durable .ino source instead of claiming the generated file exists.
    hashes={str(path.relative_to(root)):hashlib.sha256(path.read_bytes()).hexdigest() for path in paths
            if path.is_file() and not path.name.endswith('.ino.cpp')}
    manifest={'firmware':str(binary.relative_to(root)), 'size':len(firmware),'sha256':hashlib.sha256(firmware).hexdigest(),
              'app0_offset':0x10000,'app0_limit':0x140000,'embedded_vios':embedded,'internal_baud':embedded['internal_baud'],'usb_baud':115200,
              'native_stage0':json.loads((project/'include/vios_stage0.json').read_text()),
              'profile':env.subst('$PIOENV'),'embedded_boot_enabled':env.subst('$PIOENV')!='esp32-vios-probe',
              'sources':json.loads((project/'vendor/manifest.json').read_text()),'build_source_sha256':hashes,
              'platform':'espressif32@6.3.0','arduino':'2.0.9','toolchain':'8.4.0+2021r2-patch5','ESP32Time':'2.0.0'}
    (project/('build-manifest-probe.json' if env.subst('$PIOENV')=='esp32-vios-probe' else 'build-manifest.json')).write_text(json.dumps(manifest,indent=2)+'\n')
env.AddPostAction(env.subst('$BUILD_DIR/${PROGNAME}.bin'),write_manifest)
