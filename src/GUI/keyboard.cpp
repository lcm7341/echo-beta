#include <imgui-cocos.hpp>
#include <imgui.h>
#include <Geode/Geode.hpp>
#include <Geode/utils/Keyboard.hpp>

using namespace geode::prelude;

namespace {
ImGuiKey extra_im_gui_key(cocos2d::enumKeyCodes key) {
    // port this
    return {};
}

ListenerResult feed_extra_key(KeyboardInputData& event) {
    // todo
    return {};
}
}

$execute {
    KeyboardInputEvent().listen(&feed_extra_key, -100).leak();
}
