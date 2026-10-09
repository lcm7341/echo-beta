"""Compare production dash motion with actual Windows Cocos update instructions.
Usage: python tests/native_dash_timeline.py libcocos2d.dll GeometryDash.exe
Executes Sequence/ScaleTo/MoveBy/FadeTo; models target methods, RTTI and
start/stop/callback boundaries. Includes stackable external position changes.
"""
import json, random, re, struct, subprocess, sys, tempfile
from pathlib import Path
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *
root=Path(__file__).resolve().parents[1]
def pe(path):
    raw=Path(path).read_bytes();h=struct.unpack_from('<I',raw,60)[0]
    at=h+24+struct.unpack_from('<H',raw,h+20)[0]
    sections=[struct.unpack_from('<IIII',raw,at+40*i+8) for i in range(struct.unpack_from('<H',raw,h+6)[0])]
    def read(rva,n):
        _,va,size,offset=next(s for s in sections if s[1]<=rva and rva+n<=s[1]+s[2])
        return raw[offset+rva-va:offset+rva-va+n]
    return raw,sections,read
image,sections,read=pe(sys.argv[1]);_,_,readexe=pe(sys.argv[2])
def fnv(b):
    v=14695981039346656037
    for c in b:v=((v^c)*1099511628211)&((1<<64)-1)
    return v
source=(root/'src/Rendering/dash_animation.cpp').read_text()
for name in ['stopBody','actions']:
    a,n,h=[int(x,16) for x in re.search(name+r'\{(0x\w+), (0x\w+), (0x\w+)\}',source).groups()]
    assert fnv(readexe(a,n))==h,name
def f32(x):return struct.unpack('<f',struct.pack('<f',x))[0]
def bits(x):return struct.unpack('<I',struct.pack('<f',x))[0]
def flt(x):return struct.unpack('<f',struct.pack('<I',x&0xffffffff))[0]
class Native:
    base=0x180000000; target=0x2000000; sequence=0x2001000; scale=0x2002000; move=0x2003000; fade=0x2004000; callback=0x2005000
    def __init__(self,start,alpha):
        self.u=Uc(UC_ARCH_X86,UC_MODE_64);u=self.u
        u.mem_map(self.base,0x200000)
        for _,va,size,off in sections:u.mem_write(self.base+va,image[off:off+size])
        u.mem_map(0x2000000,0x10000);u.mem_map(0x3000000,0x10000);u.mem_map(0x1000000,0x10000)
        self.stub={name:0x1000100+i*0x100 for i,name in enumerate(['start','stop','done','remove','position','set_position','set_x','set_y','set_opacity','end'])}
        for a in self.stub.values():u.mem_write(a,b'\xc3')
        self.state=list(start[:4]);self.alpha=alpha;self.removed=False;self.ends=start[4:]
        v=0x2000600;self.q(self.target,v)
        for slot,name in [(0xc8,'position'),(0xb8,'set_position'),(0x78,'set_x'),(0x88,'set_y')]:self.q(v+slot,self.stub[name])
        self.q(self.target+320,0x2000800);self.q(0x2000800+0x28,self.stub['set_opacity'])
        for a,update in [(self.sequence,self.base+0x37430),(self.scale,self.base+0x373e0),(self.move,self.base+0x370d0),(self.fade,self.base+0x36f20),(self.callback,self.stub['remove'])]:
            v=a+0x400;self.q(a,v);self.q(a+0x40,self.target);self.q(v+0x20,update)
            for slot,name in [(0x50,'start'),(0x58,'stop'),(0x48,'done')]:self.q(v+slot,self.stub[name])
        self.q(self.sequence+0x60,self.scale);self.q(self.sequence+0x68,self.callback);self.f(self.sequence+0x70,1);self.i(self.sequence+0x74,-1)
        for at,val in [(0x60,-20),(0x64,0),(0x68,start[0]),(0x6c,start[1]),(0x70,start[0]),(0x74,start[1])]:self.f(self.move+at,val)
        u.mem_write(self.fade+0x60,bytes([0,alpha]));u.hook_add(UC_HOOK_CODE,self.hook)
    def q(self,a,v):self.u.mem_write(a,struct.pack('<Q',v))
    def i(self,a,v):self.u.mem_write(a,struct.pack('<I',v&0xffffffff))
    def f(self,a,v):self.u.mem_write(a,struct.pack('<f',v))
    def hook(self,u,pc,size,_):
        obj=u.reg_read(UC_X86_REG_RCX);stub=self.stub
        if pc==stub['start'] and obj==self.scale:
            for at,v in [(0x68,self.state[2]),(0x6c,self.state[3]),(0x78,f32(self.ends[0]-self.state[2])),(0x7c,f32(self.ends[1]-self.state[3]))]:self.f(obj+at,v)
        elif pc==stub['done']:u.reg_write(UC_X86_REG_RAX,int(obj==self.callback))
        elif pc==stub['remove']:self.removed=True
        elif pc==stub['position']:
            self.f(self.target+0x900,self.state[0]);self.f(self.target+0x904,self.state[1]);u.reg_write(UC_X86_REG_RAX,self.target+0x900)
        elif pc==stub['set_position']:self.state[:2]=struct.unpack('<2f',u.mem_read(u.reg_read(UC_X86_REG_RDX),8))
        elif pc==stub['set_x']:self.state[2]=flt(u.reg_read(UC_X86_REG_XMM1))
        elif pc==stub['set_y']:self.state[3]=flt(u.reg_read(UC_X86_REG_XMM1))
        elif pc==stub['set_opacity']:self.alpha=u.reg_read(UC_X86_REG_RDX)&255
    def call(self,address,time):
        u=self.u;u.reg_write(UC_X86_REG_RSP,0x3008000);self.q(0x3008000,self.stub['end']);u.reg_write(UC_X86_REG_RCX,address);u.reg_write(UC_X86_REG_XMM1,bits(time))
        u.emu_start(struct.unpack('<Q',u.mem_read(struct.unpack('<Q',u.mem_read(address,8))[0]+0x20,8))[0],self.stub['end'],count=20000)
    def run(self,steps):
        out=[]
        # RTTI is an external boundary; make its stub return normally.
        self.u.mem_write(self.base+0xe04f2,b'\x48\xb8'+struct.pack('<Q',self.target+320)+b'\xc3')
        for t,dx,dy in steps:
            self.state[0]=f32(self.state[0]+dx);self.state[1]=f32(self.state[1]+dy)
            if not self.removed:
                self.call(self.sequence,t)
                if not self.removed:self.call(self.move,t);self.call(self.fade,t)
            out.append(struct.pack('<4fII',*self.state,self.alpha,self.removed))
        return b''.join(out)
rng=random.Random(2081);cases=[];payload=[]
for i in range(600):
    start=[f32(rng.uniform(-1000,1000)) for _ in range(2)]+[f32(rng.uniform(-4,4)) for _ in range(2)]
    start+=[f32(start[2]*f32(.2)),f32(start[3]*f32(.2))];alpha=rng.randrange(256)
    times=([0,1] if i%3==0 else [0,*sorted(f32(rng.random()) for _ in range(12)),1])
    steps=[(t, f32(rng.uniform(-1,1)) if rng.random()<.25 else 0.,f32(rng.uniform(-1,1)) if rng.random()<.25 else 0.) for t in times]
    cases.append((start,alpha,steps));payload.append(struct.pack('<6fII',*start,alpha,len(steps))+b''.join(struct.pack('<3f',*s) for s in steps))
with tempfile.TemporaryDirectory(prefix='echo-dash-native-') as d:
    binary=Path(d)/'test';subprocess.run(['c++','-std=c++23','-O2','-ffp-contract=off','-Isrc','tests/dash_timeline.cpp','-o',str(binary)],cwd=root,check=True)
    actual=subprocess.run([str(binary)],input=b''.join(payload),stdout=subprocess.PIPE,check=True).stdout
cursor=0
for start,alpha,steps in cases:
    expected=Native(start,alpha).run(steps);n=len(expected)
    assert actual[cursor:cursor+n]==expected,(start,alpha,steps,actual[cursor:cursor+n].hex(),expected.hex())
    cursor+=n
assert cursor==len(actual)
print('600 native Windows dash-effect traces match bit-for-bit, including callback skips and external position changes.')
