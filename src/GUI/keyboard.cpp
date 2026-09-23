#include <imgui-cocos.hpp>
#include <imgui.h>
#include <Geode/Geode.hpp>
#include <Geode/utils/Keyboard.hpp>

using namespace geode::prelude;

namespace {
ImGuiKey extra_im_gui_key(cocos2d::enumKeyCodes key) {
    if (key >= KEY_F1 && key <= KEY_F24) {
        return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - KEY_F1));
    }
    if (key >= KEY_NumPad0 && key <= KEY_NumPad9) {
        return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (key - KEY_NumPad0));
    }
    switch (key) {
        case KEY_Decimal: return ImGuiKey_KeypadDecimal;
        case KEY_Divide: return ImGuiKey_KeypadDivide;
        case KEY_Multiply: return ImGuiKey_KeypadMultiply;
        case KEY_Subtract: return ImGuiKey_KeypadSubtract;
        case KEY_Add: return ImGuiKey_KeypadAdd;
        case KEY_Numlock: return ImGuiKey_NumLock;
        default: return ImGuiKey_None;
    }
}

ListenerResult feed_extra_key(KeyboardInputData& event) {
    auto& backend = ImGuiCocos::get();
    if (!backend.isInitialized()) return ListenerResult::Propagate;
    auto key = extra_im_gui_key(event.key);
    if (key == ImGuiKey_None) return ListenerResult::Propagate;
    auto& io = ImGui::GetIO();
    bool down = event.action != KeyboardInputData::Action::Release;
    bool blocking = backend.isVisible() && backend.getInputMode() == ImGuiCocos::InputMode::Blocking;
    if (io.WantCaptureKeyboard || blocking || !down) io.AddKeyEvent(key, down);
    return ListenerResult::Propagate;
}
}

$execute {
    KeyboardInputEvent().listen(&feed_extra_key, -100).leak();
}
