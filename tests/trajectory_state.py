"""Run production hook toggling, fake seeding and invisible-trail guards."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
def function(text, signature):
 start=text.index(signature); brace=text.index('{',start);depth=1;end=brace+1
 while depth:
  depth += (text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
hooks=(root/'src/Hooks/hooks.cpp').read_text();trajectory=(root/'src/Hack/trajectory.cpp').read_text()
fixture=r'''
#include <cassert>
#include <vector>
#include <iostream>
struct Point { float x=0,y=0; bool operator==(Point const&)const=default; };
struct PlayerObject {
 bool m_maybeReducedEffects=false,m_isHidden=false,m_playEffects=true;
 bool m_isObjectRectDirty=false,m_justPlacedStreak=true;
 void* m_pendingCheckpoint=nullptr;
 Point m_position; double velocity=0;float rotation=0;int trails=0;
 void copyAttributes(PlayerObject*){}
 void setPosition(Point p){m_position=p;}void setRotation(float r){rotation=r;}
 float getRotation(){return rotation;}void updateOrientedBox(){}
 void placeStreakPoint(){++trails;m_justPlacedStreak=false;}
};
struct SavedPlayerCheckpoint {
 PlayerObject shot;
 static SavedPlayerCheckpoint create(PlayerObject* p){return {*p};}
 void apply(PlayerObject* p){*p=shot;}
};
struct Result{operator bool(){return true;}int unwrapErr(){return 0;}};
struct Hook{bool on=false;int changes=0;Result enable(){on=true;++changes;return {};}
 Result disable(){on=false;++changes;return {};}};
std::vector<Hook*>& predictHooks(){static std::vector<Hook*> hooks;return hooks;}
bool predictHooksOn=false;
namespace log { void warn(char const*,int){} }
struct Logic{bool predicting=false,showTrajectory=true,hidden=false;
 static Logic& get(){static Logic value;return value;}void syncPredictHooks();
 bool playerHidden(PlayerObject*){return hidden;}};
struct Trajectory {PlayerObject* fake=nullptr;static Trajectory& get(){static Trajectory t;return t;}
 bool isFake(PlayerObject* p){return p==fake;}void seed(PlayerObject*,PlayerObject*);};
// SYNC
// SEED
struct EchoPlayer:PlayerObject {
// STREAK
};
int main(){
 Hook a,b;predictHooks()={&a,&b};auto& l=Logic::get();
 l.syncPredictHooks();assert(a.on && a.changes==1);
 for(int frame=0;frame<100;++frame){
  l.predicting=true;l.syncPredictHooks();assert(a.on&&b.on);
  l.predicting=false;l.syncPredictHooks();assert(a.on&&b.on);
 }
 assert(a.changes==1);l.showTrajectory=false;l.syncPredictHooks();
 assert(!a.on && a.changes==2);
 l.showTrajectory=true;l.syncPredictHooks();assert(a.on&&a.changes==3);
 PlayerObject real;real.m_position={42,91};real.velocity=-123;real.rotation=37;
 EchoPlayer fake;Trajectory::get().fake=&fake;Trajectory::get().seed(&fake,&real);
 assert(fake.m_maybeReducedEffects && fake.m_isHidden && !fake.m_playEffects);
 assert(fake.m_position==real.m_position && fake.velocity==real.velocity && fake.rotation==real.rotation);
 assert(!real.m_maybeReducedEffects && !real.m_isHidden && real.m_playEffects);
 for(int i=0;i<20000;++i){fake.m_justPlacedStreak=true;fake.placeStreakPoint();assert(!fake.m_justPlacedStreak);}
 assert(fake.trails==0);
 EchoPlayer live;live.placeStreakPoint();assert(live.trails==1&&!live.m_justPlacedStreak);
 std::cout<<"100 prediction passes without handler recreation; exact seed physics; 20,000 invisible trail calls skipped with native latch preserved\n";
}
'''
fixture=fixture.replace('// SYNC',function(hooks,'void Logic::syncPredictHooks()'))
fixture=fixture.replace('// SEED',function(trajectory,'void Trajectory::seed('))
fixture=fixture.replace('// STREAK',function(hooks,'void placeStreakPoint()'))
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'test.cpp';binary=Path(tmp)/'test';cpp.write_text(fixture)
 subprocess.run(['c++','-std=c++20','-O2',str(cpp),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
