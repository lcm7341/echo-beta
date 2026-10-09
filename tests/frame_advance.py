"""Compile the production runFrame against a deterministic scheduler/physics stub."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/Logic/updater.cpp').read_text()
run=source[source.index('void Logic::runFrame('):source.index('\nvoid Logic::cancelSteps(')]
hold=(root/'src/Logic/play.cpp').read_text()
hold=hold[hold.index('void Logic::pollAdvanceHold()'):hold.index('void Logic::checkAdvancePause(')]
hold=hold.replace('std::chrono::steady_clock::now()', 'testNow')
fixture=r'''
namespace echo::movement { void prepare() {} }
#include <algorithm>
#include <chrono>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <iostream>
constexpr bool kStepPatch=true;
constexpr float kVanillaTps=240;
struct GJBaseGameLayer {
 struct { float m_timeWarp=1; bool m_isDualMode=false; } m_gameState;
 double m_extraDelta=0; bool m_player2=false;
 void update(float) { assert(false && "frame advance must use the scheduler tick"); }
};
struct PlayLayer: GJBaseGameLayer {
 bool m_isPaused=false,m_started=true,m_playerDied=false;
 static PlayLayer* current;
 static PlayLayer* get() { return current; }
};
PlayLayer* PlayLayer::current=nullptr;
struct Trajectory { static Trajectory& get() { static Trajectory t;return t; } void refresh(PlayLayer*) {} void lookahead(PlayLayer*) {} };
struct GUI { static GUI& get() { static GUI g;return g; } bool typing() { return false; } };
std::chrono::steady_clock::time_point testNow{};
struct Logic {
 bool editorPaused=false,frameAdvance=true,advanceQueued=true;
 bool lockFps=false,realTimeMode=false,pauseBeforeKill=false,showTrajectory=false;
 bool deathNextP1=false,deathNextP2=false,trajDiesP1=false,trajDiesP2=false;
 bool skipPhysics=false,present=false,deferPlayerArt=false;
 int advanceSteps=1,stepsLeft=0,stepFrame=0,respawnFrames=0;
 int backstepTarget=-1; // No backstep replay runs in these tests.
 void finishBackstep(PlayLayer*) {}
 uint32_t stepsThisTick=0; int frameSteps=0;
 float carry=0,speedhack=1,playbackFps=60,rate=240;
 float trajAliveP1=0,trajAliveP2=0;
 static Logic& get() { static Logic l;return l; }
 static PlayLayer* asPlay(GJBaseGameLayer* p) { return static_cast<PlayLayer*>(p); }
 bool renderPaused() { return false; } bool renderCapturing() { return false; }
 bool renderActive() { return false; } bool deferPhysics() { return false; }
 void syncTimeScale() {} bool advancePaused(GJBaseGameLayer*) { return frameAdvance; }
 bool lockDeltaActive() { return frameAdvance; } bool isRecording() { return false; }
 bool recordsPlayer(bool) { return true; } float physicsStep() { return 1/rate; }
 float sampleRate() { return rate; } void checkAdvancePause(PlayLayer*) {}
 int countUnlockedSteps(GJBaseGameLayer*,float) { assert(false);return 0; }
 int stepsUntilRateChange() { return 2048; }
 void applyRate(int f) { if(f==1 && changeRate) rate=2000; }
 bool changeRate=false;
 bool holdingAdvance=false;
 unsigned advanceHoldMs=300;
 float advanceSpeed=100;
 std::chrono::steady_clock::time_point advanceHoldStart{},lastAdvanceStep{};
 int unlockedStepCap() { return 10000; } int affordableSteps(bool) { return 10000; }
 void pollAdvanceHold();
 void runFrame(GJBaseGameLayer*,float,std::function<void(float)> const&);
};
template<class T> void timeUpdate(T&& update) { update(); }
void settleCost() {}
void placeDeferredPlayerArt(GJBaseGameLayer*) {}
namespace phys { bool fastPredictionPosition() { return false; } }
// PRODUCTION
int main() {
 PlayLayer play;PlayLayer::current=&play;int cases=0;
 for(float rate:{1.f,240.f,1000.f,10000.f,100000.f})
 for(float dt:{0.f,1.f/240,1.f/60,0.25f,2.f})
 for(float speed:{0.01f,1.f,100.f})
 for(float warp:{0.f,0.25f,1.f,2.f})
 for(bool lock:{false,true}) {
  auto& v=Logic::get();v=Logic{};v.rate=rate;v.speedhack=speed;v.lockFps=lock;
  v.carry=0.37f;v.respawnFrames=3;play.m_gameState.m_timeWarp=warp;play.m_extraDelta=0.45;
  int ticks=0;
  auto tick=[&](float delta) { ++ticks;assert(v.stepsLeft==1);assert(v.stepsThisTick==1);assert(delta==1/rate);++v.stepFrame; };
  v.runFrame(&play,dt,tick);
  assert(ticks==1 && v.stepFrame==1 && !v.advanceQueued);
  assert(v.stepsLeft==0 && v.stepsThisTick==0 && v.carry==0 && play.m_extraDelta==0);
  v.runFrame(&play,dt,tick);assert(ticks==1);++cases;
 }
 auto& v=Logic::get();v=Logic{};v.advanceSteps=30;v.changeRate=true;
 int ticks=0;v.runFrame(&play,1.f/60,[&](float delta) {
  assert(delta==1/(ticks==0?240.f:2000.f));++ticks;++v.stepFrame;
 });assert(ticks==30 && v.advanceSteps==1 && !v.advanceQueued);
 v=Logic{};v.advanceSteps=20;ticks=0;play.m_playerDied=false;
 v.runFrame(&play,1.f/60,[&](float){ ++ticks;play.m_playerDied=true; });
 assert(ticks==1 && v.stepsLeft==0 && v.stepsThisTick==0);
 play.m_playerDied=false;v=Logic{};v.holdingAdvance=true;v.rate=10000;
 v.advanceQueued=false;
 testNow=std::chrono::steady_clock::time_point{}+std::chrono::milliseconds(299);
 v.pollAdvanceHold();assert(!v.advanceQueued);
 testNow+=std::chrono::milliseconds(1);v.pollAdvanceHold();assert(!v.advanceQueued);
 testNow+=std::chrono::microseconds(16550);v.pollAdvanceHold();
 assert(v.advanceQueued && v.advanceSteps==165);
 v.advanceQueued=false;testNow+=std::chrono::microseconds(16550);v.pollAdvanceHold();
 assert(v.advanceQueued && v.advanceSteps==166);
 v.advanceQueued=false;v.holdingAdvance=false;testNow+=std::chrono::seconds(1);
 v.pollAdvanceHold();assert(!v.advanceQueued);
 std::cout<<cases<<" frame-advance combinations plus held repeat, rate transition and death passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='echo-frame-tests-') as d:
    cpp=Path(d)/'test.cpp';binary=Path(d)/'test'
    cpp.write_text(fixture.replace('// PRODUCTION',run+hold))
    subprocess.run(['c++','-std=c++23','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
