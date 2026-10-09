"""Execute the installed Windows scalar routine and compare the SIMD math.

Usage: python3 tests/native_matrix_multiply.py /path/to/libcocos2d.dll
Requires an x86_64 Linux host and a C++ compiler; no emulator dependency.
"""
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
raw = Path(sys.argv[1]).read_bytes()
pe = struct.unpack_from('<I', raw, 60)[0]
table = pe + 24 + struct.unpack_from('<H', raw, pe+20)[0]
source = (root/'src/Rendering/matrix_multiply.cpp').read_text()
rva, size, expected = [int(v,16) for v in re.search(r'signature\{(0x\w+),(0x\w+),(0x\w+)\}',source).groups()]
for i in range(struct.unpack_from('<H',raw,pe+6)[0]):
    _, va, length, offset = struct.unpack_from('<IIII',raw,table+40*i+8)
    if va <= rva and rva+size <= va+length:
        code = raw[offset+rva-va:offset+rva-va+size]
        break
else:
    raise ValueError('Matrix routine is outside the PE file sections')
fingerprint = 14695981039346656037
for byte in code:
    fingerprint = ((fingerprint ^ byte)*1099511628211)&((1<<64)-1)
assert fingerprint == expected, 'Native matrix routine differs'
with tempfile.TemporaryDirectory(prefix='echo-native-matrix-') as directory:
    directory = Path(directory)
    binary = directory/'native.bin'
    binary.write_bytes(code)
    test = directory/'test'
    subprocess.run(['c++','-std=c++23','-O2','-ffp-contract=off','-Isrc',
                    'tests/matrix_multiply.cpp','-o',str(test)],cwd=root,check=True)
    subprocess.run([str(test),str(binary)],check=True)
