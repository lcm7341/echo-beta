"""Emulate the stopDashing end-effect gate against the installed Windows game.
Usage: python tests/dash_end_gate.py GeometryDash.exe
Requires unicorn; does not change the game files.
"""
from pathlib import Path
import random
import re
import struct
import sys
from unicorn import *
from unicorn.x86_const import *

source = (Path(__file__).resolve().parents[1] / 'src/Rendering/dash_animation.cpp').read_text()
def array(name):
    m = re.search(r'std::array<std::uint8_t, \d+> '+name+r'\{(.*?)\};', source, re.S)
    return bytes(int(x, 0) for x in re.findall(r'0x[0-9a-f]+', re.sub(r'//[^\n]*', '', m[1])))
site = int(re.search(r'gateSite = (0x[0-9a-f]+);', source)[1], 16)
m = re.search(r'gateMode = (0x[0-9a-f]+), gateRecord = (0x[0-9a-f]+);', source)
mode_offset, record_offset = int(m[1], 16), int(m[2], 16)
original, stub = array('gateOriginal'), array('stub')

pe = Path(sys.argv[1]).read_bytes()
header = struct.unpack_from('<I', pe, 0x3c)[0]
count = struct.unpack_from('<H', pe, header+6)[0]
optional_size = struct.unpack_from('<H', pe, header+20)[0]
sections = [struct.unpack_from('<IIII', pe, header+24+optional_size+i*40+8) for i in range(count)]
def win_bytes(addr, length):
    _, va, size, offset = next(s for s in sections if s[1] <= addr and addr+length <= s[1]+s[2])
    return pe[offset+addr-va:offset+addr-va+length]
def fingerprint(data):
    h = 14695981039346656037
    for b in data: h = ((h ^ b) * 1099511628211) & (2**64-1)
    return h
stop_body = re.search(r'stopBody\{(0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+)\}', source)
assert fingerprint(win_bytes(int(stop_body[1], 16), int(stop_body[2], 16))) == int(stop_body[3], 16), 'stopDashing differs'
assert win_bytes(site, len(original)) == original, 'gate site differs'
jne = win_bytes(site+8, 6)
assert jne[:2] == b'\x0f\x85'
enter, skip = site+14, site+14+struct.unpack('<i', jne[2:])[0]
assert (enter, skip) == (0x39678e, 0x396a4e)

base = 0x140000000
cave = (base + site - 0x30000000) & ~0xfff  # below the image, as allocateNear searches first
recorder = 0x6000000
stack, player = 0x7000000, 0x8000000
# Callee-saved under the Windows x64 ABI, plus the registers stopDashing keeps live (rsp, r14).
kept = [UC_X86_REG_RBX, UC_X86_REG_RBP, UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_R12, UC_X86_REG_R13,
        UC_X86_REG_R14, UC_X86_REG_R15, UC_X86_REG_RSP]
volatile = [UC_X86_REG_RAX, UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_R8, UC_X86_REG_R9, UC_X86_REG_R10, UC_X86_REG_R11]
def run(patched, hidden, mode, seed):
    u = Uc(UC_ARCH_X86, UC_MODE_64)
    code = base+site
    u.mem_map(code & ~0xfff, 0x2000)
    u.mem_write(code, win_bytes(site, 14))
    if patched:
        u.mem_map(cave, 0x1000)
        u.mem_write(cave, stub)
        u.mem_write(cave+mode_offset, bytes([mode]))
        u.mem_write(cave+record_offset, struct.pack('<Q', recorder))
        u.mem_write(code, b'\xe8'+struct.pack('<i', cave - (code+5))+b'\x0f\x1f\x00')
    u.mem_map(recorder, 0x1000)
    u.mem_write(recorder, b'\xc3')
    u.mem_map(stack, 0x2000)
    u.mem_map(player, 0x1000)
    rng = random.Random(seed)
    u.mem_write(player, rng.randbytes(0x1000))
    u.mem_write(player+0x7e1, bytes([hidden]))
    for r in kept[:-1] + volatile: u.reg_write(r, rng.getrandbits(64))
    u.reg_write(UC_X86_REG_R14, player)
    # stopDashing calls with a 16-byte aligned rsp at this point.
    u.reg_write(UC_X86_REG_RSP, stack+0x1000)
    u.mem_write(stack+0x1000, rng.randbytes(0x100))
    end, calls = [], []
    def watch(uc, pc, size, _):
        if pc == recorder:
            # Windows x64: rsp+8 is 16-byte aligned at entry, with 32 bytes of shadow space above it.
            calls.append((uc.reg_read(UC_X86_REG_RCX), uc.reg_read(UC_X86_REG_RSP) % 16,
                          stack+0x1000 - uc.reg_read(UC_X86_REG_RSP)))
        if pc in (base+enter, base+skip): end.append(pc-base); uc.emu_stop()
    u.hook_add(UC_HOOK_CODE, watch)
    u.emu_start(code, 0, count=80)
    return end[0], [u.reg_read(r) for r in kept], bytes(u.mem_read(stack+0x1000, 0x100)), \
        bytes(u.mem_read(player, 0x1000)), calls

cases = 0
for seed in range(40):
    for hidden in (0, 1, 0x80, 0xff):
        old = run(False, hidden, 0, seed)
        assert old[0] == (skip if hidden else enter) and not old[4]
        for mode in range(4):
            new = run(True, hidden, mode, seed)
            builds = not hidden and mode in (0, 2)
            records = not hidden and mode in (2, 3)
            assert new[0] == (enter if builds else skip), (hidden, mode)
            assert new[1:4] == old[1:4], (hidden, mode)
            assert len(new[4]) == (1 if records else 0), (hidden, mode)
            for rcx, misalign, depth in new[4]:
                assert rcx == player and misalign == 8 and depth >= 8 + 0x28, (rcx, misalign, depth)
            cases += 1
print(f'stopDashing gate verified; {cases} emulated paths across 4 modes keep callee-saved registers, stack and player bytes, '
      'and call the recorder only when the game would build the effect.')
