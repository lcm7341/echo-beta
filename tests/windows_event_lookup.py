"""Emulate the actual GD Windows event-state lookup and production patch.
Usage: python windows_event_lookup.py GeometryDash.exe (requires unicorn).
Existing keys execute the native tree lookup. Missing-key allocation/insertion
are modeled at the native allocator and tree-insert boundaries.
"""
from pathlib import Path
import re, struct, sys, random
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *
source=(Path(__file__).resolve().parents[1]/'src/Hooks/game_patches.hpp').read_text()
p=Path(sys.argv[1]).read_bytes(); h=struct.unpack_from('<I',p,60)[0]
s=h+24+struct.unpack_from('<H',p,h+20)[0]
sections=[struct.unpack_from('<IIII',p,s+i*40+8) for i in range(struct.unpack_from('<H',p,h+6)[0])]
def native(a,n):
    _,v,z,o=next(t for t in sections if t[1]<=a and a+n<=t[1]+t[2])
    return p[o+a-v:o+a-v+n]
def value(name):return int(re.search(name+r' = (0x[0-9a-f]+);',source)[1],16)
def array(name):return bytes(int(x,16) for x in re.findall(r'0x[0-9a-f]+',re.search(name+r' = \{(.*?)\};',source,re.S)[1]))
start=value('eventLookupSignatureAddress'); patch=value('eventLookupAddress')
assert native(start,len(array('eventLookupOriginal')))==array('eventLookupOriginal')
helper=value('eventMapAddress'); size=value('eventMapSize')
hash=14695981039346656037
for byte in native(helper,size):hash=((hash^byte)*1099511628211)&((1<<64)-1)
assert hash==value('eventMapHash')
base=0x140000000; stack=0x1000000; layer=0x1100000; heap=0x1200000
rng=random.Random(2081)
def run(patched, keys, key, tick, prior):
    u=Uc(UC_ARCH_X86,UC_MODE_64)
    for a,n in [(base+0x232000,4096),(base+0x1d0000,4096),(stack,0x4000),(layer,0x1000),(heap,0x10000)]:u.mem_map(a,n)
    u.mem_write(base+start,native(start,0x47))
    u.mem_write(base+helper,native(helper,size))
    if patched:u.mem_write(base+patch,array('eventLookupReplacement'))
    def q(a,v):u.mem_write(a,struct.pack('<Q',v))
    def readq(a):return struct.unpack('<Q',u.mem_read(a,8))[0]
    head=heap; allocation=heap+0xf000; nodes={k:heap+0x100+i*0x40 for i,k in enumerate(keys)}
    u.mem_write(head+0x19,b'\1')
    def tree(items,parent):
        if not items:return head
        mid=len(items)//2;k=items[mid];a=nodes[k]
        q(a,tree(items[:mid],a));q(a+8,parent);q(a+16,tree(items[mid+1:],a))
        u.mem_write(a+0x1c,struct.pack('<iiI',*k,prior if k==key else 0xffffffff));return a
    root=tree(keys,head);q(head+8,root);q(head,nodes[keys[0]] if keys else head);q(head+16,nodes[keys[-1]] if keys else head)
    q(layer+0x598,head);q(layer+0x5a0,len(keys))
    u.mem_write(layer+0x3e0,struct.pack('<I',tick))
    rsp=stack+0x2000
    u.reg_write(UC_X86_REG_RSP,rsp);u.reg_write(UC_X86_REG_RSI,layer);u.reg_write(UC_X86_REG_RCX,layer)
    u.mem_write(rsp+0x80,struct.pack('<ii',*key));u.reg_write(UC_X86_REG_RAX,rsp+0x78)
    for target in [0x4d0770,0x636d0]:
        u.mem_map(base+(target&~4095),4096);u.mem_write(base+target,b'\xc3')
    instructions=0;lookups=0;allocations=0
    def trace(uc,pc,size,_):
        nonlocal instructions,lookups,allocations
        instructions+=1
        if pc==base+helper:lookups+=1
        if pc==base+0x4d0770:
            allocations+=1;assert allocations==1;uc.reg_write(UC_X86_REG_RAX,allocation)
        if pc==base+0x636d0:
            # Model std::map insertion at the library boundary; native lookup
            # and node initialization have already executed above.
            node=uc.reg_read(UC_X86_REG_R8);where=uc.reg_read(UC_X86_REG_RDX)
            parent=readq(where);left=readq(where+8)!=0
            q(node+8,parent)
            if parent==head:q(head+8,node);q(head,node);q(head+16,node)
            else:
                q(parent+(0 if left else 16),node)
                if key<keys[0]:q(head,node)
                if key>keys[-1]:q(head+16,node)
            q(layer+0x5a0,len(keys)+1);uc.reg_write(UC_X86_REG_RAX,node)
        if pc in [base+0x232066,base+0x2320d7]:uc.emu_stop()
    u.hook_add(UC_HOOK_CODE,trace)
    u.emu_start(base+start,base+0x2320d7,count=10000)
    return (bytes(u.mem_read(heap,0x10000)),bytes(u.mem_read(layer,4096)),
            u.reg_read(UC_X86_REG_RIP),u.reg_read(UC_X86_REG_RAX)),(instructions,lookups,allocations)
saved=[];cases=0
for count in [0,1,2,7,31,127]:
    keys=sorted(set((rng.randrange(-30,31),rng.randrange(-10,11)) for _ in range(count)))
    queries=keys+[(10,0),(11,0),(-100,0),(100,0)]
    for key in queries:
        for tick,prior in [(0,0),(1,0),(0xffffffff,0),(23,23)]:
            a,stats=run(False,keys,key,tick,prior);b,fast=run(True,keys,key,tick,prior)
            assert a==b,(len(keys),key,tick,prior)
            updated=(key not in keys and tick!=0) or (key in keys and tick!=prior)
            assert fast[1]==1 and stats[1]==(2 if updated else 1)
            assert stats[2]==fast[2]==int(key not in keys)
            if updated:saved.append(stats[0]-fast[0])
            cases+=1
print(f'{cases} native lookup comparisons passed; {min(saved)}–{max(saved)} instructions removed per updated event in tested trees.')
