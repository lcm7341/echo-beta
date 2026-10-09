"""Run the installed OBB2D::calculateWithCenter against obb_math.hpp, bit for bit.

The game routine runs in a sandbox at a fixed address: its libcocos2d CCPoint operator imports
point at copies of the real operator code, its constants are copied, and sinf/cosf/the corner
ordering helper go to test stubs (both sides use the same trig, so this checks the arithmetic).
"""
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
game = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / '.local/share/Steam/steamapps/common/Geometry Dash/GeometryDash.exe'
cocos = game.with_name('libcocos2d.dll')
GD, COCOS = 0x140000000, 0x180000000

def reader(path):
    raw = path.read_bytes()
    pe = struct.unpack_from('<I', raw, 60)[0]
    table = pe + 24 + struct.unpack_from('<H', raw, pe + 20)[0]
    sections = [struct.unpack_from('<IIII', raw, table + 40 * i + 8) for i in range(struct.unpack_from('<H', raw, pe + 6)[0])]
    def code(rva, size):
        for _, va, length, offset in sections:
            if va <= rva and rva + size <= va + length:
                return raw[offset + rva - va:offset + rva - va + size]
        raise ValueError(hex(rva))
    return code

gd, cc = reader(game), reader(cocos)
START, END = 0x6da80, 0x6dd9e
body = bytearray(gd(START, END - START))
SANDBOX = 0x70000000
CODE, SLOTS, DATA, THUNKS, OPS = 0x0, 0x800, 0x900, 0xa00, 0xb00

# Imports this routine calls: GD IAT slot -> libcocos2d export RVA (operator code is a leaf).
imports = {0x516a08: 0xc760, 0x5168c8: 0xc6c0, 0x515c10: 0xc7c0, 0x515d70: 0xc7e0, 0x516cd0: 0xc840}
listing = subprocess.run(['llvm-objdump', '-d', f'--start-address={GD + START}', f'--stop-address={GD + END}', str(game)],
                         capture_output=True, text=True, check=True).stdout
image = bytearray(0x1000)
slot_of, data_of, thunk_of = {}, {}, {}
stub_slots = {}
op_cursor = OPS
for line in listing.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(\S+)\s*(.*)', line)
    if not m:
        continue
    address, encoded, mnemonic, operands = int(m.group(1), 16), bytes.fromhex(m.group(2).replace(' ', '')), m.group(3), m.group(4)
    offset = address - GD - START
    end = offset + len(encoded)
    target = None
    t = re.search(r'# (0x[0-9a-f]+)', operands)
    if '(%rip)' in operands and t:
        target = int(t.group(1), 16) - GD
    elif mnemonic.startswith('call') and encoded[0] == 0xe8:
        target = int(re.search(r'0x([0-9a-f]+)', operands).group(1), 16) - GD
    if target is None:
        continue
    if mnemonic.startswith('call') and encoded[0] == 0xff:
        export = imports.get(target)
        assert export is not None, hex(target)
        if target not in slot_of:
            slot_of[target] = SLOTS + 8 * len(slot_of)
            # Copy the operator's code (up to its ret) into the sandbox and point the slot at it.
            code = cc(export, 64)
            code = code[:code.index(b'\xc3') + 1]
            image[op_cursor:op_cursor + len(code)] = code
            struct.pack_into('<Q', image, slot_of[target], SANDBOX + op_cursor)
            op_cursor += 64
        destination = slot_of[target]
    elif mnemonic.startswith('call'):
        # sinf (0x4d1884), cosf (0x4d1860), corner helper (0x6dda0): a thunk through a stub slot.
        assert target in (0x4d1884, 0x4d1860, 0x6dda0), hex(target)
        if target not in thunk_of:
            slot = SLOTS + 0x80 + 8 * len(thunk_of)
            thunk = THUNKS + 8 * len(thunk_of)
            image[thunk:thunk + 6] = b'\xff\x25' + struct.pack('<i', slot - (thunk + 6))
            thunk_of[target] = thunk
            stub_slots[target] = slot
        destination = thunk_of[target]
    else:
        if target not in data_of:
            data_of[target] = DATA + 16 * len(data_of)
            image[data_of[target]:data_of[target] + 16] = gd(target, 16)
        destination = data_of[target]
    struct.pack_into('<i', body, end - 4, destination - end)
image[CODE:CODE + len(body)] = body

with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    blob = folder / 'sandbox.bin'
    blob.write_bytes(bytes(image))
    binary = folder / 'test'
    defines = [f'-DSIN_SLOT={stub_slots[0x4d1884]}', f'-DCOS_SLOT={stub_slots[0x4d1860]}', f'-DHELPER_SLOT={stub_slots[0x6dda0]}',
               f'-DSANDBOX={SANDBOX}']
    subprocess.run(['c++', '-std=c++20', '-O2', '-ffp-contract=off', *defines, '-I', str(root / 'src'),
                    str(root / 'tests/obb_math.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(blob)], check=True)
