"""Ensure production orb restore skips clean objects but repairs changed/dirty ones."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/Hack/orbs.cpp').read_text()
restore=s[s.index('void Orbs::restore('):s.index('\nuint32_t Orbs::poseHash')]
fixture=r'''
#include <vector>
#include <cassert>
#include <iostream>
struct Point { float x=0,y=0; bool operator==(Point const&)const=default; };
struct Object {
 Point p; float r=0,x=0,y=0; bool m_isOrientedBoxDirty=false; int writes=0,boxes=0;
 Point getPosition(){return p;} float getRotation(){return r;}
 float getRotationX(){return x;} float getRotationY(){return y;}
 void setPosition(Point v){p=v;++writes;} void setRotation(float v){r=v;++writes;}
 void setRotationX(float v){x=v;++writes;} void setRotationY(float v){y=v;++writes;}
 void updateOrientedBox(){++boxes;m_isOrientedBoxDirty=false;}
};
struct Orbs { struct Pose { Object* object; Point pos; float rotation,rotationX,rotationY; };
 static void restore(std::vector<Pose> const&); };
// RESTORE
int main(){
 Object o; std::vector<Orbs::Pose> poses{{&o,{1,2},3,4,5},{nullptr,{},0,0,0}};
 Orbs::restore(poses);assert(o.writes==4 && o.boxes==1);
 for(int i=0;i<20000;++i)Orbs::restore(poses);
 assert(o.writes==4 && o.boxes==1);
 o.r=10;Orbs::restore(poses);assert(o.r==3 && o.writes==5 && o.boxes==2);
 o.m_isOrientedBoxDirty=true;Orbs::restore(poses);assert(o.boxes==3 && o.writes==5);
 std::cout<<"20,000 clean restores: zero transform writes/bounds rebuilds; changed and dirty restores pass\n";
}
'''.replace('// RESTORE',restore)
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'test.cpp'; binary=Path(tmp)/'test';cpp.write_text(fixture)
 subprocess.run(['c++','-std=c++20','-O2',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
