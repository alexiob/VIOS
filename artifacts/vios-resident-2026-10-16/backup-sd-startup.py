"""Run from repository root against its existing resident TCP harness.
Reads the accepted test startup files, then explicitly reboots to reclaim RAM.
"""
import sys,json,re,zlib,hashlib
from pathlib import Path
sys.path.insert(0,'tools')
from vios_cli import RPCClient
from vios_resident_acceptance import Report,require,synchronized
out=Path('artifacts/vios-resident-2026-10-16/sd-startup')
r=Report('docs/bring-up/vios-resident-sd-startup-backup.json','SDBACK1',['read-only-sd-backup','reboot'])
a=json.loads(Path('docs/bring-up/vios-resident-accepted-run3.json').read_text())
paths=sorted({c['result']['path'] for c in a['checks'] if c['name'].startswith('module-save')})
paths+=['/VIOS/BOOT_A.VIB','/VIOS/BOOT_B.VIB','/VIOS/BOOT0.REC','/VIOS/BOOT1.REC']
# Numeric echo tokens must be excluded: only parse lines consisting of numbers.
def numeric_lines(d):
 result=[]
 for line in d['output'].replace('\r','').splitlines():
  if re.fullmatch(r'\s*\d+(?:\s+\d+)*\s*',line):result.extend(map(int,line.split()))
 return result
with RPCClient() as c:
 c.initialize();require(c.tool('connect').get('connected'),'Harness is not connected')
 def allocate():
  source='S" '+'.'*1024+'" DROP CONSTANT _VBACK'
  synchronized(c.tool('module_stage',{'name':'SDBACKTEMP','version':'1','source':source}))
  synchronized(c.tool('module_activate',{'name':'SDBACKTEMP'}))
  d=synchronized(c.tool('evaluate',{'source':'_VBACK . CR'}));n=numeric_lines(d);require(len(n)==1,'Cannot obtain native buffer address');return {'address':n[0],'reserved_bytes':1025,'temporary_module':'SDBACKTEMP'}
 address=r.check('temporary-module-buffer',allocate)['address']
 for path in paths:
  def read(path=path):
   d=synchronized(c.tool('evaluate',{'source':f'DECIMAL S" {path}" _VBACK 1024 FILE-READ . . CR'}));n=numeric_lines(d)
   require(len(n)==2 and n[0]==0 and 4<=n[1]<=1024,'File read failed: '+repr(d))
   data=bytes.fromhex(c.tool('memory_read',{'address':address,'count':n[1]})['hex'])
   require(data[:4] in [b'VIM1',b'VIB1',b'VIR1'],'Unknown file format')
   require(zlib.crc32(data[:-4])==int.from_bytes(data[-4:],'little'),'File CRC mismatch')
   dest=out/path.lstrip('/');dest.parent.mkdir(parents=True,exist_ok=True);require(not dest.exists(),'Refuse overwrite');dest.write_bytes(data)
   return {'sd_path':path,'host_path':str(dest),'size':len(data),'sha256':hashlib.sha256(data).hexdigest(),'format_crc_verified':True}
  r.check('backup-'+path,read)
 r.check('explicit-mcp-reboot-to-reclaim-temporary-buffer',lambda:synchronized(c.tool('reboot')))
 def boot():
  d=synchronized(c.tool('boot_status'));require(d['boot']['confirmed']=='A' and d['boot']['state']=='CLEAN','Wrong startup');return d
 r.check('confirmed-startup-preserved',boot)
 r.check('clean-resources',lambda:synchronized(c.tool('resources')))
 r.data['status']='passed';r.save()
