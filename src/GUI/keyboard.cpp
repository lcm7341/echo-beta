#include <imgui-cocos.hpp>
#include <imgui.h>
#include <Geode/Geode.hpp>
#include <Geode/utils/Keyboard.hpp>
#include "GUI/gui.hpp"

using namespace geode::prelude;

namespace {
    ImGuiKey extra_im_gui_key(enumKeyCodes key) {
        if (key >= KEY_F1 && key <= KEY_F24)
            return static_cast<ImGuiKey>(ImGuiKey_F1 + (key - KEY_F1));
        if (key >= KEY_NumPad0 && key <= KEY_NumPad9)
            return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (key - KEY_NumPad0));
        switch (key) {
        case KEY_Decimal:
            return ImGuiKey_KeypadDecimal;
        case KEY_Divide:
            return ImGuiKey_KeypadDivide;
        case KEY_Multiply:
            return ImGuiKey_KeypadMultiply;
        case KEY_Subtract:
            return ImGuiKey_KeypadSubtract;
        case KEY_Add:
            return ImGuiKey_KeypadAdd;
        case KEY_Numlock:
            return ImGuiKey_NumLock;
        default:
            return ImGuiKey_None;
        }
    }

    ListenerResult feed_extra_key(KeyboardInputData& event) {
        auto& backend = ImGuiCocos::get();
        if (!backend.isInitialized())
            return ListenerResult::Propagate;
        auto key = extra_im_gui_key(event.key);
        if (key == ImGuiKey_None)
            return ListenerResult::Propagate;
        auto& io = ImGui::GetIO();
        bool down = event.action != KeyboardInputData::Action::Release;
        bool blocking = backend.isVisible() && backend.getInputMode() == ImGuiCocos::InputMode::Blocking;
        if (io.WantCaptureKeyboard || blocking || !down)
            io.AddKeyEvent(key, down);
        return ListenerResult::Propagate;
    }

#ifdef GEODE_IS_MACOS
    bool types_text(enumKeyCodes key) {
        if (key >= KEY_Zero && key <= KEY_Nine)
            return true;
        if (key >= KEY_A && key <= KEY_Z)
            return true;
        if (key >= KEY_NumPad0 && key <= KEY_Divide)
            return true;
        if (key >= KEY_OEM1 && key <= KEY_OEM102)
            return true;
        if (key >= KEY_GraveAccent && key <= KEY_Equal)
            return true;
        return key == KEY_Space;
    }

    // fix priority
    ListenerResult shield_typing(KeyboardInputData& event) {
        auto& backend = ImGuiCocos::get();
        if (!backend.isInitialized() || !backend.isVisible() || !ImGui::GetIO().WantTextInput)
            return ListenerResult::Propagate;
        if (event.action == KeyboardInputData::Action::Release)
            return ListenerResult::Propagate;
        if (static_cast<bool>(event.modifiers & KeyboardModifier::Control) || static_cast<bool>(event.modifiers & KeyboardModifier::Super))
            return ListenerResult::Propagate;
        if (types_text(event.key))
            event.key = KEY_Unknown;
        return ListenerResult::Propagate;
    }

    ListenerResult capture_bind(KeyboardInputData& event) {
        auto& gui = GUI::get();
        if (gui.prompt.empty() || gui.prompt_shown != gui.prompt)
            return ListenerResult::Propagate;
        auto& backend = ImGuiCocos::get();
        if (!backend.isInitialized() || !backend.isVisible() || ImGui::GetFrameCount() - gui.prompt_drawn_frame > 2)
            return ListenerResult::Propagate;
        if (event.key == KEY_None)
            return ListenerResult::Propagate;
        bool modifier = gui.modifier_for_key(event.key) != KeyboardModifier::None;
        if (event.action == KeyboardInputData::Action::Release) {
            if (modifier && !gui.pressed) {
                auto own = gui.modifier_for_key(event.key);
                gui.pressed = event.key;
                gui.bind_ctrl = static_cast<bool>(own & KeyboardModifier::Control);
                gui.bind_shift = static_cast<bool>(own & KeyboardModifier::Shift);
                gui.bind_alt = static_cast<bool>(own & KeyboardModifier::Alt);
                gui.bind_super = static_cast<bool>(own & KeyboardModifier::Super);
            }
            return ListenerResult::Propagate;
        }
        if (!modifier && event.action == KeyboardInputData::Action::Press) {
            gui.pressed = event.key;
            gui.bind_ctrl = static_cast<bool>(event.modifiers & KeyboardModifier::Control);
            gui.bind_shift = static_cast<bool>(event.modifiers & KeyboardModifier::Shift);
            gui.bind_alt = static_cast<bool>(event.modifiers & KeyboardModifier::Alt);
            gui.bind_super = static_cast<bool>(event.modifiers & KeyboardModifier::Super);
        }
        return ListenerResult::Stop;
    }
#endif
}

$execute {
    KeyboardInputEvent().listen(&feed_extra_key, -100).leak();
#ifdef GEODE_IS_MACOS
    KeyboardInputEvent().listen(&capture_bind, -200).leak();
    KeyboardInputEvent().listen(&shield_typing, -300).leak();
#endif
}
