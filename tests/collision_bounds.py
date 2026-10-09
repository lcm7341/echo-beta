"""Execute production collision bounds and the installed GD/Cocos instructions."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
game = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.home() / '.local/share/Steam/steamapps/common/Geometry Dash/GeometryDash.exe'
cocos = game.with_name('libcocos2d.dll')

def code(path, rva, size):
    raw = path.read_bytes()
    pe = struct.unpack_from('<I', raw, 60)[0]
    table = pe + 24 + struct.unpack_from('<H', raw, pe + 20)[0]
    for i in range(struct.unpack_from('<H', raw, pe + 6)[0]):
        _, va, length, offset = struct.unpack_from('<IIII', raw, table + 40 * i + 8)
        if va <= rva and rva + size <= va + length:
            return raw[offset + rva - va:offset + rva - va + size]
    raise ValueError('Code outside PE sections')

with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    block = folder / 'bounds.bin'
    getters = folder / 'getters.bin'
    binary = folder / 'test'
    block.write_bytes(code(game, 0x214b09, 0x53))
    getters.write_bytes(b''.join(code(cocos, address, size) for address, size in
                                ((0xcc20, 5), (0xcbb0, 10), (0xcc30, 6), (0xcbc0, 11))))
    subprocess.run(['c++', '-std=c++20', '-O2', '-ffp-contract=off', '-I', str(root / 'src'),
                    str(root / 'tests/collision_bounds.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary), str(block), str(getters)], check=True)
