"""Compare production movement against installed Windows code on x86_64 Linux."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import sys
root=Path(__file__).resolve().parents[1]
exe=Path(sys.argv[1]) if len(sys.argv)>1 else Path.home()/'.local/share/Steam/steamapps/common/Geometry Dash/GeometryDash.exe'
raw=exe.read_bytes();pe=struct.unpack_from('<I',raw,60)[0];table=pe+24+struct.unpack_from('<H',raw,pe+20)[0]
def code(rva,size):
 for i in range(struct.unpack_from('<H',raw,pe+6)[0]):
  _,va,length,offset=struct.unpack_from('<IIII',raw,table+40*i+8)
  if va<=rva and rva+size<=va+length:return raw[offset+rva-va:offset+rva-va+size]
 raise ValueError('Routine is outside PE sections')
source=(root/'src/Physics/moving_objects.cpp').read_text()
hashes=[int(x,16) for x in re.findall(r'== (0x[0-9a-f]+)ull',source)]
for (rva,size),expected in zip([(0x22dd70,0x160),(0x227f70,0xf7)],hashes):
 h=14695981039346656037
 for b in code(rva,size):h=((h^b)*1099511628211)&((1<<64)-1)
 assert h==expected,'Unsupported native routine'
with tempfile.TemporaryDirectory() as tmp:
 tmp=Path(tmp);move=tmp/'move.bin';gate=tmp/'gate.bin';binary=tmp/'test'
 move.write_bytes(code(0x22dd50,0x180));gate.write_bytes(code(0x227f50,0x117))
 subprocess.run(['c++','-std=c++20','-O2','-ffp-contract=off','-pthread','-I',str(root/'src'),str(root/'tests/moving_objects.cpp'),'-o',str(binary)],check=True)
 subprocess.run([str(binary),str(move),str(gate)],check=True)
