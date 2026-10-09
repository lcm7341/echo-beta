"""Audit the supported native setter and run the production physics-only setter.

This is a setter/component check, not a full-game replay/FPS measurement.
"""
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import sys
root=Path(__file__).resolve().parents[1]
exe=Path(sys.argv[1]) if len(sys.argv)>1 else Path.home()/'.local/share/Steam/steamapps/common/Geometry Dash/GeometryDash.exe'
raw=exe.read_bytes();pe=struct.unpack_from('<I',raw,60)[0]
table=pe+24+struct.unpack_from('<H',raw,pe+20)[0]
rva=0x39c660;size=0xcab
for i in range(struct.unpack_from('<H',raw,pe+6)[0]):
 _,va,length,offset=struct.unpack_from('<IIII',raw,table+40*i+8)
 if va<=rva and rva+size<=va+length:
  code=raw[offset+rva-va:offset+rva-va+size];break
else:raise ValueError('Native setter is outside PE sections')
hash=14695981039346656037
for b in code:hash=((hash^b)*1099511628211)&((1<<64)-1)
source=(root/'src/Physics/prediction_art.cpp').read_text()
expected=int(re.search(r'return hash == (0x[0-9a-f]+)ull',source)[1],16)
assert hash==expected,'Native setter differs: fast path must stay disabled'
assembly=subprocess.check_output(['objdump','-d','-Mintel','--start-address=0x14039c640',
 '--stop-address=0x14039d30b',str(exe)],text=True)
# r15 holds the player for the whole routine; no stores to player state in
# the art tail. The prefix sets both dirty bytes at [rcx+0x368] before calling
# GameObject::setPosition. Later calls on rcx=r15 only get the node position.
assert 'mov    r15,rcx' in assembly
assert 'mov    WORD PTR [rcx+0x368],0x101' in assembly
assert 'call   0x140197b60' in assembly
instructions=[]
for line in assembly.splitlines():
 parts=line.split('\t')
 if len(parts)>=3:instructions.append(parts[-1].strip())
for instruction in instructions:
 if re.match(r'(?:mov\w*|add|sub|or|and|inc|dec|xchg)\s+(?:\w+ PTR )?\[r15',instruction):
  raise AssertionError(f'Unexpected player state store: {instruction}')
assert len([i for i in instructions if re.match(r'mov\s+r15,',i)])==1
header=(root/'src/Physics/prediction_art.hpp').read_text().replace('#include <Geode/Geode.hpp>','')
fixture=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
namespace cocos2d { struct CCPoint {float x,y;}; }
struct GameObject {
 bool m_isObjectRectDirty=false,m_isOrientedBoxDirty=false;
 cocos2d::CCPoint nodePosition{0,0};int baseCalls=0;
 void setPosition(cocos2d::CCPoint const& p){nodePosition=p;++baseCalls;}
};
struct PlayerObject:GameObject {
 double velocity=-13.75, xVelocity=42;float rotation=91;
 bool upsideDown=true,dashing=true;int physicsFrame=123;
};
// HEADER
int main(){
 PlayerObject player;
 for(int frame=0;frame<31010;++frame){
  player.m_isObjectRectDirty=false;player.m_isOrientedBoxDirty=false;
  auto previous=player;
  cocos2d::CCPoint point{float(frame)*.013f,float(frame%123)-50};
  phys::positionWithoutArt(&player,point);
  assert(player.m_isObjectRectDirty && player.m_isOrientedBoxDirty);
  assert(player.nodePosition.x==point.x && player.nodePosition.y==point.y);
  assert(player.baseCalls==frame+1);
  assert(player.velocity==previous.velocity && player.xVelocity==previous.xVelocity);
  assert(player.rotation==previous.rotation && player.upsideDown==previous.upsideDown);
  assert(player.dashing==previous.dashing && player.physicsFrame==previous.physicsFrame);
 }
 std::cout<<"Native setter fingerprint/write audit and 31,010 production setter position/bounds/physics checks passed\n";
}
'''.replace('// HEADER',header)
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(fixture)
 subprocess.run(['c++','-std=c++20','-O2',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
