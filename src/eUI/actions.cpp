#include "GUI/gui.hpp"
#include <imgui-cocos.hpp>
#include <algorithm>
#include <cmath>

using namespace geode::prelude;

void GUI::rewrite_selected(std::function<void(Frame&)> const& edit) {
    // TODO
}

void GUI::move_selected(int dir) {
    // todo
}

void GUI::addInput(int frame) {
    // port this
}

void GUI::deleteSelected() {
    // TODO
}

void GUI::reset_macro() {
    // TODO
}

void GUI::offset_chosen(int delta) {
    // TODO
}

void GUI::random_offset_chosen() {
    // TODO
}

void GUI::set_frame_advance(bool on) {
    // todo
}

void GUI::apply_seed_override() {
    // TODO
}

void GUI::setSpeed(float speed, bool settle) {
    // TODO
}

void GUI::set_playback_fps(float fps) {
    Logic::get().playback_fps = std::max(fps, 1.f);
    persist();
    Logic::get().sync_playback_fps();
}

void GUI::drop_carry() {
    Logic::get().carry = 0.f;
    if (auto* play = PlayLayer::get())
        play->m_extraDelta = 0.0;
}

void GUI::apply_no_mirror() {
}

std::string GUI::render_blocker() {
    // later
    return {};
}

bool GUI::two_player_dual() {
    // port this
    return {};
}
