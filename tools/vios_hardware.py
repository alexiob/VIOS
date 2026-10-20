#!/usr/bin/env python3
"""VIOS RAM deployment, differential hardware checks and timer benchmarks."""
import argparse, hashlib, json, re, secrets, time, zlib
from pathlib import Path
from serial_monitor import validate_image
from zdi_backup import ask,load_probe,read_block

def deploy(session,data):
 validate_image(data)
 session.write(b'2')  # Conservative 10 us debug edges over breadboard leads.
 identity=ask(session,b'I');print(identity.decode(),flush=True)
 if not identity.startswith(b'ID 0007 '):raise IOError('CPU identity mismatch')
 if not ask(session,b'H').startswith(b'REGS 98 '):raise IOError('CPU halt failed')
 mapping=ask(session,b'D')
 fields=mapping.split()
 if len(fields)!=5 or fields[0]!=b'CS0':raise IOError(f'RAM mapping {mapping!r}')
 lower,upper,control,mode=(int(x,16) for x in fields[1:])
 if not (lower<=4 and upper>=10 and control&15==8 and mode&3==1):raise IOError(f'RAM mapping {mapping!r}')
 readback=bytearray()
 for offset in range(0,len(data),256):
  block=data[offset:offset+256];address=0x40000+offset;crc=zlib.crc32(block)
  cmd=f'W{address:06X}{len(block):04X}{crc:08X}'.encode()+block.hex().upper().encode()
  if ask(session,cmd)!=f'WROTE {address:06X} {len(block):04X} {crc:08X}'.encode():raise IOError('RAM write failed')
  got=read_block(session,address,len(block),'V')
  if got!=block:raise IOError('RAM readback mismatch')
  readback.extend(got)
 digest=hashlib.sha256(readback).hexdigest();print('RAM verified',len(data),'bytes',digest,flush=True)
 session.write(b'X');boot=session.until(b'vios> ',20)
 print(boot.decode('ascii','replace'),end='',flush=True)
 return {'identity':identity.decode(),'mapping':mapping.decode(),'readback_sha256':digest,'boot':boot.decode('ascii','replace')}

def command(session,source,timeout=15):
 nonce=1000000+secrets.randbelow(6000000)
 session.command(source)
 # The eZ80 RX FIFO is only16 bytes and timed workloads do not poll it.
 # Wait for the complete REPL line before sending the longer fresh nonce.
 result=session.until(b'\r\nvios> ',timeout)
 session.command(f'DECIMAL {nonce} . CR')
 marker=f'\r\n{nonce} \r\nok\r\nvios> '.encode()
 session.until(marker,timeout)
 return result

def checks(session):
 evidence=[]
 for engine in ['C','TOKEN','DIRECT']:
  command(session,'ENGINE '+engine)
  programs=[
   (': SQUARE DUP * ; 7 SQUARE . CR',b'\r\n49 \r\n'),
   (': ABS DUP 0 < IF NEGATE THEN ; -7 ABS SQUARE . CR',b'\r\n49 \r\n'),
   (': SUM 0 SWAP 0 DO I + LOOP ; 10 SUM . CR',b'\r\n45 \r\n'),
   (': DOWN BEGIN 1- DUP 0= UNTIL ; 12 DOWN . CR',b'\r\n0 \r\n'),
   (': INNER 0 SWAP 0 DO I + LOOP ; : NEST 0 6 1 DO I INNER + LOOP ; NEST . CR',b'\r\n20 \r\n'),
   ('VARIABLE X 123456 X ! X @ . CR',b'\r\n123456 \r\n'),
   ('$FFFFFF 1 + . $800000 0 < . $7FFFFF -1 < . CR',b'\r\n0 -1 0 \r\n'),
   ('-7 3 / . -7 3 MOD . 1 23 LSHIFT . $800000 23 RSHIFT . CR',b'\r\n-2 -1 -8388608 1 \r\n'),
   (': RTEST 7 >R 2 R> + ; RTEST . CR',b'\r\n9 \r\n'),
   ("' SQUARE DROP",b'ok\r\n'),
   ('4 \' SQUARE EXECUTE . CR',b'\r\n16 \r\n'),
   (': ONE 11 ; : TWO 22 ; DEFER SERVICE \' ONE IS SERVICE : CLIENT SERVICE . ; CLIENT CR \' TWO IS SERVICE CLIENT CR',b'11 \r\n22 \r\n'),
   (': BAD 1 MISSING ;',b'error: unknown word: MISSING'),
   ('BAD',b'error: unknown word: BAD'),
   ('DROP DROP',b'error: data stack underflow'),
   ('1 0 /',b'error: arithmetic error'),
   ('3 4 + . CR',b'\r\n7 \r\n'),
   ('SEE CLIENT',b': CLIENT SERVICE . ;'),
  ]
  # No residual values between examples.
  command(session,'ABORT')
  for source,expected in programs:
   result=command(session,source)
   if expected not in result:raise IOError(f'{engine} {source!r}: {result!r}')
   if b'error:' in result and b'error:' not in expected:raise IOError(f'{engine} unexpected error {result!r}')
   evidence.append({'engine':engine,'source':source,'response':result.decode('ascii','replace')})
  print(engine,'hardware language checks passed',flush=True)
 return evidence

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--port',default='/dev/cu.usbserial-224130');p.add_argument('--image',type=Path,default=Path('build/vios.bin'))
 p.add_argument('--log',type=Path,default=Path('docs/bring-up/vios-hardware.jsonl'));p.add_argument('--no-checks',action='store_true');p.add_argument('--no-bench',action='store_true');p.add_argument('--command',action='append',default=[])
 args=p.parse_args();data=args.image.read_bytes();validate_image(data)
 session=load_probe(args.port,Path('build/zdi-loader.bin'),args.log)
 evidence={'started_unix':time.time(),'image':str(args.image),'sha256':hashlib.sha256(data).hexdigest(),'size':len(data)}
 try:
  evidence.update(deploy(session,data))
  if not args.no_checks:evidence['checks']=checks(session)
  for source in args.command:print(command(session,source).decode('ascii','replace'),flush=True)
  if not args.no_bench:
   result=command(session,'BENCH',60)
   print(result.decode('ascii','replace'),flush=True)
   if b'BENCH DONE' not in result or b'BENCH ERROR' in result:raise IOError('benchmark failed')
   args.log.with_suffix('.bench.txt').write_text(result.decode('ascii','replace'))
   evidence['benchmark']=result.decode('ascii','replace')
  evidence['resources']=command(session,'RESOURCES').decode('ascii','replace')
  print(evidence['resources'],flush=True)
  evidence['verified']=True
 except BaseException:
  session.write(b'\x1d'*3);print('Debug recovery',repr(session.listen(.5)),flush=True);raise
 finally:
  evidence['finished_unix']=time.time();args.log.with_suffix('.summary.json').write_text(json.dumps(evidence,indent=2)+'\n');session.close()

if __name__=='__main__':main()
