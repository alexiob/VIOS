import pathlib, subprocess, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class TextScreenTests(unittest.TestCase):
 def test_native_dirty_renderer(self):
  binary=ROOT/'build/tests/text-screen';binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude','tests/text_screen_native.c','drivers/agon-vdp/text_screen.c','-o',str(binary)],cwd=ROOT,check=True)
  subprocess.run([str(binary)],check=True,capture_output=True,timeout=5)

 def test_native_scroll_and_compressed_framebuffer(self):
  binary=ROOT/'build/tests/text-screen-scroll';binary.parent.mkdir(parents=True,exist_ok=True)
  subprocess.run(['cc','-std=c99','-pedantic','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g','-Iinclude','tests/text_screen_scroll_native.c','drivers/agon-vdp/text_screen.c','-o',str(binary)],cwd=ROOT,check=True)
  result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
  self.assertEqual(result.returncode,0,result.stdout+result.stderr)

 def test_pending_is_constant_time_in_actual_target_code(self):
  # Bound target instructions rather than wall-clock timing. The empty check
  # must not inspect either 4,800-cell plane on the 18.432 MHz CPU.
  from test_vios_native_c import LinkedC, TargetImage
  target=ROOT/'build/verification/text-pending';target.mkdir(parents=True,exist_ok=True)
  prefix=ROOT/'.tools/agondev/bin/ez80-none-elf'
  subprocess.run([str(prefix)+'-clang','-target','ez80-none-elf','-mllvm','-z80-gas-style','-mllvm','-z80-print-zero-offset','-std=c99','-ffreestanding','-fno-builtin','-fomit-frame-pointer','-Oz','-Wall','-Wextra','-Werror','-Wa,-march=ez80+full','-nostdinc','-Iinclude','-I.','-isystem',str(ROOT/'.tools/agondev/include'),'-c','drivers/agon-vdp/text_screen.c','-o',str(target/'screen.o')],cwd=ROOT,check=True)
  (target/'stub.s').write_text('.section .text\n.global _image_start\n_image_start:\n ret\n.global _platform_vdp_send\n_platform_vdp_send:\n ret\n')
  subprocess.run([str(prefix)+'-as','-march=ez80+full',str(target/'stub.s'),'-o',str(target/'stub.o')],cwd=ROOT,check=True)
  subprocess.run([str(prefix)+'-ld','-T','boards/agon-light-2/ram.ld','-o',str(target/'screen.elf'),str(target/'stub.o'),str(target/'screen.o'),'-L'+str(ROOT/'.tools/agondev/lib'),'-lagon'],cwd=ROOT,check=True)
  subprocess.run([str(prefix)+'-objcopy','-O','binary',str(target/'screen.elf'),str(target/'screen.bin')],cwd=ROOT,check=True)
  image=TargetImage(target/'screen.elf')
  class PendingCPU(LinkedC):
   def rd(self,address,n=3):
    for name in ('_desired','_painted'):
     begin=self.syms[name]
     assert not (address<begin+9600 and address+n>begin), 'pending scanned a text plane'
    return super().rd(address,n)
  cpu=PendingCPU(image)
  self.assertEqual(cpu.call('_vios_text_pending',budget=64)&0xffffff,0)
  cpu.wr(image.syms['_dirty_cells'],4799)
  self.assertEqual(cpu.call('_vios_text_pending',budget=64)&0xffffff,1)
  cpu.wr(image.syms['_dirty_cells'],0)
  cpu.wr(image.syms['_scroll_pending'],1,1)
  self.assertEqual(cpu.call('_vios_text_pending',budget=64)&0xffffff,1)
  class SparseCPU(LinkedC):
   def __init__(self,image):
    super().__init__(image);self.steps=0;self.plane_reads=0;self.row_reads=0
    for name in ('__imulu','__idivu','__ishl','__ishru','__ishrs','__ladd','__bshru','__bshl','_platform_vdp_send'):
     if name in self.syms:self.hooks[self.syms[name]]=name
   def wr(self,address,value,n=3):
    if getattr(self,'scroll_bounds',False):
     for name in ('_desired','_painted'):
      offset=address-self.syms[name]
      if 0<=offset<9600:
       row,col=divmod(offset//2,80)
       end_row,end_col=divmod((offset+n-1)//2,80)
       assert 18<=row<56 and col>=26 and end_row==row and end_col<80,(hex(self.pc),name,row,col,n)
    return super().wr(address,value,n)
   def rd(self,address,n=3):
    for name in ('_desired','_painted'):
     if self.syms[name]<=address<self.syms[name]+9600:self.plane_reads+=1
    if '_dirty_rows' in self.syms and self.syms['_dirty_rows']<=address<self.syms['_dirty_rows']+60:self.row_reads+=1
    return super().rd(address,n)
   def step(self):
    self.steps+=1
    if self.pc in self.hooks:return super().step()
    if self.pc and self.ins[self.pc][1].startswith('ld (') and ',0x' in self.ins[self.pc][1]:
     size,operation=self.ins[self.pc];self.pc+=size;dest,value=operation[3:].split(',');self.wr(self.addr(dest),int(value,16),1);return True
    if self.pc and self.ins[self.pc][1]=='rla':
     self.pc+=self.ins[self.pc][0];old=self.r['a'];self.set('a',(old<<1)|self.c);self.c=bool(old&128);return True
    if self.pc and self.ins[self.pc][1]=='ldir':
     # Count each actual repeated byte-transfer instruction, rather than
     # hiding libc memcpy behind a host helper.
     size,_=self.ins[self.pc];self.wr(self.r['de'],self.rd(self.r['hl'],1),1)
     self.set('de',self.r['de']+1);self.set('hl',self.r['hl']+1);self.set('bc',self.r['bc']-1)
     self.p=bool(self.r['bc'])
     if not self.r['bc']:self.pc+=size
     return True
    if self.pc and self.ins[self.pc][1] in ('inc (hl)','dec (hl)'):
     size,operation=self.ins[self.pc];self.pc+=size;old=self.rd(self.r['hl'],1)
     value=old+(1 if operation.startswith('inc') else -1);carry=self.c
     self.wr(self.r['hl'],value,1);self.flags(value,8,carry,old==(127 if operation.startswith('inc') else 128));return True
    if self.pc and self.ins[self.pc][1].startswith('adc a,'):
     size,operation=self.ins[self.pc];self.pc+=size;left=self.r['a'];right=self.g(operation.split(',')[1]);result=left+right+self.c;self.set('a',result);self.flags(result,8,result>255,bool((~(left^right)&(left^result))&128));return True
    if self.pc and self.ins[self.pc][1].startswith('sub a,'):
     size,operation=self.ins[self.pc];self.pc+=size;left=self.r['a'];right=self.g(operation.split(',')[1]);result=left-right;self.set('a',result);self.flags(result,8,result<0,bool(((left^right)&(left^result))&128));return True
    if self.pc and self.ins[self.pc][1].startswith('mlt '):
     size,operation=self.ins[self.pc];self.pc+=size;pair=operation.split()[1];value=self.g(pair);self.set(pair,(value&255)*((value>>8)&255));return True
    if self.pc and self.ins[self.pc][1]=='scf':
     self.pc+=self.ins[self.pc][0];self.c=True;return True
    if self.pc and self.ins[self.pc][1] in ('push af','pop af'):
     size,operation=self.ins[self.pc];self.pc+=size
     if operation=='push af':
      flags=(128 if self.s else 0)|(64 if self.z else 0)|(4 if self.p else 0)|(1 if self.c else 0)
      self.push((self.r['a']<<8)|flags)
     else:
      value=self.pop();self.set('a',value>>8);self.s=bool(value&128);self.z=bool(value&64);self.p=bool(value&4);self.c=bool(value&1)
     return True
    return super().step()
   def helper(self,name):
    if name=='__imulu':self.set('hl',self.r['hl']*self.r['bc'])
    elif name=='__idivu':self.set('hl',self.r['hl']//self.r['bc'])
    elif name=='__ishl':self.set('hl',self.r['hl']<<self.g('c'))
    elif name=='__ishru':self.set('hl',self.r['hl']>>self.g('c'))
    elif name=='__ishrs':
     value=self.r['hl'];value=value-(1<<24) if value&(1<<23) else value;self.set('hl',value>>self.g('c'))
    elif name=='__bshru':self.set('a',self.r['a']>>self.g('b'))
    elif name=='__bshl':self.set('a',self.r['a']<<self.g('b'))
    elif name=='__ladd':
     result=(self.r['hl']|(self.g('e')<<24))+(self.r['bc']|(self.r['a']<<24))
     self.set('hl',result);self.set('e',result>>24)
    elif name=='_platform_vdp_send':
     start=self.rd(self.r['sp']+3);length=self.rd(self.r['sp']+6)
     self.output.extend(self.mem[start:start+length]);self.clobber()
    else:return super().helper(name)
    self.pc=self.pop()
  # Sparse dirt across 59 clean rows stays bounded. A second case forces
  # scanning the suffix and wrapping back before the starting column.
  # The span setup of the pointer-walk render loop costs ~25 fixed steps.
  for cursor,budget,max_reads in ((80,2100,20),(2,5000,340)):
   sparse=SparseCPU(image)
   for name in ('_desired','_painted'):
    start=image.syms[name];sparse.mem[start:start+9600]=bytes((32,3))*4800
   sparse.wr(image.syms['_paint_cursor'],cursor);sparse.wr(image.syms['_dirty_cells'],1)
   if '_dirty_rows' in image.syms:sparse.wr(image.syms['_dirty_rows'],1,1)
   sparse.wr(image.syms['_desired']+2,ord('X'),1)
   self.assertEqual(sparse.call('_vios_text_render',[8],budget=budget)&0xffffff,8)
   self.assertLessEqual(sparse.plane_reads,max_reads)
   self.assertLessEqual(sparse.row_reads,64)
   self.assertEqual(sparse.output,bytes((17,3,17,128,31,1,0,ord('X'))))
   stats=image.syms['_stats']
   self.assertEqual([sparse.rd(stats+i*4,4) for i in range(4)],[1,1,1,8])
   self.assertEqual(sparse.call('_vios_text_pending',budget=64)&0xffffff,0)

  # A real editor scroll moves both 54x38 shadow rectangles with the linked
  # target memcpy/LDIR. This semantic-step bound rejects per-cell indexing;
  # arithmetic compiler helpers are intercepted, so this is not a cycle count.
  for displacement in (1,-1):
   scroll=SparseCPU(image)
   original=bytes(value for row in range(60) for _ in range(80) for value in (65+row%26,row%4))
   for name in ('_desired','_painted'):
    start=image.syms[name];scroll.mem[start:start+9600]=original
   scroll.scroll_bounds=True
   result=scroll.call('_vios_text_scroll',[26,18,54,38,displacement],budget=20000)&0xffffff
   self.assertEqual(result,1)
   self.assertLess(scroll.steps,20000)
   for name in ('_desired','_painted'):
    start=image.syms[name]
    for row in range(60):
     for col in range(80):
      index=row*80+col;source=row+displacement
      want=(bytes((65+source%26,source%4)) if 18<=source<56 else bytes((32,3))) if 18<=row<56 and col>=26 else original[index*2:index*2+2]
      self.assertEqual(scroll.mem[start+index*2:start+index*2+2],want,(displacement,name,row,col))
   self.assertEqual(scroll.call('_vios_text_pending',budget=64)&0xffffff,1)
