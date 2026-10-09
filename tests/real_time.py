"""Run production scheduling: bound catch-up batches without losing real-time ticks."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/Logic/updater.cpp').read_text()
s=s[s.index('int Logic::countUnlockedSteps('):s.index('\nvoid Logic::runFrame(')]
fixture=r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <iostream>
struct GJBaseGameLayer { struct {float m_timeWarp=1;}m_gameState;double m_extraDelta=0; };
struct Logic {
 int stepFrame=0,respawnFrames=0,budget=128,cap=6000,frameSteps=0;
 bool realTimeMode=true,lockFps=false,rendering=false,lastVisual=false;
 float playbackFps=60,rate=31010; double carry=0;
 static Logic& get(){static Logic l;return l;}
 void applyRate(int){}float physicsStep(){return 1.f/rate;}
 int unlockedStepCap(){return cap;}int affordableSteps(bool visual){lastVisual=visual;return budget;}
 bool renderCapturing(){return rendering;}int countUnlockedSteps(GJBaseGameLayer*,double);
};
// FUNCTION
int main(){
 auto& l=Logic::get();GJBaseGameLayer play;
 for(float rate:{240.f,31010.f,1000000.f})for(float warp:{.25f,1.f})for(bool fps:{false,true}){
  l=Logic{};l.rate=rate;l.lockFps=fps;play.m_gameState.m_timeWarp=warp;
  double received=0,executed=0;int maxTaken=0;
  for(int frame=0;frame<100;++frame){
   float dt=frame==30?.5f:1.f/60;received+=dt;
   int taken=l.countUnlockedSteps(&play,dt);assert(taken<=128);maxTaken=std::max(maxTaken,taken);
   executed+=taken*double(l.physicsStep()*warp);
   assert(std::abs(received-executed-l.carry)<.0001);
   assert(l.lastVisual==fps);
  }
  assert(maxTaken==128 || rate==240.f);
  int batches=0;
  while(l.carry>=l.physicsStep()*warp){int taken=l.countUnlockedSteps(&play,0);
   assert(taken>0 && taken<=128);executed+=taken*double(l.physicsStep()*warp);
   assert(++batches<100000);
  }
  assert(std::abs(received-executed-l.carry)<.001);
 }
 // One display frame recounts after every chunk; together the recounts stay within one budget
 // and the rest of the backlog waits for later frames.
 l=Logic{};play.m_gameState.m_timeWarp=1;
 {int taken=l.countUnlockedSteps(&play,30.f),total=0,chunks=0;double backlog=l.carry;
  while(taken>0){int chunk=std::min(taken,48);total+=chunk;l.frameSteps+=chunk;assert(++chunks<100);
   double left=(taken-chunk)*double(l.physicsStep()),carry=l.carry;l.carry=0;taken=l.countUnlockedSteps(&play,left+carry);}
  assert(total==128 && std::abs(l.carry-backlog)<1e-3);}
 l=Logic{};play.m_gameState.m_timeWarp=1;l.budget=std::numeric_limits<int>::max();
 int warmup=l.countUnlockedSteps(&play,2.f);assert(warmup==6000 && l.carry>1.8f);
 l=Logic{};l.realTimeMode=false;
 assert(l.countUnlockedSteps(&play,2.f)==128 && l.carry==0); // old slowdown mode
 l=Logic{};l.rendering=true;
 int rendered=l.countUnlockedSteps(&play,1.f);assert(rendered>30000 && l.carry<l.physicsStep());
 l=Logic{};l.respawnFrames=1;l.carry=100;
 assert(l.countUnlockedSteps(&play,1.f/60)==1 && l.respawnFrames==0);
 l=Logic{};l.rate=1000000;
 assert(l.countUnlockedSteps(&play,100000.f)==128 && l.carry>99999.f); // no int overflow
 std::cout<<"31,010/1,000,000 TPS catch-up, frame spikes, backlog conservation/draining, warm-up, FPS lock, respawn and render timing passed\n";
}
'''.replace('// FUNCTION',s)
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(fixture)
 subprocess.run(['c++','-std=c++20','-O2',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
