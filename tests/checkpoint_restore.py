"""Exercise production snapshot/identity restoration against a native-state stub.

The stub deliberately changes physics during mode/scale/gravity transitions.
This checks restore ordering, not Android's full game physics or visual output.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
record = (root/'src/Logic/record.cpp').read_text()
play = (root/'src/Logic/play.cpp').read_text()
header = (root/'src/Logic/logic.hpp').read_text()
hooks = (root/'src/Hooks/hooks.cpp').read_text()
rotation = hooks[hooks.index('std::unordered_map<PlayerObject*, float>& tiltFix()'):hooks.index('class $modify(EchoPlayLayer, PlayLayer)')]
shot = header[header.index('struct PlayerSnapshot {'):header.index('struct LayerSnapshot {')]
point_fields = {'m_lastGroundedPos', 'm_stateForceVector'}
shot = re.sub(r'decltype\(PlayerObject::(\w+)\)',
              lambda m: 'cocos2d::CCPoint' if m[1] in point_fields else
              'void*' if m[1] == 'm_dashRing' else 'double', shot)
# Keep declaration order, with a name->type map.
types = {name: kind for kind, name in re.findall(
    r'^\s*(bool|float|double|int|void\*|cocos2d::CCPoint)\s+(\w+)', shot, re.M)}
capture = record[record.index('PlayerSnapshot Logic::capturePlayer('):record.index('LayerSnapshot Logic::captureLayer(')]
mapping = re.findall(r'shot\.(\w+) = player->(m_\w+);', capture)
player_fields = '\n'.join(f'{types[name]} {member}{{}};' for name, member in mapping)
assignments, assertions = [], []
for i, (name, kind) in enumerate(types.items(), 1):
    if name == 'present': continue
    val = '{12.f, -7.f}' if kind == 'cocos2d::CCPoint' else 'nullptr' if kind == 'void*' else ('true' if i % 2 else 'false') if kind == 'bool' else f'static_cast<{kind}>({i}*.125)'
    assignments.append(f'saved.{name} = {val};')
    # internalPosition is the stored m_position. A restore sets it to the restored node position
    # instead (GD moves the node onto it at the next update), checked separately below.
    if name not in ('holdingLeft', 'holdingRight', 'internalPosition'):
        assertions.append(f'assert(actual.{name} == saved.{name});')
side = play[play.index('\t\tauto liveSide ='):play.index('\n\t\tauto live =', play.index('\t\tauto liveSide ='))]
identity = play[play.index('void Logic::syncCheckpoints('):play.index('void Logic::onComplete(')]
fixture = r'''
#include <algorithm>
#include <cassert>
#include <iostream>
#include <ranges>
#include <vector>
#include <unordered_map>
namespace cocos2d {
struct CCPoint { float x=0, y=0; bool operator==(CCPoint const&) const = default; };
}
struct PlayerObject {
// PLAYER_FIELDS
cocos2d::CCPoint position;
float rotation=0, sx=1, sy=1;
int art=0, transitions=0;
bool upsideArt=false;
float getPositionX() { return position.x; }
float getPositionY() { return position.y; }
float getRotation() { return rotation; }
float getScaleX() { return sx; }
float getScaleY() { return sy; }
void setPosition(cocos2d::CCPoint p) { position=p; }
void setRotation(float r) { rotation=r; }
void setScaleX(float v) { sx=v; }
void setScaleY(float v) { sy=v; }
void perturb() { m_yVelocity=-100; m_rotationSpeed=-99; m_gravity=-98; rotation=87; ++transitions; }
void setMode(bool& flag, bool on, bool quiet, int mode) {
 assert(quiet); flag=on; art=on ? mode : 0; perturb();
}
void toggleFlyMode(bool b,bool q) { setMode(m_isShip,b,q,1); }
void toggleBirdMode(bool b,bool q) { setMode(m_isBird,b,q,2); }
void toggleRollMode(bool b,bool q) { setMode(m_isBall,b,q,3); }
void toggleDartMode(bool b,bool q) { setMode(m_isDart,b,q,4); }
void toggleRobotMode(bool b,bool q) { setMode(m_isRobot,b,q,5); }
void toggleSpiderMode(bool b,bool q) { setMode(m_isSpider,b,q,6); }
void toggleSwingMode(bool b,bool q) { setMode(m_isSwing,b,q,7); }
void flipGravity(bool b,bool q) { assert(q); m_isUpsideDown=b; upsideArt=b; perturb(); }
void togglePlayerScale(bool b,bool q) { assert(q); m_vehicleSize=b?.6f:1.f; sx=sy=m_vehicleSize; perturb(); }
};
// SNAPSHOT
struct CheckpointObject {};
struct Checkpoint { CheckpointObject* object; int frame; };
struct Array {
 std::vector<CheckpointObject*> items;
 bool containsObject(CheckpointObject* p) { return std::ranges::find(items,p)!=items.end(); }
};
struct PlayLayer {
 bool m_isPracticeMode=true;
 Array array;
 Array* m_checkpointArray=&array;
 CheckpointObject* getLastCheckpoint() { return array.items.empty()?nullptr:array.items.back(); }
};
struct Logic {
 std::vector<Checkpoint> checkpoints;
 CheckpointObject* restoredCheckpoint=nullptr;
 bool suppressAutoHold=false, restartHoldCaptured=false, fixTpsRotation=false;
 bool backstepping=false; // A backstep loads its save through the practice path.
 static Logic& get() { static Logic l; return l; }
 PlayerSnapshot capturePlayer(PlayerObject*);
 void applyPlayer(PlayerObject*,PlayerSnapshot const&);
 void syncCheckpoints(PlayLayer*);
 Checkpoint const* savedCheckpoint(PlayLayer*);
};
struct Trajectory {
 static Trajectory& get() { static Trajectory value; return value; }
 bool isFake(PlayerObject*) { return false; }
};
// ROTATION
// PRODUCTION
int main() {
 Logic& logic=Logic::get();
 assert(!logic.capturePlayer(nullptr).present);
 for(int from=0;from<8;++from) for(int to=0;to<8;++to)
 for(bool upside:{false,true}) {
  PlayerObject player;
  bool* flags[]={&player.m_isShip,&player.m_isBird,&player.m_isBall,&player.m_isDart,&player.m_isRobot,&player.m_isSpider,&player.m_isSwing};
  if(from) *flags[from-1]=true;
  player.art=from;
  PlayerSnapshot saved;
  // SAVED_VALUES
  saved.ship=to==1; saved.bird=to==2; saved.ball=to==3;
  saved.dart=to==4; saved.robot=to==5; saved.spider=to==6; saved.swing=to==7;
  saved.upsideDown=upside; saved.vehicleSize=.6f; saved.scaleX=-.6f; saved.scaleY=.6f;
  saved.present=true; saved.visualRotation=saved.rotation;
  logic.applyPlayer(&player,saved);
  auto actual=logic.capturePlayer(&player);
  // ASSERT_VALUES
  assert(player.art==to && player.upsideArt==upside);
  assert(player.m_position == cocos2d::CCPoint(saved.x, saved.y)); // Not the trailing saved m_position.
  assert(player.m_lastPosition == saved.lastPosition);
  auto transitions=player.transitions;
  logic.applyPlayer(&player,saved);
  assert(player.transitions==transitions); // Already matching state must not retrigger native effects.
 }
 // Fix Rotation Bug presents a different angle between physics frames.
 // Save both angles even if the checkpoint is created while the visual angle is shown.
 logic.fixTpsRotation=true;
 for(int vehicle=0;vehicle<4;++vehicle) {
  clearTilt();
  PlayerObject ship;
  bool* modes[]={&ship.m_isShip,&ship.m_isBird,&ship.m_isDart,&ship.m_isSwing};
  *modes[vehicle]=true;
  ship.setRotation(12);
  tiltFix()[&ship]=30; tiltLeft()[&ship]=12;
  presentTilt(&ship);
  assert(ship.getRotation()==30);
  auto saved=logic.capturePlayer(&ship);
  assert(saved.rotation==12 && saved.visualRotation==30);
  for(int attempt=0;attempt<10;++attempt) {
   // The failed attempt leaves a pending gameplay angle and a different visual cache.
   ship.setRotation(-80);
   tiltFix()[&ship]=-80; tiltLeft()[&ship]=-60; tiltHeld()[&ship]=-60;
   logic.applyPlayer(&ship,saved);
   assert(ship.getRotation()==12 && !tiltHeld().contains(&ship));
   restoreTilt(&ship); // Beginning of the next scheduler frame.
   assert(ship.getRotation()==12);
   presentTilt(&ship); // Drawing must use the checkpoint's visual angle.
   assert(ship.getRotation()==30);
   auto actual=logic.capturePlayer(&ship);
   assert(actual.rotation==12 && actual.visualRotation==30);
   restoreTilt(&ship);
   turnBoth(&ship,[&]{ship.setRotation(13);},[&]{ship.setRotation(31);});
   assert(ship.getRotation()==13 && visualTilt(&ship)==31);
  }
  logic.fixTpsRotation=false;
  logic.applyPlayer(&ship,saved);
  assert(ship.getRotation()==12 && !tiltFix().contains(&ship) && !tiltHeld().contains(&ship));
  logic.fixTpsRotation=true;
 }
 clearTilt();
 logic.fixTpsRotation=false;
 // Post-checkpoint jumps/orbs must not survive any later respawn.
 PlayerObject respawn;
 respawn.m_yVelocity=3;
 respawn.m_isAccelerating=false;
 respawn.m_ringJumpRelated=false;
 respawn.m_touchedRing=false;
 respawn.m_touchedCustomRing=false;
 respawn.m_touchedPad=false;
 respawn.m_wasRobotJump=false;
 respawn.m_blackOrbRelated=0;
 auto checkpoint=logic.capturePlayer(&respawn);
 for(int attempt=0;attempt<10;++attempt) {
  respawn.m_yVelocity=-15;
  respawn.m_isAccelerating=true;
  respawn.m_ringJumpRelated=true;
  respawn.m_touchedRing=true;
  respawn.m_touchedCustomRing=true;
  respawn.m_touchedPad=true;
  respawn.m_wasRobotJump=true;
  respawn.m_blackOrbRelated=-15;
  logic.applyPlayer(&respawn,checkpoint);
  assert(respawn.m_yVelocity==3 && !respawn.m_isAccelerating);
  assert(!respawn.m_ringJumpRelated && !respawn.m_touchedRing);
  assert(!respawn.m_touchedCustomRing && !respawn.m_touchedPad);
  assert(!respawn.m_wasRobotJump && respawn.m_blackOrbRelated==0);
 }
 auto& value=logic;
 // LIVE_SIDE
 bool key=false, physical=true;
#ifdef GEODE_IS_MOBILE
 assert(liveSide(false,false,key,physical,false)); // Touch hold survives without a keyboard key.
 assert(physical);
 physical=false;
 assert(!liveSide(false,false,key,physical,false)); // Real release remains released.
#else
 assert(!liveSide(false,false,key,physical,false)); // Desktop stale hold is cleared by hardware polling.
 assert(!physical);
#endif
 value.suppressAutoHold=true;
 physical=true;
 assert(!liveSide(false,true,key,physical,true)); // Full restart suppression still wins.
 PlayLayer layer;
 CheckpointObject a,b,c,unknown;
 layer.array.items={&a,&b,&c};
 logic.checkpoints={{&a,10},{&b,20},{&c,30}};
 assert(logic.savedCheckpoint(&layer)->frame==30);
 layer.array.items.erase(layer.array.items.begin()); // Oldest evicted at cap.
 logic.syncCheckpoints(&layer);
 assert(logic.checkpoints.size()==2 && logic.savedCheckpoint(&layer)->frame==30);
 logic.restoredCheckpoint=&b; // Restore the selected checkpoint, not the last cached snapshot.
 assert(logic.savedCheckpoint(&layer)->frame==20);
 layer.array.items.pop_back(); // Undo newest checkpoint.
 logic.syncCheckpoints(&layer);
 assert(logic.checkpoints.size()==1 && logic.savedCheckpoint(&layer)->frame==20);
 layer.array.items.push_back(&unknown);
 logic.restoredCheckpoint=nullptr;
 assert(!logic.savedCheckpoint(&layer)); // Never overwrite an unknown native checkpoint with a stale one.
 layer.array.items.clear();
 logic.restoredCheckpoint=&b;
 logic.syncCheckpoints(&layer);
 assert(logic.checkpoints.empty() && !logic.restoredCheckpoint && !logic.savedCheckpoint(&layer));
 std::cout << "128 mode/gravity restores, 40 visual/gameplay rotation restores, snapshot round trips and checkpoint identity passed\n";
}
'''
fixture = fixture.replace('// PLAYER_FIELDS', player_fields).replace('// SNAPSHOT', shot)
fixture = fixture.replace('// ROTATION', rotation).replace('// PRODUCTION', capture + identity).replace('// LIVE_SIDE',side)
fixture = fixture.replace('// SAVED_VALUES', '\n'.join(assignments)).replace('// ASSERT_VALUES', '\n'.join(assertions))
with tempfile.TemporaryDirectory(prefix='echo-checkpoints-') as directory:
    cpp, binary = Path(directory)/'test.cpp', Path(directory)/'test'
    cpp.write_text(fixture)
    for profile in ([], ['-DGEODE_IS_MOBILE']):
        subprocess.run(['c++','-std=c++23','-O1','-g','-Wall','-Wextra','-Werror',
                        '-Wno-unused-parameter','-fsanitize=address,undefined',*profile,
                        str(cpp),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
