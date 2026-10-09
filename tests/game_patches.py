"""Validate production patch bytes against installed binaries and emulate both paths.
Usage: python tests/game_patches.py libcocos2dcpp.so GeometryDash.exe
Requires unicorn and pyelftools; does not change the game files.
"""
from pathlib import Path
import random
import re
import struct
import sys
from elftools.elf.elffile import ELFFile
from unicorn import *
from unicorn.arm64_const import *
from unicorn.x86_const import *

source = (Path(__file__).resolve().parents[1] / 'src/Hooks/game_patches.hpp').read_text()
def array(name):
    m = re.search(r'std::array<uint(\d+)_t, \d+> '+name+r' = \{(.*?)\};', source, re.S)
    values = [int(x, 0) for x in re.findall(r'0x[0-9a-f]+|\b0\b', m[2])]
    return struct.pack('<'+{8:'B',32:'I'}[int(m[1])]*len(values), *values)
def address(name):
    return int(re.search(name+r' = (0x[0-9a-f]+);', source)[1], 16)
with open(sys.argv[1], 'rb') as f:
    elf = ELFFile(f)
    segments = [(s['p_vaddr'], s['p_offset'], s['p_filesz']) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
    arm = f.read() if False else Path(sys.argv[1]).read_bytes()
def arm_bytes(addr, length):
    va, offset, _ = next(s for s in segments if s[0] <= addr and addr+length <= s[0]+s[2])
    return arm[offset+addr-va:offset+addr-va+length]
pe = Path(sys.argv[2]).read_bytes()
header = struct.unpack_from('<I', pe, 0x3c)[0]
count = struct.unpack_from('<H', pe, header+6)[0]
optional_size = struct.unpack_from('<H', pe, header+20)[0]
section = header+24+optional_size
sections = [struct.unpack_from('<IIII', pe, section+i*40+8) for i in range(count)]
def win_bytes(addr, length):
    _, va, size, offset = next(s for s in sections if s[1]<=addr and addr+length<=s[1]+s[2])
    return pe[offset+addr-va:offset+addr-va+length]
for name, reader in [('point',arm_bytes), ('rings',arm_bytes), ('queue',win_bytes), ('queueCave',win_bytes)]:
    expected = array(name+'Original')
    assert reader(address(name+'Address'), len(expected)) == expected, name+' signature differs'

rng = random.Random(2081)
stop, stack, data = 0x1000000, 0x1100000, 0x1200000
def arm_vm(start, code):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    page = start & ~4095
    u.mem_map(page, 0x2000)
    u.mem_write(start, code)
    u.mem_map(stop, 0x1000)
    u.mem_map(stack, 0x2000)
    u.mem_map(data, 0x4000)
    u.reg_write(UC_ARM64_REG_SP, stack+0x1000)
    u.reg_write(UC_ARM64_REG_X30, stop)
    return u
point = address('pointAddress')
def copy_point(patched, offset, bits):
    u = arm_vm(point-12, arm_bytes(point-12, 12)+ (array('pointReplacement') if patched else array('pointOriginal')))
    for reg in range(UC_ARM64_REG_X0, UC_ARM64_REG_X28+1):
        u.reg_write(reg, 0x12345000+reg)
    u.reg_write(UC_ARM64_REG_X29, stack+0x800)
    u.reg_write(UC_ARM64_REG_X0, data+offset)
    u.reg_write(UC_ARM64_REG_X1, data)
    u.mem_write(data, bits+bytes(24))
    u.emu_start(point, stop, count=100)
    regs = [u.reg_read(r) for r in [UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X19,
                                    UC_ARM64_REG_X29, UC_ARM64_REG_X30, UC_ARM64_REG_SP,
                                    UC_ARM64_REG_S0, UC_ARM64_REG_S1]]
    return bytes(u.mem_read(data,32)), regs
patterns = [bytes(8), struct.pack('<II',0x80000000,0x7fc12345),struct.pack('<II',0x7f800000,0xff800000)]
patterns += [rng.randbytes(8) for _ in range(300)]
for bits in patterns:
    for offset in [0, 4, 8]:
        assert copy_point(False,offset,bits)==copy_point(True,offset,bits)

rings = address('ringsAddress')
def clear_rings(patched, nodes, buckets):
    code = bytearray(array('ringsOriginal'))
    if patched: code[4:8] = array('ringsReplacement')
    u = arm_vm(rings, bytes(code))
    player = data
    container = player+0xa18
    bucket = data+0x1000
    head = data+0x2000 if nodes else 0
    u.reg_write(UC_ARM64_REG_X20, player)
    u.reg_write(UC_ARM64_REG_X21, container)
    u.mem_write(container, struct.pack('<QQQQ', bucket,buckets,head,nodes))
    u.mem_write(bucket, struct.pack('<'+'Q'*buckets, *([head]*buckets)))
    for i in range(nodes):
        u.mem_write(head+i*16, struct.pack('<Q',head+(i+1)*16 if i+1<nodes else 0))
    freed=[]; clears=[]
    # External calls are modeled with their memory effects and AAPCS return.
    for target in [0xccbef0,0x4d8dd0]:
        u.mem_map(target & ~4095,4096)
        u.mem_write(target, struct.pack('<I',0xd65f03c0))
    def call(uc, pc, size, _):
        if pc==0xccbef0: freed.append(uc.reg_read(UC_ARM64_REG_X0))
        if pc==0x4d8dd0:
            dest=uc.reg_read(UC_ARM64_REG_X0); n=uc.reg_read(UC_ARM64_REG_X2)
            clears.append(n); uc.mem_write(dest,bytes(n))
    u.hook_add(UC_HOOK_CODE,call)
    u.emu_start(rings,rings+len(code),count=1000)
    return bytes(u.mem_read(container,32)),bytes(u.mem_read(bucket,buckets*8)),freed,clears
for nodes in [0,1,2,9]:
    for buckets in [1,13,127]:
        old=clear_rings(False,nodes,buckets); new=clear_rings(True,nodes,buckets)
        assert old[:3]==new[:3]
        assert new[3]==([] if nodes==0 else old[3])

site = address('queueAddress'); cave=address('queueCaveAddress'); move=0x4d1836
# Execute actual call site and cave; model overlap-safe memmove at the original thunk.
def move_queue(patched, size):
    u=Uc(UC_ARCH_X86,UC_MODE_64)
    for page in {site & ~4095, cave & ~4095, move & ~4095,stop,stack,data}:
        u.mem_map(page,0x1000 if page!=data else 0x5000)
    u.mem_write(site,array('queueOriginal'))
    if patched:
        u.mem_write(site,array('queueReplacement')); u.mem_write(cave,array('queueCaveReplacement'))
    u.mem_write(move,b'\xc3')
    u.reg_write(UC_X86_REG_RSP,stack+0x800)
    u.reg_write(UC_X86_REG_RBX,data)
    u.reg_write(UC_X86_REG_RSI,0x13579)
    dest=data+0x100; src=dest+24
    content=bytes((i*17)%256 for i in range(size+24))
    u.mem_write(dest,content)
    u.mem_write(data+0x3308,struct.pack('<Q',src+size))
    u.reg_write(UC_X86_REG_RCX,dest);u.reg_write(UC_X86_REG_RDX,src);u.reg_write(UC_X86_REG_R8,size)
    calls=[]
    def call(uc,pc,length,_):
        if pc==move:
            n=uc.reg_read(UC_X86_REG_R8);calls.append(n)
            if n: uc.mem_write(uc.reg_read(UC_X86_REG_RCX),bytes(uc.mem_read(uc.reg_read(UC_X86_REG_RDX),n)))
            uc.reg_write(UC_X86_REG_RAX,uc.reg_read(UC_X86_REG_RCX))
    u.hook_add(UC_HOOK_CODE,call)
    u.emu_start(site,site+len(array('queueOriginal')),count=100)
    result=(bytes(u.mem_read(dest,size+24)),bytes(u.mem_read(data+0x3308,8)),
            [u.reg_read(r) for r in [UC_X86_REG_RAX,UC_X86_REG_RBX,UC_X86_REG_RSI,UC_X86_REG_RSP]])
    return result,calls
for size in [0,24,48,192,960]:
    old=move_queue(False,size);new=move_queue(True,size)
    assert old[0]==new[0]
    assert new[1]==([] if size==0 else old[1])
print('Native patch signatures verified; 909 point copies, 12 ring clears, and 5 queue moves preserve results.')
