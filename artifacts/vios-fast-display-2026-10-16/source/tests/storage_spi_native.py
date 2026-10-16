"""Semantic execution of assembled SPI routines, including real IN0 flags."""
from pathlib import Path
import re
import subprocess
import os

class NativeSPI:
    def __init__(self,root,out):
        self.root=root;self.out=out;out.mkdir(parents=True,exist_ok=True)
        prefix=str(Path(os.environ.get('AGONDEV',str(root/'.tools/agondev')))/'bin/ez80-none-elf')
        subprocess.run([prefix+'-as','-march=ez80+full','-o',str(out/'spi.o'),str(root/'arch/ez80/spi.s')],check=True,capture_output=True)
        subprocess.run([prefix+'-ld','-Ttext=0x1000','-e','_ez80_spi_init','-o',str(out/'spi.elf'),str(out/'spi.o')],check=True,capture_output=True)
        listing=subprocess.check_output([prefix+'-objdump','-d',str(out/'spi.elf')],text=True)
        (out/'spi.dis').write_text(listing)
        self.ins={}
        for line in listing.splitlines():
            m=re.match(r'\s*([0-9a-f]+):\s*((?:[0-9a-f]{2} )+)\s*([^\t]+)\s*$',line)
            if m:self.ins[int(m[1],16)]=(len(m[2].split()),m[3].split(' <')[0].strip())
        self.symbols={s[2]:int(s[0],16) for line in subprocess.check_output([prefix+'-nm',str(out/'spi.elf')],text=True).splitlines() if len(s:=line.split())==3}
        self.io={};self.tx=[];self.rx=0xa5
    def call(self,name,args,status=0x80):
        r=dict(hl=0,bc=0,de=0,sp=0x80000,a=0);mem=bytearray(0x100000);zero=False
        for i,value in enumerate(args):mem[0x80003+i*3:0x80006+i*3]=value.to_bytes(3,'little')
        def get(x):
            if x in r:return r[x]
            if x in 'bcdehl' and len(x)==1:
                pair={'b':'bc','c':'bc','d':'de','e':'de','h':'hl','l':'hl'}[x]
                return (r[pair]>>(8 if x in 'bdh' else 0))&255
            return int(x,0)
        def put(x,v):
            if x in r:r[x]=v&(255 if x=='a' else 0xffffff);return
            pair={'b':'bc','c':'bc','d':'de','e':'de','h':'hl','l':'hl'}[x];shift=8 if x in 'bdh' else 0
            r[pair]=(r[pair]&~(255<<shift))|((v&255)<<shift)
        pc=self.symbols['_ez80_spi_'+name]
        for _ in range(1000000):
            size,text=self.ins[pc];pc+=size;mn,*rest=text.split(None,1);a=[x.strip() for x in rest[0].split(',')] if rest else []
            if mn=='ret':self.memory=mem;return r['a']
            if mn=='ld':
                dst,src=a;n=1 if dst in ['a','b','c','d','e','h','l'] or src in ['a','b','c','d','e','h','l'] else 3
                value=int.from_bytes(mem[get(src[1:-1]):get(src[1:-1])+n],'little') if src.startswith('(') else get(src)
                if dst.startswith('('):at=get(dst[1:-1]);mem[at:at+n]=(value&((1<<(n*8))-1)).to_bytes(n,'little')
                else:put(dst,value)
            elif mn=='add':put(a[0],get(a[0])+get(a[1]))
            elif mn=='dec':put(a[0],get(a[0])-1)
            elif mn in ['and','or','xor']:
                lhs=get(a[0]);rhs=get(a[1]);value={'and':lhs&rhs,'or':lhs|rhs,'xor':lhs^rhs}[mn];put('a',value);zero=not(value&255)
            elif mn=='in0':
                port=int(a[1][1:-1],0);value=status if port==0xbb else self.rx if port==0xbc else self.io.get(port,0)
                put(a[0],value);zero=value==0
            elif mn=='out0':
                port=int(a[0][1:-1],0);value=get(a[1]);self.io[port]=value
                if port==0xbc:self.tx.append(value)
            elif mn=='jr':
                if len(a)==1 or (zero if a[0]=='z' else not zero):pc=int(a[-1],0)
            else:raise AssertionError(text)
        raise AssertionError('native instruction budget exceeded')
