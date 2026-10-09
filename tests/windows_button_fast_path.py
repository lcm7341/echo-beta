"""Differential test the Windows in-place queue path against the general pipeline."""
from pathlib import Path
import re,subprocess,tempfile
root=Path(__file__).resolve().parents[1];s=(root/'src/Logic/play.cpp').read_text()
prepare=s[s.index('void Logic::prepareButtons('):s.index('void Logic::rememberMove(')]
pump=s[s.index('void runOrbClicks('):s.index('void Logic::offsetInputs(')]
reference=pump[pump.index('void Logic::pumpButtons('):]
reference=re.sub(r'#ifdef GEODE_IS_WINDOWS.*?#endif','',reference,flags=re.S).replace('Logic::pumpButtons(', 'Logic::pumpButtonsReference(')
f=(root/'tests/button_pipeline_fixture.hpp').read_text().replace('#include <iostream>','#include <iostream>\n#include <random>\n#include <functional>\n#include <tuple>')
f=f.replace('    void pumpButtons(GJBaseGameLayer*, GJBaseGameLayer*, float, bool);','    void pumpButtons(GJBaseGameLayer*, GJBaseGameLayer*, float, bool);\n    void pumpButtonsReference(GJBaseGameLayer*, GJBaseGameLayer*, float, bool);')
f=f.replace('bool autoDual = false;','bool autoDual = false;\nstd::function<void()> buttonCallback;')
f=f.replace('    if (clear) m_queuedButtons.clear();','    if (buttonCallback) {auto callback=std::move(buttonCallback);buttonCallback={};callback();}\n    if (clear) m_queuedButtons.clear();')
f=f.replace('    bool operator==(PlayerButtonCommand const&) const = default;', '''    static inline size_t copies = 0;
    PlayerButtonCommand() = default;
    PlayerButtonCommand(PlayerButtonCommand const& other) { *this = other; }
    PlayerButtonCommand& operator=(PlayerButtonCommand const& other) {
        ++copies; m_button=other.m_button; m_isPush=other.m_isPush;
        m_isPlayer2=other.m_isPlayer2; m_step=other.m_step; m_timestamp=other.m_timestamp;
        return *this;
    }
    bool operator==(PlayerButtonCommand const&) const = default;''')
f=f.replace('int main() {','void regressions() {')
f=f.replace('// PRODUCTION_PIPELINE',prepare+pump+reference)
f+=r'''
auto scenario(unsigned seed,bool fast){
 reset();std::mt19937 rng(seed);auto& l=Logic::get();PlayLayer play;
 l.playing=(rng()%2)==0;l.recording=(rng()%2)==0;
 l.playP1=(rng()%2)==0;l.playP2=(rng()%2)==0;
 l.recordP1=(rng()%2)==0;l.recordP2=(rng()%2)==0;
 l.maintainGravity=(rng()%5)==0;l.sameGravity=(rng()%2)==0;
 l.restoringHolds=(rng()%13)==0;l.editorPaused=(rng()%23)==0;
 l.applyingPlayback=(rng()%2)==0;l.suppressClickHitbox=(rng()%2)==0;
 play.m_isPaused=(rng()%29)==0;play.m_isPlatformer=(rng()%3)==0;
 play.m_gameState.m_isDualMode=(rng()%3)==0;play.settings.m_twoPlayerMode=(rng()%2)==0;
 autoDual=(rng()%2)==0;
 int count=rng()%6;for(int i=0;i<count;++i)play.m_queuedButtons.push_back(cmd(rng()%2,rng()%2,1+rng()%3));
 count=rng()%6;for(int i=0;i<count;++i)l.inputs.push_back({1,bool(rng()%2),bool(rng()%2),1+int(rng()%3)});
 if(rng()%4==0)pads.push_back(cmd(true));
 if(seed%5==0)buttonCallback=[&]{pads.push_back(cmd(true));l.maintainGravity=true;l.sameGravity=true;};
 bool clear=(rng()%4)!=0;
 if(fast)l.pumpButtons(&play,&play,1.f/5000,clear);
 else l.pumpButtonsReference(&play,&play,1.f/5000,clear);
 buttonCallback={};
 return std::tuple{events,commits,play.m_queuedButtons,l.replayPos,l.replayClicksLeft,
  l.applyingPlayback,l.injecting,l.suppressClickHitbox,l.buttonsPrepared,l.partnerPresses,play.p1.held,play.p2.held};
}
size_t copies(bool fast){
 reset();auto& l=Logic::get();PlayLayer play;
 l.playing=true;l.buttonsPrepared=true;l.replayClicksLeft=1;
 play.m_queuedButtons.push_back(cmd(true));PlayerButtonCommand::copies=0;
 if(fast)l.pumpButtons(&play,&play,1.f/5000,true);
 else l.pumpButtonsReference(&play,&play,1.f/5000,true);
 return PlayerButtonCommand::copies;
}
int main(){auto oldCopies=copies(false);auto newCopies=copies(true);
 assert(oldCopies==3 && newCopies==0);
 std::cout<<"Single-command replay uses "<<newCopies<<" command copies instead of "<<oldCopies<<".\n";
 regressions();for(unsigned seed=0;seed<10000;++seed){
 auto reference=scenario(seed,false);auto fast=scenario(seed,true);assert(reference==fast);
 }
 // 5000-TPS alternating inputs use one native queue dispatch each, in order.
 reset();auto& l=Logic::get();PlayLayer play;l.playing=true;
 for(int i=1;i<=10000;++i)l.inputs.push_back({i,bool(i&1),false,1});
 for(int i=0;i<10000;++i){l.stepFrame=i;l.pumpButtons(&play,&play,1.f/5000,true);}
 assert(l.replayPos==10000 && events.size()==10000 && commits==10000);
 for(int i=0;i<10000;++i)assert(events[i].kind==0&&events[i].down==bool((i+1)&1)&&events[i].playback);
 std::cout<<"10,000 differential input/tool/dual/callback cases and 10,000 alternating 5000-TPS ticks passed.\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 cpp=Path(d)/'test.cpp';exe=Path(d)/'test';cpp.write_text(f)
 subprocess.run(['c++','-std=c++23','-DGEODE_IS_WINDOWS','-O1','-g','-fsanitize=address,undefined','-Isrc',str(cpp),'-o',str(exe)],cwd=root,check=True)
 subprocess.run([str(exe)],check=True)
