"""Production compact dash action lifetime, clocks, copy and fallback checks."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'src/Rendering/dash_animation.cpp').read_text();body=s[s.index('namespace {'):s.index('\n$execute {')]
fixture=r'''
#include <algorithm>
#include <cassert>
#include <vector>
#include "Rendering/dash_timeline.hpp"
struct CCPoint {float x=0,y=0;};
struct CCObject {int refs=1;bool autoRelease=false;virtual ~CCObject()=default;void retain(){++refs;}void release(){assert(refs>0);--refs;}void autorelease(){autoRelease=true;}};
struct CCZone{CCObject* m_pCopyObject=nullptr;};
struct CCActionInterval;
struct CCNode:CCObject {CCPoint p;float sx=1,sy=1;bool removed=false;std::vector<CCActionInterval*> actions;
 CCPoint const& getPosition(){return p;}float getScaleX(){return sx;}float getScaleY(){return sy;}
 void setScaleX(float f){sx=f;}void setScaleY(float f){sy=f;}void setPosition(CCPoint f){p=f;}
 void removeMeAndCleanup(){removed=true;}void runAction(CCActionInterval*);
};
struct CCSprite:CCNode{unsigned char alpha=255;unsigned char getOpacity(){return alpha;}void setOpacity(unsigned char v){alpha=v;}};
namespace echo::dash {
int registrations=0;
void trackDraw(CCSprite*,CCSprite*){++registrations;}
void untrackDraw(CCSprite*){--registrations;assert(registrations>=0);}
}
struct CCActionInterval:CCObject {CCNode* m_pTarget=nullptr;float duration=0,elapsed=0;bool first=true;static bool fail;
 bool initWithDuration(float d){duration=d;return !fail;}float getDuration(){return duration;}
 virtual void startWithTarget(CCNode* p){m_pTarget=p;elapsed=0;first=true;}
 virtual void update(float){}virtual CCObject* copyWithZone(CCZone*){return nullptr;}virtual CCActionInterval* reverse(){return nullptr;}
 void step(float dt){if(first){first=false;elapsed=0;}else elapsed+=dt;update(std::min(elapsed/duration,1.f));}
};bool CCActionInterval::fail=false;
void CCNode::runAction(CCActionInterval* a){actions.push_back(a);a->startWithTarget(this);}
CCActionInterval fallback;
struct CCScaleTo{static CCActionInterval* create(float,float,float){return &fallback;}};
struct CCMoveBy{static CCActionInterval* create(float,CCPoint){return &fallback;}};
struct CCFadeTo{static CCActionInterval* create(float,unsigned char){return &fallback;}};
struct CCCallFunc{template<class T>static CCActionInterval* create(CCObject*,T){return &fallback;}};
struct CCSequence{static CCActionInterval* create(CCActionInterval*,...){return &fallback;}};
#define callfunc_selector(fn) &fn
// PRODUCTION
int main(){
 nativeRemoval=[](CCSprite* p){p->removeMeAndCleanup();};CCSprite root,outline;root.p={100,20};root.sx=2;root.sy=3;
 installAnimation(&root,&outline);assert(root.actions.size()==1&&outline.actions.size()==1&&root.refs==2&&echo::dash::registrations==1);
 auto* action=root.actions[0];assert(action->autoRelease&&action->getDuration()==.2f);
 action->step(.1f);assert(root.p.x==100&&root.sx==2&&root.alpha==255); // first delta ignored natively
 action->step(.1f);assert(root.p.x==90&&root.alpha==127&&!root.removed);
 auto* copy=static_cast<DashAnimation*>(action->copyWithZone(nullptr));assert(root.refs==3);copy->startWithTarget(&root);copy->step(1);assert(!root.removed);delete copy;assert(root.refs==2&&echo::dash::registrations==1);
 action->step(.1f);assert(root.removed&&root.p.x==90&&root.alpha==127);delete action;assert(root.refs==1&&echo::dash::registrations==0);
 root.actions.clear();outline.actions.clear();CCActionInterval::fail=true;installAnimation(&root,&outline);
 assert(root.actions.size()==3&&outline.actions.size()==1&&root.refs==1);
}
'''
with tempfile.TemporaryDirectory(prefix='echo-dash-action-') as d:
 p=Path(d)/'test.cpp';exe=Path(d)/'test';p.write_text(fixture.replace('// PRODUCTION',body))
 subprocess.run(['c++','-std=c++23','-O1','-ffp-contract=off','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Isrc',str(p),'-o',str(exe)],cwd=root,check=True)
 subprocess.run([str(exe)],check=True)
print('Production dash lifetime, first-delta, copy, completion and fallback checks passed with sanitizers.')
