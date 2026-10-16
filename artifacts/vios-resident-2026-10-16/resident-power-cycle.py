import sys,time,json
from pathlib import Path
sys.path.insert(0,'tools')
from vios_resident_board import ResidentBoard
from vios_resident_acceptance import Report,require,synchronized,assert_numbers
port=Path('/dev/cu.usbserial-22430')
r=Report('docs/bring-up/vios-resident-power-cycle.json','POWER1',['physical-power-cycle'])
b=None
try:
 def removal():
  require(port.exists(),'Expected initially connected board')
  started=time.monotonic()
  while port.exists():
   require(time.monotonic()-started<600,'No physical disconnect observed')
   time.sleep(.1)
  removed=time.monotonic();print('USB removal observed',flush=True)
  while not port.exists():
   require(time.monotonic()-removed<600,'USB device did not return')
   time.sleep(.1)
  return {'physical_usb_removal_observed':True,'absence_seconds':round(time.monotonic()-removed,3),'port':str(port),'assumption':'User disconnects all board power, including any battery, for this test'}
 r.check('observe-usb-disconnect-and-reappearance',removal)
 b=ResidentBoard(log='docs/bring-up/vios-resident-power-cycle.serial.jsonl')
 r.check('attach-no-host-upload',lambda:synchronized(b.connect(120)))
 def boot():
  d=synchronized(b.boot_status());v=d['boot'];require(v['confirmed']=='A' and v['current']=='A' and v['state']=='CLEAN' and v['candidate']=='NONE','Unexpected SD selection');return d
 r.check('confirmed-sd-slot-A',boot)
 def values():
  s='DECIMAL LIVE1-C . LIVE1-D . CR';d=synchronized(b.evaluate(s));d['numbers']=assert_numbers(d,s,[11,12]);return d
 r.check('persisted-deferred-service-and-dependent',values)
 r.check('resources',lambda:synchronized(b.resources()))
 def video():
  d=synchronized(b.evaluate('VDP-INFO'));require('width 640 height 480 colours 4 mode 1 console-dropped 0' in d['output'],'Unexpected video/input state');return d
 r.check('vdp-mode-and-no-receive-drops',video)
 r.data['status']='passed';r.save()
except BaseException as e:r.data.update(status='failed',error=repr(e));r.save();raise
finally:
 if b:b.close()
