"""Execute native action graphs and compare the production compact timeline.
Usage: python tests/native_wave_timeline.py arm64-lib cocos-dll game-exe
Uses real Sequence/Spawn/Ease/Tween update instructions. Action graph allocation,
start/stop bookkeeping, powf and delegate callbacks are modeled.
"""
import ctypes
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.arm64_const import *
from unicorn.x86_const import *
root=Path(__file__).resolve().parents[1]
arm=Path(sys.argv[1]).read_bytes()
with open(sys.argv[1],'rb') as f:
    elf=ELFFile(f);segments=[(s['p_vaddr'],s['p_offset'],s['p_filesz']) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
def readarm(a,n):
    v,o,_=next(s for s in segments if s[0]<=a and a+n<=s[0]+s[2]);return arm[o+a-v:o+a-v+n]
def pe_reader(path):
    b=Path(path).read_bytes();h=struct.unpack_from('<I',b,0x3c)[0];sec=h+24+struct.unpack_from('<H',b,h+20)[0]
    sections=[struct.unpack_from('<IIII',b,sec+i*40+8) for i in range(struct.unpack_from('<H',b,h+6)[0])]
    def read(a,n):
        _,v,_,o=next(s for s in sections if s[1]<=a and a+n<=s[1]+s[2]);return b[o+a-v:o+a-v+n]
    return read
readwin=pe_reader(sys.argv[2]);readexe=pe_reader(sys.argv[3])
def fnv(b):
    value=14695981039346656037
    for c in b:value=((value^c)*1099511628211)&((1<<64)-1)
    return value
source=(root/'src/Rendering/wave_animation.cpp').read_text()
for name,reader in [('armInit',readarm),('armTween',readarm),('armEase',readarm),('winInit',readexe),('winTween',readwin),('winEase',readwin)]:
    a,n,h=(int(x,16) for x in re.search(name+r'\{(0x\w+), (0x\w+), (0x\w+)\}',source).groups())
    assert fnv(reader(a,n))==h,name
libm=ctypes.CDLL('libm.so.6');libm.powf.argtypes=[ctypes.c_float,ctypes.c_float];libm.powf.restype=ctypes.c_float
def f32(v):return struct.unpack('<f',struct.pack('<f',v))[0]
def bits(v):return struct.unpack('<I',struct.pack('<f',v))[0]
def float_of(v):return struct.unpack('<f',struct.pack('<I',v&0xffffffff))[0]

class Native:
    def __init__(self,fused,fade,ease,from_radius,to_radius):
        self.fused=fused;self.nodes={};self.events=[];self.next=0x2003000
        self.u=Uc(UC_ARCH_ARM64,UC_MODE_ARM) if fused else Uc(UC_ARCH_X86,UC_MODE_64)
        self.pages=set()
        self.map(0x1000000,0x10000);self.map(0x2000000,0x10000);self.map(0x3000000,0x10000)
        self.start,self.stop,self.done,self.callback,self.remove,self.end= [0x1000100+i*0x100 for i in range(6)]
        for a in [self.start,self.stop,self.done,self.callback,self.remove,self.end]:
            self.u.mem_write(a,struct.pack('<I',0xd65f03c0) if fused else b'\xc3')
        if fused:
            self.functions={'sequence':(0xabe9cc,0x194),'spawn':(0xabeb7c,0x54),'tween':(0xac9ba8,0x74),'ease':(0xab69e0,0x40)}
            self.power=0x4d9b20
            self.update_slot,self.done_slot,self.start_slot,self.stop_slot=0x28,0x50,0x58,0x60
            self.wave_slot=0x360
        else:
            self.functions={'sequence':(0x180037430,0x114),'spawn':(0x1800375a0,0x60),'tween':(0x18003d1a0,0x79),'ease':(0x18002a4d0,0x3d)}
            self.power=0x1800e0594
            self.update_slot,self.done_slot,self.start_slot,self.stop_slot=0x20,0x48,0x50,0x58
            self.wave_slot=0x358
            self.map(0x1800e6000,4096);self.u.mem_write(0x1800e6000,readwin(0xe6000,4096))
        for a,n in self.functions.values():
            self.map(a,n);self.u.mem_write(a,readarm(a,n) if fused else readwin(a-0x180000000,n))
        self.map(self.power,8);self.u.mem_write(self.power,struct.pack('<I',0xd65f03c0) if fused else b'\xc3')
        self.wave=0x2000000;wave_vtable=0x2000400
        self.put64(self.wave,wave_vtable);self.put64(wave_vtable+self.wave_slot,self.callback)
        self.key_radius,self.key_opacity=0x2000800,0x2000820
        self.u.mem_write(self.key_radius,b'radius\0');self.u.mem_write(self.key_opacity,b'opacity\0')
        radius=self.node('tween',1,from_=from_radius,to=to_radius,key=self.key_radius)
        if not fade:
            opacity=self.node('tween',1,from_=255,to=0,key=self.key_opacity)
            if ease:
                radius=self.node('ease',1,children=[radius]);opacity=self.node('ease',1,children=[opacity])
            parallel=self.node('spawn',1,children=[radius,opacity])
            remove=self.node('remove',0)
            self.root=self.node('sequence',1,children=[parallel,remove])
        else:
            fade_in=self.node('tween',.5,from_=0,to=255,key=self.key_opacity)
            fade_out=self.node('tween',.5,from_=255,to=0,key=self.key_opacity)
            opacity=self.node('sequence',1,children=[fade_in,fade_out])
            opacity=self.node('sequence',1,children=[opacity,self.node('remove',0)])
            self.root=self.node('spawn',1,children=[radius,opacity])
        self.start_node(self.root,self.wave)
        self.u.hook_add(UC_HOOK_CODE,self.hook)
    def map(self,a,n):
        for p in range(a&~4095,(a+n+4095)&~4095,4096):
            if p not in self.pages:self.u.mem_map(p,4096);self.pages.add(p)
    def put64(self,a,n):self.u.mem_write(a,struct.pack('<Q',n))
    def put32(self,a,n):self.u.mem_write(a,struct.pack('<I',n&0xffffffff))
    def node(self,kind,duration,children=None,from_=0,to=0,key=0):
        a=self.next;self.next+=0x600;v=a+0x100
        self.nodes[a]=(kind,children or [])
        self.put64(a,v);self.put64(a+0x40,self.wave)
        self.put64(v+self.update_slot,self.remove if kind=='remove' else self.functions[kind][0])
        for slot,fun in [(self.start_slot,self.start),(self.stop_slot,self.stop),(self.done_slot,self.done)]:self.put64(v+slot,fun)
        if kind=='tween':
            self.put64(a+0x60,key)
            if not self.fused:
                self.u.mem_write(a+0x60,b'radius\0' if key==self.key_radius else b'opacity\0')
                self.put64(a+0x78,15) # inline gd::string
            at=a+(0x6c if self.fused else 0x84)
            for i,f in enumerate([from_,to,f32(to-from_)]):self.put32(at+4*i,bits(f))
        elif kind=='sequence':
            self.put64(a+0x60,children[0]);self.put64(a+0x68,children[1])
            self.put32(a+0x70,bits(.5 if self.nodes[children[0]][0]=='tween' else 1))
            self.put32(a+0x74,-1)
        elif kind=='spawn':
            self.put64(a+0x60,children[0]);self.put64(a+0x68,children[1])
        elif kind=='ease':self.put64(a+0x60,children[0]);self.put32(a+0x68,bits(2))
        return a
    def start_node(self,a,target):
        kind,children=self.nodes[a];self.put64(a+0x40,target)
        if kind=='sequence':self.put32(a+0x74,-1)
        if kind in ['spawn','ease']:
            for child in children:self.start_node(child,target)
    def hook(self,u,pc,size,_):
        obj=u.reg_read(UC_ARM64_REG_X0 if self.fused else UC_X86_REG_RCX)
        if pc==self.start:
            target=u.reg_read(UC_ARM64_REG_X1 if self.fused else UC_X86_REG_RDX)
            self.start_node(obj,target)
        elif pc==self.done:
            u.reg_write(UC_ARM64_REG_X0 if self.fused else UC_X86_REG_RAX,int(self.nodes[obj][0]=='remove'))
        elif pc==self.callback:
            reg=UC_ARM64_REG_S0 if self.fused else UC_X86_REG_XMM1
            key=u.reg_read(UC_ARM64_REG_X1 if self.fused else UC_X86_REG_R8)
            self.events.append((0 if bytes(u.mem_read(key,6))==b'radius' else 1,u.reg_read(reg)&0xffffffff))
        elif pc==self.remove:self.events.append((2,0))
        elif pc==self.power:
            reg0=UC_ARM64_REG_S0 if self.fused else UC_X86_REG_XMM0
            reg1=UC_ARM64_REG_S1 if self.fused else UC_X86_REG_XMM1
            value=libm.powf(float_of(u.reg_read(reg0)),float_of(u.reg_read(reg1)))
            u.reg_write(reg0,bits(value))
    def run(self,times):
        for time in times:
            if self.events and self.events[-1][0]==2:break
            if self.fused:
                self.u.reg_write(UC_ARM64_REG_SP,0x3008000);self.u.reg_write(UC_ARM64_REG_X30,self.end)
                self.u.reg_write(UC_ARM64_REG_X0,self.root);self.u.reg_write(UC_ARM64_REG_S0,bits(time))
            else:
                self.u.reg_write(UC_X86_REG_RSP,0x3008000);self.put64(0x3008000,self.end)
                self.u.reg_write(UC_X86_REG_RCX,self.root);self.u.reg_write(UC_X86_REG_XMM1,bits(time))
            self.u.emu_start(self.functions[self.nodes[self.root][0]][0],self.end,count=20000)
        return self.events

rng=random.Random(2081);cases=[];payload=[]
for fused in [0,1]:
 for fade in [0,1]:
  for ease in [0,1]:
   for n in range(20):
    from_,to=(5.,30.) if n<3 else (rng.uniform(-100,200),rng.uniform(-100,200))
    times=([0,.25,.5,.75,1] if n==0 else [0,1] if n==1 else [0,.6,.2,.5,1] if n==2 else [0,*sorted(rng.random() for _ in range(10)),1])
    q=struct.pack('<IIIffI',fused,fade,ease,from_,to,len(times));times=[f32(t) for t in times]
    cases.append((fused,fade,ease,f32(from_),f32(to),times))
    payload.append(q+struct.pack('<'+'f'*len(times),*times))
with tempfile.TemporaryDirectory(prefix='echo-wave-native-') as d:
    exe=Path(d)/'test'
    subprocess.run(['c++','-std=c++23','-O2','-ffp-contract=off','-Isrc','tests/wave_timeline.cpp','-o',str(exe)],cwd=root,check=True)
    cached=subprocess.run([str(exe),'--stream'],input=b''.join(payload),stdout=subprocess.PIPE,check=True).stdout
cursor=0
for fused,fade,ease,from_,to,times in cases:
    count=struct.unpack_from('<I',cached,cursor)[0];cursor+=4
    expected=[struct.unpack_from('<II',cached,cursor+i*8) for i in range(count)];cursor+=count*8
    actual=Native(fused,fade,ease,from_,to).run(times)
    assert actual==expected,(fused,fade,ease,from_,to,times,actual,expected)
assert cursor==len(cached)
print('6 native fingerprints and 160 native action-graph callback traces match bit-for-bit (both platforms, fades, easing, skips and rewinds).')
