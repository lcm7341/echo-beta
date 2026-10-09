"""Exercise the actual end-check guard against a native-style completion latch."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/Hooks/hooks.cpp').read_text()
start=s.index('\tvoid checkForEnd()');end=s.index('\n\tvoid levelComplete()',start)
fixture=r'''
#include <cassert>
#include <iostream>
struct Logic { bool predicting=false;static Logic& get(){static Logic l;return l;} };
struct PlayLayer {
 bool m_hasCompletedLevel=false,atEnd=false;int animations=0,checks=0;
 void checkForEnd(){++checks;if(atEnd&&!m_hasCompletedLevel){m_hasCompletedLevel=true;++animations;}}
};
struct EchoPlayLayer:PlayLayer {
// GUARD
};
int main(){
 EchoPlayLayer play;
 for(int i=0;i<100;++i){
  Logic::get().predicting=true;play.atEnd=true;play.checkForEnd();
  assert(!play.m_hasCompletedLevel && play.animations==0 && play.checks==i);
  Logic::get().predicting=false;play.atEnd=false;play.checkForEnd();
  assert(!play.m_hasCompletedLevel && play.animations==0);
 }
 play.atEnd=true;play.checkForEnd();assert(play.m_hasCompletedLevel&&play.animations==1);
 play.checkForEnd();assert(play.animations==1);
 std::cout<<"100 predicted finishes leave completion clear; real finish starts endscreen exactly once\n";
}
'''.replace('// GUARD',s[start:end])
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(fixture)
 subprocess.run(['c++','-std=c++20','-O2',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
