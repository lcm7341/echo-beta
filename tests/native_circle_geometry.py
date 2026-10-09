"""Compare the production cache with vertex loops from both real game binaries.
Usage: python tests/native_circle_geometry.py arm64-lib cocos-dll
Requires unicorn and pyelftools. CRT cosf/sinf calls use the same modeled libm
for native and cached paths; production Windows uses the game's own CRT imports.
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
    elf=ELFFile(f)
    load_segments=[(s['p_vaddr'],s['p_offset'],s['p_filesz']) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
def arm_bytes(a,n):
    v,o,_=next(s for s in load_segments if s[0]<=a and a+n<=s[0]+s[2]);return arm[o+a-v:o+a-v+n]
win=Path(sys.argv[2]).read_bytes();h=struct.unpack_from('<I',win,0x3c)[0]
sec=h+24+struct.unpack_from('<H',win,h+20)[0]
sections=[struct.unpack_from('<IIII',win,sec+i*40+8) for i in range(struct.unpack_from('<H',win,h+6)[0])]
def win_bytes(a,n):
    _,v,_,o=next(s for s in sections if s[1]<=a and a+n<=s[1]+s[2]);return win[o+a-v:o+a-v+n]
def fnv(b):
    x=14695981039346656037
    for c in b:x=((x^c)*1099511628211)&((1<<64)-1)
    return x
header=(root/'src/Rendering/circle_signatures.hpp').read_text()
for name,reader in [('armCircle',arm_bytes),('armFilled',arm_bytes),('winCircle',win_bytes),('winFilled',win_bytes)]:
    offset,length,value=[int(x,16) for x in re.search(name+r'\{(0x\w+), (0x\w+), (0x\w+)\}',header).groups()]
    assert fnv(reader(offset,length))==value,name+' signature differs'
    changed=bytearray(reader(offset,length));changed[20]^=1
    assert fnv(changed)!=value
libm=ctypes.CDLL('libm.so.6')
for f in [libm.cosf,libm.sinf]:f.argtypes=[ctypes.c_float];f.restype=ctypes.c_float
def bits(f):return struct.unpack('<I',struct.pack('<f',f))[0]
def float_of(b):return struct.unpack('<f',struct.pack('<I',b&0xffffffff))[0]
def step(segments):return float_of(bits(float_of(0x40c90fdb)/segments))
data,stack=0x2000000,0x2100000

def native_arm(filled,x,y,radius,segments):
    start,end=(0xad878c,0xad87cc) if filled else (0xad8448,0xad8494)
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for page in {start&~4095,0x4d8000,data,stack}:u.mem_map(page,4096)
    u.mem_write(start,arm_bytes(start,end-start))
    for a in [0x4d8860,0x4d8c20]:u.mem_write(a,struct.pack('<I',0xd65f03c0))
    def trig(uc,pc,size,_):
        if pc in [0x4d8860,0x4d8c20]:
            f=libm.cosf if pc==0x4d8860 else libm.sinf
            uc.reg_write(UC_ARM64_REG_S0,bits(f(float_of(uc.reg_read(UC_ARM64_REG_S0)))))
    u.hook_add(UC_HOOK_CODE,trig)
    u.reg_write(UC_ARM64_REG_X19,0)
    if filled:
        u.reg_write(UC_ARM64_REG_X21,data);u.reg_write(UC_ARM64_REG_X20,1);u.reg_write(UC_ARM64_REG_X22,segments)
        floats=[(UC_ARM64_REG_S11,step(segments)),(UC_ARM64_REG_S14,0),(UC_ARM64_REG_S10,radius),(UC_ARM64_REG_S13,x),(UC_ARM64_REG_S12,y)]
    else:
        u.reg_write(UC_ARM64_REG_X25,data);u.reg_write(UC_ARM64_REG_X24,1);u.reg_write(UC_ARM64_REG_X20,segments);u.reg_write(UC_ARM64_REG_X26,0)
        floats=[(UC_ARM64_REG_S14,step(segments)),(UC_ARM64_REG_S10,radius),(UC_ARM64_REG_S11,x),(UC_ARM64_REG_S15,y),(UC_ARM64_REG_S12,1),(UC_ARM64_REG_S13,1)]
    for reg,v in floats:u.reg_write(reg,bits(v))
    u.emu_start(start,end,count=10000)
    return bytes(u.mem_read(data,(segments+1)*8))

def native_win(filled,x,y,radius,segments):
    base=0x180000000
    a,b=(0x112c3,0x11311) if filled else (0x10b10,0x10b68)
    start,end=base+a,base+b;cosine,sine=base+0xe0564,base+0xe056a
    u=Uc(UC_ARCH_X86,UC_MODE_64)
    for page in {start&~4095,cosine&~4095,data,stack}:u.mem_map(page,4096)
    u.mem_write(start,win_bytes(a,b-a));u.mem_write(cosine,b'\xc3');u.mem_write(sine,b'\xc3')
    u.reg_write(UC_X86_REG_RSP,stack+0x800)
    u.reg_write(UC_X86_REG_R14,segments)
    u.reg_write(UC_X86_REG_RSI,data if filled else 0)
    u.reg_write(UC_X86_REG_RDI,0 if filled else data)
    for reg,v in [(UC_X86_REG_XMM7,step(segments)),(UC_X86_REG_XMM8,radius),(UC_X86_REG_XMM11,0),
                  (UC_X86_REG_XMM9,x),(UC_X86_REG_XMM10,y),(UC_X86_REG_XMM12,1),(UC_X86_REG_XMM13,1)]:
        u.reg_write(reg,bits(v))
    def trig(uc,pc,size,_):
        if pc in [cosine,sine]:
            f=libm.cosf if pc==cosine else libm.sinf
            uc.reg_write(UC_X86_REG_XMM0,bits(f(float_of(uc.reg_read(UC_X86_REG_XMM0)))))
    u.hook_add(UC_HOOK_CODE,trig)
    u.emu_start(start,end,count=10000)
    return bytes(u.mem_read(data,(segments+1)*8))

rng=random.Random(2081);requests=[]
for fused in [0,1]:
    for filled in [0,1]:
        for segments in [10,15,20,30,50]:
            for n in range(12):
                x,y,r=(rng.uniform(-100000,100000),rng.uniform(-100000,100000),rng.uniform(-500,500))
                if n<4:x,y,r=((-0.0,0.0,-0.0) if n%2 else (0.0,-0.0,0.0))
                requests.append(struct.pack('<IIfffI',fused,filled,x,y,r,segments))
with tempfile.TemporaryDirectory(prefix='echo-native-circles-') as d:
    binary=Path(d)/'test'
    subprocess.run(['c++','-std=c++23','-O2','-ffp-contract=off','-Isrc','tests/circle_geometry.cpp','-o',str(binary)],cwd=root,check=True)
    cached=subprocess.run([str(binary),'--stream'],input=b''.join(requests),stdout=subprocess.PIPE,check=True).stdout
cursor=0
for request in requests:
    fused,filled,x,y,r,segments=struct.unpack('<IIfffI',request)
    original=(native_arm if fused else native_win)(filled,x,y,r,segments)
    assert original==cached[cursor:cursor+len(original)],(fused,filled,x,y,r,segments)
    cursor+=len(original)
assert cursor==len(cached)
print('4 native renderer signatures and 240 native/cached vertex streams verified bit-for-bit.')
