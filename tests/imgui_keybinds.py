"""Check missing-key event forwarding and round trips using actual SDK/ImGui enums."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
cache = (root/'build-win/CMakeCache.txt').read_text()
imgui_path = re.search(r'^CPM_PACKAGE_imgui_SOURCE_DIR:INTERNAL=(.+)$', cache, re.M)[1]
imgui = (Path(imgui_path)/'imgui.h').read_text()
start = imgui.index('enum ImGuiKey : int\n{')
imgui_enum = imgui[start:imgui.index('\n};', start)+3]
sdk = Path(os.environ.get('GEODE_SDK', root.parent/'Geode'))
cocos = (sdk/'loader/include/Geode/cocos/robtop/keyboard_dispatcher/CCKeyboardDelegate.h').read_text()
start = cocos.index('typedef enum')
cocos_enum = cocos[start:cocos.index('} enumKeyCodes;', start)+len('} enumKeyCodes;')]
source = (root/'src/GUI/keyboard.cpp').read_text()
bridge = source[source.index('ImGuiKey extraImGuiKey('):source.index('\n}\n\n$execute')]
theme = (root/'src/GUI/theme.cpp').read_text()
reverse = theme[theme.index('std::optional<cocos2d::enumKeyCodes> GUI::cocosKey('):theme.index('\nbool GUI::isModifier(')]
fixture = r'''
#include <cassert>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>
// IMGUI_ENUM
namespace cocos2d {
// COCOS_ENUM
}
using namespace cocos2d;
struct GUI { std::optional<enumKeyCodes> cocosKey(ImGuiKey); };
enum class ListenerResult { Propagate, Stop };
struct KeyboardInputData {
 enum class Action { Press, Release, Repeat };
 enumKeyCodes key;
 Action action;
};
struct ImGuiCocos {
 enum class InputMode { Blocking, Default };
 bool initialized=true,visible=true;
 InputMode mode=InputMode::Blocking;
 static ImGuiCocos& get() { static ImGuiCocos backend; return backend; }
 bool isInitialized() { return initialized; }
 bool isVisible() { return visible; }
 InputMode getInputMode() { return mode; }
};
namespace ImGui {
struct IO {
 bool WantCaptureKeyboard=false;
 std::vector<std::pair<ImGuiKey,bool>> events;
 void AddKeyEvent(ImGuiKey key,bool down) { events.emplace_back(key,down); }
};
IO& GetIO() { static IO io; return io; }
}
// BRIDGE
// REVERSE
int main() {
 GUI gui;
 std::vector<enumKeyCodes> keys;
 for(int k=KEY_F1;k<=KEY_F24;++k) keys.push_back(static_cast<enumKeyCodes>(k));
 for(int k=KEY_NumPad0;k<=KEY_NumPad9;++k) keys.push_back(static_cast<enumKeyCodes>(k));
 for(auto k:{KEY_Decimal,KEY_Divide,KEY_Multiply,KEY_Subtract,KEY_Add,KEY_Numlock}) keys.push_back(k);
 auto& backend=ImGuiCocos::get(); auto& io=ImGui::GetIO();
 for(auto key:keys) {
  io.events.clear();
  auto imKey=extraImGuiKey(key);
  assert(imKey!=ImGuiKey_None && imKey<ImGuiKey_MouseLeft);
  assert(gui.cocosKey(imKey)==key);
  for(auto action:{KeyboardInputData::Action::Press,KeyboardInputData::Action::Repeat,KeyboardInputData::Action::Release}) {
   KeyboardInputData event{key,action};
   assert(feedExtraKey(event)==ListenerResult::Propagate);
  }
  assert(io.events.size()==3 && io.events[0].second && io.events[1].second && !io.events[2].second);
 }
 // Ordinary keys/modifiers remain exclusively handled by the backend.
 io.events.clear();
 for(auto key:{KEY_A,KEY_Zero,KEY_LeftControl,KEY_Enter,KEY_Escape}) {
  KeyboardInputData event{key,KeyboardInputData::Action::Press};feedExtraKey(event);
 }
 assert(io.events.empty());
 backend.visible=false;
 KeyboardInputData event{KEY_F1,KeyboardInputData::Action::Press};feedExtraKey(event);
 assert(io.events.empty()); // Do not inject gameplay hotkeys into hidden ImGui.
 event.action=KeyboardInputData::Action::Release;feedExtraKey(event);
 assert(io.events.size()==1 && !io.events.back().second); // Always clear a held key.
 io.events.clear();io.WantCaptureKeyboard=true;
 event.action=KeyboardInputData::Action::Press;feedExtraKey(event);
 assert(io.events.size()==1);
 io.events.clear();backend.initialized=false;feedExtraKey(event);
 assert(io.events.empty());
 std::cout<<"40 function/numpad key round trips, press/repeat/release forwarding, capture gating and ordinary-key deduplication passed.\n";
}
'''
fixture = fixture.replace('// IMGUI_ENUM',imgui_enum).replace('// COCOS_ENUM',cocos_enum).replace('// BRIDGE',bridge).replace('// REVERSE',reverse)
with tempfile.TemporaryDirectory(prefix='echo-imgui-keybinds-') as directory:
    cpp = Path(directory)/'test.cpp'
    binary = Path(directory)/'test'
    cpp.write_text(fixture)
    subprocess.run(['c++','-std=c++23','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
