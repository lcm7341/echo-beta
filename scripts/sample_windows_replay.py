"""Read-only Linux/Wine main-thread CPU sampler; briefly stops at each sample."""
import argparse
import collections
import ctypes
import json
import os
import struct
import time
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('pid', type=int)
parser.add_argument('--seconds', type=float, default=5)
parser.add_argument('--output', required=True)
args = parser.parse_args()
libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_void_p, ctypes.c_void_p]
libc.ptrace.restype = ctypes.c_long

def ptrace(op, data=None):
    if libc.ptrace(op, args.pid, None, data) == -1:
        raise OSError(ctypes.get_errno(), 'ptrace')

maps = []
for line in Path(f'/proc/{args.pid}/maps').read_text().splitlines():
    parts = line.split(maxsplit=5)
    start, end = [int(x, 16) for x in parts[0].split('-')]
    maps.append((start, end, parts[5] if len(parts) > 5 else '[anonymous]'))
images = []
ptrace(16)
try:
    os.waitpid(args.pid, 0)
    mem = os.open(f'/proc/{args.pid}/mem', os.O_RDONLY)
    try:
        for begin, end, path in maps:
            try:
                header = os.pread(mem, min(end-begin, 4096), begin)
                if header[:2] != b'MZ':
                    continue
                pe = struct.unpack_from('<I', header, 60)[0]
                if header[pe:pe+4] != b'PE\0\0':
                    continue
                size = struct.unpack_from('<I', header, pe+24+56)[0]
                exports = struct.unpack_from('<I', header, pe+24+112)[0]
                name = Path(path).name if path != '[anonymous]' else hex(begin)
                if exports:
                    export_header = os.pread(mem, 40, begin+exports)
                    name_rva = struct.unpack_from('<I', export_header, 12)[0]
                    name = os.pread(mem, 256, begin+name_rva).split(b'\0')[0].decode(errors='replace')
                if begin == 0x140000000:
                    name = 'GeometryDash.exe'
                images.append((begin, begin+size, name))
            except (OSError, OverflowError, struct.error):
                continue
    finally:
        os.close(mem)
finally:
    ptrace(17)
counts = collections.Counter()
addresses = collections.Counter()
regs = (ctypes.c_ulonglong * 27)()
started = time.monotonic()
while time.monotonic() - started < args.seconds:
    attached = False
    try:
        ptrace(16)
        attached = True
        os.waitpid(args.pid, 0)
        ptrace(12, ctypes.byref(regs))
        rip = regs[16]
        addresses[hex(rip)] += 1
        module = next((path for begin, end, path in images if begin <= rip < end), None)
        if module is None:
            module = next((path for begin, end, path in maps if begin <= rip < end), '[unknown]')
        counts[module] += 1
    finally:
        if attached:
            ptrace(17)
    time.sleep(.001)
result = dict(pid=args.pid, elapsed=time.monotonic()-started,
              samples=sum(counts.values()), modules=counts.most_common(),
              addresses=addresses.most_common(), mappings=maps, images=images,
              limitation='Sampling briefly stops the main thread; not a throughput benchmark.')
Path(args.output).write_text(json.dumps(result, indent=2))
print(json.dumps({k: result[k] for k in ('pid', 'elapsed', 'samples', 'modules')}, indent=2))
