"""Compile production wave initializer/action against a reference-counted manager stub."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/Rendering/wave_animation.cpp').read_text()
# The animation hook only; the ring merge after it (second anonymous namespace) has its own coverage.
first=source.index('namespace {')
merge=source.find('\nnamespace {', source.index('class $modify(EchoWaveAnimation'))
body=source[first:merge if merge!=-1 else source.index('\n$execute {')]
body=body.replace('class $modify(EchoWaveAnimation, CCCircleWave)', 'struct EchoWaveAnimation : public CCCircleWave')
fixture=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "Rendering/wave_timeline.hpp"
struct CCObject { int refs=1;bool autoreleased=false;virtual ~CCObject()=default;
 void retain(){++refs;}void release(){assert(refs>0);--refs;}void autorelease(){autoreleased=true;}
};
struct CCNode : CCObject {
 virtual void updateTweenAction(float,char const*) {}
};
struct CCZone { CCObject* m_pCopyObject=nullptr; };
struct CCActionInterval : CCObject {
 float duration=0,elapsed=0;bool first=true;CCNode* m_pTarget=nullptr;
 static bool fail;
 bool initWithDuration(float d){duration=d;elapsed=0;first=true;return !fail;}
 virtual void startWithTarget(CCNode* p){m_pTarget=p;elapsed=0;first=true;}
 virtual void update(float) {}
 virtual CCObject* copyWithZone(CCZone*){return nullptr;}
 virtual CCActionInterval* reverse(){return nullptr;}
 float getDuration(){return duration;}
 void step(float dt) { if(first){elapsed=0;first=false;}else elapsed+=dt;update(std::min(elapsed/duration,1.f)); }
};
bool CCActionInterval::fail=false;
enum class CircleMode { Filled };
struct CCCircleWave:CCNode {
 CCNode* m_target=nullptr;float m_radius=123,m_opacity=1,m_opacityMod=0;
 struct { unsigned char r,g,b; } m_color{};
 CircleMode m_circleMode{};int m_lineWidth=0;bool m_blendAdditive=false;
 bool removed=false;unsigned native=0;
 virtual bool init(float,float,float,bool,bool){++native;return true;}
 void updateTweenAction(float f,char const* k) override {
  if(std::string_view(k)=="radius")m_radius=f;else {assert(std::string_view(k)=="opacity");m_opacity=f;}
 }
 virtual void removeMeAndCleanup(){removed=true;}
};
struct Manager {
 CCActionInterval* action=nullptr;bool paused=true;
 void addAction(CCActionInterval* a,CCNode* node,bool p){action=a;paused=p;a->startWithTarget(node);}
};
struct CCDirector { Manager manager;static CCDirector* sharedDirector(){static CCDirector d;return &d;}
 Manager* getActionManager(){return &manager;}
};
CCActionInterval reverseGraph;
struct CCActionTween { static CCActionInterval* create(float,char const*,float,float){return &reverseGraph;} };
struct CCCallFunc { template<class T> static CCActionInterval* create(CCObject*,T){return &reverseGraph;} };
struct CCSpawn { static CCActionInterval* create(CCActionInterval*,...){return &reverseGraph;} };
struct CCSequence { static CCActionInterval* create(CCActionInterval*,...){return &reverseGraph;} };
struct CCEaseOut { static CCActionInterval* create(CCActionInterval*,float){return &reverseGraph;} };
#define callfunc_selector(fn) &fn
// PRODUCTION
int main() {
 auto* manager=CCDirector::sharedDirector()->getActionManager();compactAnimation=true;nativePower=::powf;
 EchoWaveAnimation wave;
 assert(wave.init(5,30,.25,true,true));auto* action=manager->action;
 assert(action && !manager->paused && action->autoreleased && wave.refs==2);
 assert(wave.m_opacity==0 && wave.m_radius==5 && wave.m_opacityMod==1 && wave.m_lineWidth==2);
 action->step(.2f);assert(wave.m_radius==5 && wave.m_opacity==0 && !wave.removed); // first dt ignored
 action->step(.125f);assert(wave.m_radius==17.5f && wave.m_opacity==255 && !wave.removed);
 action->step(.125f);assert(wave.m_radius==30 && wave.m_opacity==0 && wave.removed);
 auto* clone=static_cast<CCActionInterval*>(action->copyWithZone(nullptr));assert(wave.refs==3);
 CCCircleWave other;clone->startWithTarget(&other);wave.removed=false;
 clone->update(1);assert(wave.removed && !other.removed); // native callback owns the original wave
 clone->startWithTarget(&wave);wave.removed=false;clone->step(100);assert(!wave.removed && wave.m_radius==5);
 delete clone;delete action;assert(wave.refs==1);
 assert(wave.init(5,30,.25,false,false));action=manager->action;assert(wave.m_opacity==255);
 delete action;assert(wave.refs==1);
 unsigned n=wave.native;assert(wave.init(5,30,0,false,false));assert(wave.native==n+1);
 assert(wave.init(NAN,30,.25,false,false));assert(wave.native==n+2);
 compactAnimation=false;assert(wave.init(5,30,.25,false,false));assert(wave.native==n+3);
 compactAnimation=true;CCActionInterval::fail=true;assert(wave.init(5,30,.25,false,false));
 assert(wave.native==n+4 && wave.refs==1);
}
'''
with tempfile.TemporaryDirectory(prefix='echo-wave-tests-') as d:
    cpp=Path(d)/'test.cpp';cpp.write_text(fixture.replace('// PRODUCTION',body))
    for platform in [[],['-DGEODE_IS_ANDROID64']]:
        binary=Path(d)/('arm' if platform else 'win')
        subprocess.run(['c++','-std=c++23','-O1','-g','-Wall','-Wextra','-Werror','-ffp-contract=off',
                        '-fsanitize=address,undefined','-Isrc',*platform,str(cpp),'-o',str(binary)],cwd=root,check=True)
        subprocess.run([str(binary)],check=True)
print('Production wave initializer/action: first tick, midpoint, removal, target ownership, copies, restart, fallback and failure cleanup passed in both profiles.')
