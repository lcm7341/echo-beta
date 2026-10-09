"""Check idle checkpoint resumes without inventing a jump/orb press."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/Logic/play.cpp').read_text()
start = source.index('void Logic::restoreHolds(PlayLayer* play) {')
end = source.index('\n\tfor (auto const& cmd : play->m_queuedButtons)', start)
production = source[start:end] + '\n}\n'
fixture = r'''
#include <cassert>
#include <iostream>
#include <map>
#include <vector>
enum class PlayerButton { Jump = 1 };
namespace Input {
bool key=false, mouse=false;
bool jumpKeyDown() { return key; }
bool mouseDown() { return mouse; }
}
struct PlayerObject {
 std::map<int,bool> m_holdingButtons;
 bool m_jumpBuffered=true, m_wasJumpBuffered=true;
 unsigned char m_stateJumpBuffered=1;
 double velocity=-15;
 void releaseAllButtons() { m_holdingButtons.clear(); m_jumpBuffered=false; }
};
struct UILayer {
 bool m_p1Jumping=true,m_p2Jumping=true;
 int m_p1TouchId=9,m_p2TouchId=10;
};
struct LevelSettings { bool m_twoPlayerMode=false; };
struct PlayLayer {
 PlayerObject p1,p2;
 PlayerObject *m_player1=&p1,*m_player2=&p2;
 UILayer ui; UILayer* m_uiLayer=&ui;
 LevelSettings settings; LevelSettings* m_levelSettings=&settings;
 struct { bool m_isDualMode=true; } m_gameState;
 bool m_isPracticeMode=true;
 std::vector<int> m_queuedButtons{1};
 void queueButton(int, bool, bool second, double) {
  m_queuedButtons.push_back(1);
  // Native dispatch could apply a jump/black orb to the restored body.
  (second ? p2 : p1).velocity=-99;
 }
};
struct Logic {
 bool restartHoldCaptured=true,deathJumpP1=true,deathJumpP2=true;
 bool deathLeftP1=true,deathRightP1=true,deathLeftP2=true,deathRightP2=true;
 bool keyJumpP1=true,keyJumpP2=true,physicalJumpP1=true,physicalJumpP2=true;
 bool suppressAutoHold=false,checkpoint=true;
 std::vector<int> steeredKeys{1};
 static Logic& get() { static Logic value; return value; }
 bool isPlaying() { return false; }
 bool isRecording() { return false; }
 bool backstepping=false;
 // As in play.cpp: only a practice respawn or a backstep load has a saved checkpoint.
 bool savedCheckpoint(PlayLayer* play) { return checkpoint && (play->m_isPracticeMode || backstepping); }
 void restoreHolds(PlayLayer*);
};
// PRODUCTION
int main() {
 auto& logic=Logic::get();
 for(int repeat=0; repeat<10; ++repeat) {
  PlayLayer play;
  // A release while dead must beat the stale UI and checkpoint buffers.
  Input::key=Input::mouse=false;
  logic.restoreHolds(&play);
  assert(play.m_queuedButtons.empty());
  for(auto* body:{play.m_player1,play.m_player2}) {
   assert(!body->m_holdingButtons[1] && !body->m_jumpBuffered);
   assert(!body->m_wasJumpBuffered && body->m_stateJumpBuffered==0);
   assert(body->velocity==-15);
  }
  assert(!play.ui.m_p1Jumping && !play.ui.m_p2Jumping);
  assert(play.ui.m_p1TouchId==-1 && play.ui.m_p2TouchId==-1);
 }
 // A real held press survives on both icons of one-player dual, without firing.
 PlayLayer held;
 held.ui.m_p2Jumping=false;held.ui.m_p2TouchId=-1;
 Input::key=true;
 logic.restoreHolds(&held);
 assert(held.p1.m_holdingButtons[1] && held.p2.m_holdingButtons[1]);
 assert(held.m_queuedButtons.empty() && held.p1.velocity==-15 && held.p2.velocity==-15);
 // Two-player holds remain independent.
 PlayLayer two;
 two.settings.m_twoPlayerMode=true;
 two.ui.m_p1Jumping=false;two.ui.m_p1TouchId=-1;
 logic.restoreHolds(&two);
 assert(!two.p1.m_holdingButtons[1] && two.p2.m_holdingButtons[1]);
 // Full resets and suppression never re-arm either icon.
 for(bool suppress:{false,true}) {
  PlayLayer full;
  full.m_isPracticeMode=suppress;
  logic.suppressAutoHold=suppress;
  logic.restoreHolds(&full);
  assert(!full.p1.m_holdingButtons[1] && !full.p2.m_holdingButtons[1]);
  assert(full.m_queuedButtons.empty());
 }
 std::cout<<"Repeated checkpoint releases, velocity preservation, silent dual holds and full-reset suppression passed.\n";
}
'''
with tempfile.TemporaryDirectory(prefix='echo-checkpoint-holds-') as directory:
    cpp = Path(directory) / 'test.cpp'
    binary = Path(directory) / 'test'
    cpp.write_text(fixture.replace('// PRODUCTION', production))
    subprocess.run(['c++', '-std=c++23', '-DGEODE_IS_WINDOWS', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
