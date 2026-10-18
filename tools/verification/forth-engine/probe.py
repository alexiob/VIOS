"""No-hardware semantic execution of the actual linked disassembly.

This intentionally small interpreter is evidence for control/stack logic, not a
cycle accurate eZ80 emulator or proof of hardware flag behavior.
"""
from pathlib import Path
import re
import sys
ROOT=Path(__file__).resolve().parents[3]
P=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else ROOT/"build/verification/forth-engine"
MASK=0xffffff
class CPU:
 def __init__(self,kind):
  self.kind=kind; self.mem=bytearray(1<<24)
  raw=(P/f'{kind}.bin').read_bytes(); self.mem[0x10000:0x10000+len(raw)]=raw
  self.syms={s[2]:int(s[0],16) for l in (P/f'{kind}.sym').read_text().splitlines() if len(s:=l.split())==3}
  self.ins={}
  for l in (P/f'{kind}-linked.dis').read_text().splitlines():
   m=re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$',l)
   if m:
    a=int(m[1],16); bs=m[2].split(); text=m[3].split(' <')[0].strip(); self.ins[a]=(len(bs),text)
  self.r=dict(hl=0,de=0,bc=0,ix=0x123456,iy=0xabcdef,sp=0xf00000,a=0)
  self.z=self.s=self.p=self.c=False; self.pc=self.syms[f'_fvm_{kind}_run']; self.push(0)
 def rd(self,a,n=3): return int.from_bytes(self.mem[a:a+n],'little')
 def wr(self,a,v,n=3): self.mem[a:a+n]=(v&((1<<(n*8))-1)).to_bytes(n,'little')
 def g(self,x):
  x=x.strip()
  if x in self.r:return self.r[x]
  if x in ['b','c','h','l','d','e']:
   pair={'b':'bc','c':'bc','h':'hl','l':'hl','d':'de','e':'de'}[x]
   return (self.r[pair]>>(8 if x in 'bhd' else 0))&255
  return int(x,0)
 def set(self,x,v):
  if x in self.r:self.r[x]=v&(255 if x=='a' else MASK);return
  pair={'b':'bc','c':'bc','h':'hl','l':'hl','d':'de','e':'de'}[x]; shift=8 if x in 'bhd' else 0
  self.r[pair]=(self.r[pair]&~(255<<shift))|((v&255)<<shift)
 def addr(self,x):
  x=x.strip('()')
  m=re.fullmatch(r'(ix|iy)([+-]\d+)',x)
  return (self.g(m[1])+int(m[2]))&MASK if m else self.g(x)
 def push(self,v):self.r['sp']-=3;self.wr(self.r['sp'],v)
 def pop(self):v=self.rd(self.r['sp']);self.r['sp']+=3;return v
 def cond(self,x):return {'z':self.z,'nz':not self.z,'c':self.c,'nc':not self.c,'m':self.s,'p':not self.s,'pe':self.p,'po':not self.p}[x]
 def flags(self,res,bits=8,carry=False,overflow=None):
  res&=(1<<bits)-1;self.z=res==0;self.s=bool(res&(1<<(bits-1)));self.c=carry;self.p=(res.bit_count()%2==0) if overflow is None else overflow
 def globalset(self,s,v):self.wr(self.syms['_fvm_'+s],v)
 def globalget(self,s):return self.rd(self.syms['_fvm_'+s])
 def stack(self):return [self.rd(self.syms['_fvm_ds']+i*3) for i in range(self.globalget('depth'))]
 def step(self):
  if self.pc==0:return False
  if self.pc not in self.ins:raise AssertionError(f'no instruction {self.pc:x}')
  size,t=self.ins[self.pc]; self.pc+=size
  a=t.split(None,1); op=a[0];args=[x.strip() for x in a[1].split(',')] if len(a)>1 else []
  if op=='ld':
   dst,src=args
   n=1 if (dst in ['a','b','c','h','l','d','e'] or src in ['a','b','c','h','l','d','e']) else 3
   v=self.rd(self.addr(src),n) if src.startswith('(') else self.g(src)
   if dst.startswith('('):self.wr(self.addr(dst),v,n)
   else:self.set(dst,v)
  elif op=='lea':
   dst,src=args; self.set(dst,self.addr('('+src+')'))
  elif op=='push':self.push(self.g(args[0]))
  elif op=='pop':self.set(args[0],self.pop())
  elif op in ['inc','dec']:self.set(args[0],self.g(args[0])+(1 if op=='inc' else -1))
  elif op=='add':
   left=self.g(args[0]);right=self.g(args[1]);res=left+right; self.set(args[0],res);self.c=res>MASK
  elif op=='sbc':
   left=self.g(args[0]);right=self.g(args[1]);res=left-right-self.c
   self.set(args[0],res); self.flags(res,24,res<0,bool(((left^right)&(left^res))&0x800000))
  elif op in ['or','and','xor','cp']:
   left=self.g(args[0]);x=args[1];right=self.rd(self.addr(x),1) if x.startswith('(') else self.g(x)
   if op=='cp':self.flags(left-right,8,left<right)
   else:
    res={'or':lambda:left|right,'and':lambda:left&right,'xor':lambda:left^right}[op]();self.set('a',res);self.flags(res)
  elif op=='cpl':self.set('a',~self.g('a'))
  elif op=='ex':left,right=args;v=self.g(left);self.set(left,self.g(right));self.set(right,v)
  elif op in ['jp','call']:
   if len(args)==2 and not self.cond(args[0]):return True
   dest=args[-1];target=self.g(dest.strip('()'))
   if op=='call':self.push(self.pc)
   self.pc=target
  elif op=='ret':
   if not args or self.cond(args[0]):self.pc=self.pop()
  else:raise AssertionError(t)
  return True
 def run(self,budget=100000):
  for _ in range(budget):
   if not self.step():
    assert self.r['ix']==0x123456 and self.r['iy']==0xabcdef,'C ABI register corruption'
    assert self.r['sp']==0xf00000,'native stack imbalance'
    return self.r['a']
  raise AssertionError('instruction limit')
ops='exit lit call branch zbranch dup drop swap over add sub mul and or xor invert eq lt ult zeroeq fetch store cfetch cstore emit dot cr key tor rfrom rfetch do loop i execute defer div mod lshift rshift depth native'.split()
def assemble(cpu,items,start=0x50000):
 labels={};pc=start
 for x in items:
  if isinstance(x,str) and x.endswith(':'):labels[x[:-1]]=pc
  else:pc+=1 if cpu.kind=='token' and isinstance(x,str) and x in ops else 3
 pc=start
 for x in items:
  if isinstance(x,str) and x.endswith(':'):continue
  if isinstance(x,str) and x in ops:
   n=ops.index(x);v=n if cpu.kind=='token' else cpu.rd(cpu.syms['_fvm_direct_primitives']+n*3);size=1 if cpu.kind=='token' else 3
  else:v=labels[x] if isinstance(x,str) else x;size=3
  cpu.wr(pc,v,size);pc+=size
 cpu.globalset('ip',start);cpu.globalset('budget',10000)
 return labels
count=0
def check(kind,items,expected=None,error=0,trap=0,initial=None,budget=None):
 global count
 c=CPU(kind);assemble(c,items)
 for i,v in enumerate(initial or []):c.wr(c.syms['_fvm_ds']+i*3,v)
 c.globalset('depth',len(initial or []))
 if budget is not None:c.globalset('budget',budget)
 result=c.run();assert result==(255 if error else trap),(kind,items,result,error,trap)
 assert c.globalget('error')==error,(kind,items,c.globalget('error'),error)
 if expected is not None:assert c.stack()==expected,(kind,items,c.stack(),expected)
 count+=1;return c
for kind in ['token','direct']:
 check(kind,['exit'],[])
 check(kind,['lit',0xffffff,'lit',2,'add','exit'],[1])
 check(kind,['lit',0,'lit',1,'sub','exit'],[MASK])
 check(kind,['lit',7,'dup','over','swap','drop','depth','exit'],[7,7,2])
 for op,expected in [('and',0x123456&0xffabcd),('or',0x123456|0xffabcd),('xor',0x123456^0xffabcd)]:check(kind,['lit',0x123456,'lit',0xffabcd,op,'exit'],[expected])
 check(kind,['lit',0x123456,'invert','exit'],[0xedcba9])
 for op in ['lt','ult','eq']:
  for a,b in [(0,0),(0xffffff,0),(0,0xffffff),(0x7fffff,0x800000),(0x800000,0x7fffff),(0x800000,0xffffff),(7,9)]:
   signed=lambda v:v-(1<<24) if v&0x800000 else v
   truth={'lt':signed(a)<signed(b),'ult':a<b,'eq':a==b}[op]
   check(kind,['lit',a,'lit',b,op,'exit'],[MASK if truth else 0])
 check(kind,['lit',0,'zeroeq','lit',0x10000,'zeroeq','exit'],[MASK,0])
 check(kind,['lit',99,'tor','rfetch','rfrom','exit'],[99,99])
 check(kind,['call','word','exit','word:','lit',77,'exit'],[77])
 check(kind,['lit',0,'zbranch','yes','lit',1,'branch','end','yes:','lit',2,'end:','exit'],[2])
 check(kind,['lit',5,'lit',0,'do','body:','i','loop','body','exit'],[0,1,2,3,4])
 check(kind,['lit',0xabcdef,'lit',0x90000,'store','lit',0x90000,'fetch','lit',0x90002,'cfetch','exit'],[0xabcdef,0xab])
 check(kind,['lit',0xdead,'lit',0x90000,'cstore','lit',0x90000,'cfetch','exit'],[0xad])
 for op in ['mul','emit','dot','cr','key','execute','div','mod','lshift','rshift']:check(kind,[op],[],trap=0x80+ops.index(op))
 c=CPU(kind);assemble(c,['defer',0x90000]);c.wr(0x90000,0x123456);assert c.run()==0x80+35 and c.stack()==[0x123456];count+=1
 check(kind,['drop'],error=1)
 check(kind,['swap'],error=1,initial=[1])
 check(kind,['lit',7],error=2,initial=list(range(256)))
 check(kind,['rfrom'],error=3)
 check(kind,['i'],error=5)
 check(kind,['branch','again','again:','branch','again'],error=9,budget=3)
 if kind=='token':
  c=CPU(kind);assemble(c,[255]);c.mem[0x50000]=255;assert c.run()==255 and c.globalget('error')==8;count+=1
 else:
  # Real native CALL DOCOL prologue, followed by threaded LIT/EXIT body.
  c=CPU(kind);assemble(c,['exit']);c.wr(0x50000,0x60000);c.wr(0x50003,c.rd(c.syms['_fvm_direct_primitives']))
  c.mem[0x60000]=0xcd;c.wr(0x60001,c.syms['_fvm_docol']);c.ins[0x60000]=(4,f'call 0x{c.syms["_fvm_docol"]:x}')
  c.wr(0x60004,c.rd(c.syms['_fvm_direct_primitives']+3));c.wr(0x60007,456);c.wr(0x6000a,c.rd(c.syms['_fvm_direct_primitives']))
  assert c.run()==0 and c.stack()==[456] and c.globalget('rdepth')==0;count+=1
print(f'PASS: {count} linked-disassembly execution probes across token/direct, including DOCOL and IX/IY/SP preservation')
# Wraparound/nested loops and boundary/error recovery cases.
for kind in ['token','direct']:
 check(kind,['lit',1,'lit',0xfffffe,'do','body:','i','loop','body','exit'],[0xfffffe,0xffffff,0])
 check(kind,['lit',2,'lit',0,'do','outer:','lit',2,'lit',0,'do','inner:','i','loop','inner','i','loop','outer','exit'],[0,1,0,0,1,1])
 check(kind,['exit'],error=9,budget=0)
 check(kind,['exit'],[],budget=1)
 check(kind,['depth'],error=2,initial=list(range(256)))
 for field,bad,err in [('depth',257,2),('depth',0x10000,2),('rdepth',257,4),('ldepth',33,6)]:
  c=CPU(kind);assemble(c,['exit']);c.globalset(field,bad);assert c.run()==255 and c.globalget('error')==err;count+=1
 c=CPU(kind);assemble(c,['tor']);c.globalset('depth',1);c.globalset('rdepth',256);assert c.run()==255 and c.globalget('error')==4;count+=1
 c=CPU(kind);assemble(c,['do']);c.globalset('ldepth',32);assert c.run()==255 and c.globalget('error')==6;count+=1
 c=CPU(kind);assemble(c,['loop',0x50000]);assert c.run()==255 and c.globalget('error')==5;count+=1
 c=CPU(kind);assemble(c,['exit']);c.globalset('error',7);assert c.run()==255 and c.globalget('error')==7;count+=1
 c=CPU(kind);assemble(c,['cr','lit',456,'exit']);assert c.run()==0x80+26
 # A new C invocation can resume from published IP without resetting budget.
 c.pc=c.syms[f'_fvm_{kind}_run'];c.push(0);assert c.run()==0 and c.stack()==[456] and c.globalget('budget')==9997;count+=1
print(f'PASS: {count} total linked-disassembly execution probes, including wrap/nested loops, corrupt-depth rejection, and trap resumption')
# Peaks must observe transient pushes that are gone when the engine returns.
for kind in ['token','direct']:
 c=check(kind,['call','deep','exit','deep:','lit',1,'dup','dup','dup','drop','drop','drop','drop','exit'],[])
 assert c.globalget('ds_peak')==4 and c.globalget('rs_peak')==1,(kind,'transient peaks',c.globalget('ds_peak'),c.globalget('rs_peak'))
 c=check(kind,['call','outer','exit','outer:','call','inner','exit','inner:','lit',1,'drop','exit'],[])
 assert c.globalget('ds_peak')==1 and c.globalget('rs_peak')==2
 c=CPU(kind);assemble(c,['lit',1,'drop','exit']);c.globalset('ds_peak',17);c.globalset('rs_peak',9)
 assert c.run()==0 and c.globalget('ds_peak')==17 and c.globalget('rs_peak')==9;count+=1
print(f'PASS: {count} total probes, including transient/nested high-water counters and monotonic peak preservation')

# Native services fetch one inline 24-bit ID and resume after the operand.
for kind in ['token','direct']:
 c=check(kind,['native',0x123456,'lit',7,'exit'],[0x123456],trap=0x80+41)
 c.pc=c.syms[f'_fvm_{kind}_run'];c.push(0)
 assert c.run()==0 and c.stack()==[0x123456,7]
 check(kind,['native',1],error=2,initial=list(range(256)))
print(f'PASS: {count} total probes, including native service operand/trap/resume')

# Background tasks preempt by budget exhaustion: the fault leaves IP at the
# unexecuted instruction and depths current, so clearing error and granting a
# new budget resumes exactly. Slices of every size must reproduce the result.
program=['call','work','exit','work:','lit',3,'lit',0,'do','body:','i','lit',10,'mul','tor','rfrom','loop','body','lit',7,'exit']
for kind in ['token','direct']:
 for slice_size in [1,2,3,5,8]:
  c=CPU(kind);assemble(c,program);c.globalset('budget',slice_size);slices=0
  while True:
   result=c.run();slices+=1
   if result==0x80+ops.index('mul'):
    b,a=c.stack()[-1],c.stack()[-2];c.globalset('depth',c.globalget('depth')-2);c.push_ds=None
    d=c.globalget('depth');c.wr(c.syms['_fvm_ds']+d*3,(a*b)&MASK);c.globalset('depth',d+1)
   elif result==255 and c.globalget('error')==9:
    c.globalset('error',0);c.globalset('budget',slice_size)
   else:break
   c.pc=c.syms[f'_fvm_{kind}_run'];c.push(0)
  assert result==0 and c.stack()==[0,10,20,7] and c.globalget('rdepth')==0 and c.globalget('ldepth')==0,(kind,slice_size,result,c.stack())
  assert slices>3,(kind,slice_size,slices);count+=1
print(f'PASS: {count} total probes, including budget-exhaustion preemption and exact resume')
