"""Run production entry scheduling and physics hooks against a startup stub."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
play = (root / 'src/Logic/play.cpp').read_text()
updater = (root / 'src/Logic/updater.cpp').read_text()
hooks = (root / 'src/Hooks/hooks.cpp').read_text()
defer = play[play.index('bool Logic::deferPhysics() const {'):play.index('\ndouble Logic::clickStamp()')]
entry = updater[updater.index('void Logic::runFrame('):updater.index('\n\tif (paused)', updater.index('void Logic::runFrame('))] + '\n}\n'
start = hooks.index('\tvoid update(float dt) {', hooks.index('class $modify(EchoGameLayer, GJBaseGameLayer)'))
physics = hooks[start:hooks.index('\n\tvoid addToSection(', start)]
start = hooks.index('int vanillaPhysicsSteps(float dt, float timeWarp) {')
vanilla = hooks[start:hooks.index('\n#ifdef GEODE_IS_ANDROID64', start)]
fixture = r'''
namespace echo::movement { void prepare() {} }
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
struct GJBaseGameLayer {
 struct { float m_timeWarp=1; } m_gameState;
 int updates=0;
 void update(float);
 double getModifiedDelta(float dt) { return dt; }
};
struct PlayLayer : GJBaseGameLayer {
 bool m_started=false,m_isPaused=false,m_playerDied=false;
 static PlayLayer* current;
 static PlayLayer* get() { return current; }
};
PlayLayer* PlayLayer::current=nullptr;
struct LevelEditorLayer : GJBaseGameLayer { bool m_playbackActive=false; };
struct Logic {
 bool recording=false,playing=false,frameAdvance=false,rendering=false,nativeClick=false;
 bool refreshOnly=false,skipPhysics=false,editorPaused=false,editorTesting=false;
 bool disableShakes=false,layoutMode=false;
 float carry=.4f; int stepsLeft=0;
 uint32_t stepsThisTick=0; int frameSteps=0;
 static Logic& get() { static Logic value; return value; }
 static PlayLayer* asPlay(GJBaseGameLayer* p) { return p==PlayLayer::current ? PlayLayer::current : nullptr; }
 static LevelEditorLayer* asEditor(GJBaseGameLayer*) { return nullptr; }
 bool physicsLocked() const { return recording || playing || frameAdvance; }
 bool lockDeltaActive() { return physicsLocked(); }
 bool renderCapturing() { return rendering; }
 bool renderPaused() { return false; }
 bool vanillaClickPhysics() { return nativeClick; }
 int countUnlockedSteps(GJBaseGameLayer*,float) { assert(false && "startup must use native timing"); return 0; }
 float physicsStep() const { return 1.f/10000; }
 bool deferPhysics() const;
 void runFrame(GJBaseGameLayer*,float,std::function<void(float)> const&);
};
// DEFER
// ENTRY
// VANILLA
struct EchoGameLayer : PlayLayer {
 bool m_skipCameraShake=false;
 // PHYSICS
};
void GJBaseGameLayer::update(float) {
 ++updates;
 // GD must receive physics ticks to finish initializing the attempt.
 if (Logic::get().stepsThisTick>0) PlayLayer::get()->m_started=true;
}
int main() {
 auto& logic=Logic::get();
 for(bool nativeClick:{false,true}) for(int mode=0;mode<4;++mode) for(float dt:{0.f,1.f/60,1.f/240}) {
  logic=Logic{};
  logic.nativeClick=nativeClick;
  logic.recording=mode==1;logic.playing=mode==2;logic.frameAdvance=mode==3;
  EchoGameLayer layer;PlayLayer::current=&layer;
  int ticks=0;
  logic.runFrame(&layer,dt,[&](float delta) {
   ++ticks;assert(delta==dt);
   assert(layer.getModifiedDelta(delta)==delta);
   layer.update(delta);
  });
  assert(ticks==1 && layer.updates==1 && layer.m_started);
  assert(logic.stepsThisTick>=1 && logic.stepsLeft==0 && logic.carry==0);
  // Once started, recording/frame advance must return to controlled timing.
  if(mode) assert(!logic.deferPhysics());
 }
 // Rendering keeps its explicit step budget even during the intro.
 logic=Logic{};logic.recording=true;logic.rendering=true;
 EchoGameLayer render;PlayLayer::current=&render;
 assert(!logic.deferPhysics());
 logic.refreshOnly=true;assert(!logic.deferPhysics());
 std::cout<<"Idle, recording, playback and frame-advance entry complete with native startup timing; rendering budget preserved.\n";
}
'''
fixture = fixture.replace('// DEFER', defer).replace('// ENTRY', entry).replace('// VANILLA', vanilla).replace('// PHYSICS', physics)
with tempfile.TemporaryDirectory(prefix='echo-level-entry-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(fixture)
    subprocess.run(['c++', '-std=c++23', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
