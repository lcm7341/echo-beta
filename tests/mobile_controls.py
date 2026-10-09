"""Exercise production recording and speed changes with deterministic game stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
record = (root/'src/Logic/record.cpp').read_text()
record = record[record.index('void Logic::recordInput('):record.index('void Logic::injectButton(')]
speed = (root/'src/GUI/actions.cpp').read_text()
speed = speed[speed.index('void GUI::setSpeed('):speed.index('void GUI::setPlaybackFps(')]
hooks=(root/'src/Hooks/hooks.cpp').read_text()
a=hooks.index('		// A touch-up from the former second dual icon')
b=hooks.index('		bool openedButton = !inOrbButton;',a)
route=hooks[a:b].replace('m_gameState','game.m_gameState').replace('m_levelSettings','game.m_levelSettings')
route='bool nativeRoute(GJBaseGameLayer& game,bool down,bool player1,int button){ auto& value=Logic::get();\n'+route+'\nreturn player1; }\n'
fixture = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>
enum class PlayerButton { Jump=1, Left=2, Right=3 };
struct Frame { int number=0; bool holding=false, player2=false; uint8_t button=1; };
struct PlayerObject {};
struct Settings { bool m_twoPlayerMode=false; };
struct GJBaseGameLayer {
 struct { bool m_isDualMode=false; } m_gameState;
 Settings* m_levelSettings=nullptr;
 PlayerObject* m_player1=nullptr; PlayerObject* m_player2=nullptr;
};
struct Logic {
 bool resetting=false, refreshOnly=false, mirrorClick=false, saveDebug=false;
 bool clickBoth=false, routingInput=false, maintainingGravity=false;
 bool recording=true, playing=false, p1=true, p2=true;
 bool applyingPlayback=false, restoringHolds=false;
 int ignoreRecordFrames=0, stepFrame=0, syncs=0;
 float speedhack=1, speedhackMemory=.5;
 std::vector<Frame> inputs;
 static Logic& get(){ static Logic l; return l; }
 bool isRecording(){ return recording; } bool isPlaying(){ return playing; }
 bool recordsPlayer(bool second){ return second ? p2 : p1; }
 bool heldAt(Logic& l,bool second,int at,int button){
  bool hold=false; for(auto const& f:l.inputs) if(f.player2==second && f.number<=at && f.button==button) hold=f.holding;
  return hold;
 }
 size_t placeRecorded(Logic&,Frame f){ inputs.push_back(f); return inputs.size()-1; }
 int capturePose(PlayerObject*) { return 0; } void setPoseAt(size_t,int) {}
 void syncTimeScale(){ ++syncs; } void syncPlaybackFps(){ ++syncs; }
 void recordInput(GJBaseGameLayer*,bool,bool,int,bool force=false);
};
struct GUI { int saves=0; void persist(){ ++saves; } void setSpeed(float,bool); };
// FUNCTIONS
int main(){
 auto& l=Logic::get(); Settings settings; GJBaseGameLayer game; game.m_levelSettings=&settings;
 // A touch started on either half of a one-player dual is canonical P1.
 for(bool sourceP1:{false,true}) {
  l=Logic{}; game.m_gameState.m_isDualMode=true;
  l.recordInput(&game,true,sourceP1,1); assert(l.inputs.size()==1 && !l.inputs[0].player2);
  game.m_gameState.m_isDualMode=false; ++l.stepFrame;
  assert(nativeRoute(game,false,false,1));
  l.applyingPlayback=true; assert(!nativeRoute(game,false,false,1)); l.applyingPlayback=false;
  l.recordInput(&game,false,false,1); assert(l.inputs.size()==2 && !l.inputs.back().player2 && !l.inputs.back().holding);
  assert(!l.heldAt(l,false,2,1));
  l.recordInput(&game,false,false,1); assert(l.inputs.size()==2);
 }
 // Two-player lanes and independent P2 holds must never be redirected.
 l=Logic{}; settings.m_twoPlayerMode=true;
 l.recordInput(&game,true,true,1); l.recordInput(&game,true,false,1); ++l.stepFrame;
 l.recordInput(&game,false,false,1); assert(l.inputs.back().player2 && l.heldAt(l,false,2,1));
 // A one-player level plays the second player's keys as player 1 (GJBaseGameLayer::handleButton
 // presses and releases player 1 when the level is not two-player), so they record as player 1.
 settings.m_twoPlayerMode=false; l=Logic{};
 l.recordInput(&game,true,true,1); l.recordInput(&game,true,false,1); ++l.stepFrame;
 assert(l.inputs.size()==1 && !l.inputs[0].player2);
 l.recordInput(&game,false,false,1); assert(!l.inputs.back().player2 && !l.inputs.back().holding && !l.heldAt(l,false,2,1));
 GUI gui; l=Logic{};
 gui.setSpeed(.25f,false); assert(l.speedhack==.25f && l.speedhackMemory==.25f && gui.saves==0);
 gui.setSpeed(1.f,false); assert(l.speedhack==1.f && l.speedhackMemory==.25f);
 gui.setSpeed(l.speedhackMemory,false); assert(l.speedhack==.25f);
 gui.setSpeed(1.025f,false); assert(l.speedhack==1.025f); // small changes must apply, not snap to 1x
 gui.setSpeed(200.f,true); assert(l.speedhack==100.f && gui.saves==1 && l.syncs==10);
 std::cout<<"Dual-exit releases, independent lanes, duplicate releases, and immediate speed changes passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='echo-mobile-controls-') as directory:
    cpp=Path(directory)/'test.cpp'; binary=Path(directory)/'test'
    cpp.write_text(fixture.replace('// FUNCTIONS', record+speed+route))
    subprocess.run(['c++','-std=c++23','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
