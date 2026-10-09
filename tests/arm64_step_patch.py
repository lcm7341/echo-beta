"""Execute the actual ARM64 patch against an extracted game library.

Usage: python tests/arm64_step_patch.py /path/to/libcocos2dcpp.so
Requires unicorn and pyelftools. No phone or game process is modified.
"""
import math
from pathlib import Path
import re
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X19, UC_ARM64_REG_X20,
    UC_ARM64_REG_X29, UC_ARM64_REG_D0, UC_ARM64_REG_D8,
    UC_ARM64_REG_D13, UC_ARM64_REG_S1,
)

source = (Path(__file__).resolve().parents[1] / "src/Hooks/hooks.cpp").read_text()
def words(name):
    body = re.search(rf"{name} = \{{(.*?)\}};", source, re.S).group(1)
    return struct.pack("<10I", *(int(n, 0) for n in re.findall(r"0x[0-9a-f]+|\b0\b", body)))

start, end = 0x89D9F8, 0x89DA20
with open(sys.argv[1], "rb") as library:
    elf = ELFFile(library)
    segment = next(s for s in elf.iter_segments()
                   if s["p_type"] == "PT_LOAD" and
                   s["p_vaddr"] <= start < s["p_vaddr"] + s["p_filesz"])
    library.seek(segment["p_offset"] + start - segment["p_vaddr"])
    assert library.read(end - start) == words("original"), "Game instructions differ"
    library.seek(segment["p_offset"] + end - segment["p_vaddr"])
    continuation = library.read(0x28)  # through all three delta divisions

count_address, stack, layer = 0x200000, 0x300000, 0x400000
patch = bytearray(words("words"))
struct.pack_into("<Q", patch, 32, count_address)
def double_bits(value):
    return struct.unpack("<Q", struct.pack("<d", value))[0]
def as_double(value):
    return struct.unpack("<d", struct.pack("<Q", value))[0]

cases = 0
for count in (0, 1, 2, 4, 16, 240, 100000):
    for warp in (0.5, 1.0, 2.0):
        for tps in (60, 240, 1000):
            cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
            for base, size in ((start & ~0xFFF, 0x1000), (count_address, 0x1000),
                               (stack, 0x1000), (layer, 0x5000)):
                cpu.mem_map(base, size)
            cpu.mem_write(start, bytes(patch) + continuation)
            cpu.mem_write(count_address, struct.pack("<I", count))
            cpu.reg_write(UC_ARM64_REG_X29, stack)
            cpu.reg_write(UC_ARM64_REG_X19, layer)
            cpu.reg_write(UC_ARM64_REG_X20, layer + 0x3000)
            effective = max(count, 1)
            dt = effective * warp / tps
            cpu.mem_write(stack + 0xE8, struct.pack("<d", dt))
            cpu.reg_write(UC_ARM64_REG_D8, double_bits(dt / warp))
            cpu.reg_write(UC_ARM64_REG_D13, double_bits(dt * 60))
            warp_bits = struct.unpack("<I", struct.pack("<f", warp))[0]
            cpu.reg_write(UC_ARM64_REG_S1, warp_bits)
            cpu.emu_start(start, end, count=30)
            assert struct.unpack("<I", cpu.mem_read(stack + 0xE0, 4))[0] == effective
            assert cpu.reg_read(UC_ARM64_REG_S1) == warp_bits
            assert as_double(cpu.reg_read(UC_ARM64_REG_D0)) == effective
            cpu.emu_start(end, 0x89DA48, count=30)
            assert struct.unpack("<I", cpu.mem_read(stack + 0xE4, 4))[0] == warp_bits
            for offset, expected in ((0xF8, warp / tps), (0xC0, warp * 60 / tps),
                                     (0xD8, 1 / tps)):
                actual = struct.unpack("<d", cpu.mem_read(stack + offset, 8))[0]
                assert math.isclose(actual, expected, rel_tol=1e-12), (count, warp, tps, offset)
            cases += 1
print(f"ARM64 instruction signature and {cases} count/delta cases passed")
