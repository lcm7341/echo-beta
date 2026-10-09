#include "GUI/gui.hpp"
#include <imgui-cocos.hpp>
#include <algorithm>
#include <cmath>


using namespace geode::prelude;

void GUI::rewriteSelected(std::function<void(Frame&)> const& edit) {
	auto& inputs = Logic::get().inputs;
	int first = 0;
	int last = 0;
	if (!selectionSpan(first, last)) return;
	std::ranges::for_each(inputs.begin() + first, inputs.begin() + last + 1, edit);
	if (selected >= first && selected <= last) draft = inputs[static_cast<size_t>(selected)];
	if (selected >= 0) selectedId = Logic::get().inputId(static_cast<size_t>(selected));
	Logic::get().touchInputs();
	Logic::get().refreshLiveHold();
	Logic::get().autosaveReplay();
}

void GUI::moveSelected(int dir) {
	auto& inputs = Logic::get().inputs;
	int first = 0;
	int last = 0;
	if (!selectionSpan(first, last)) return;
	int n = static_cast<int>(inputs.size());
	if (dir < 0 && first <= 0) return;
	if (dir > 0 && last + 1 >= n) return;
	auto rotate = [&](auto& side) {
		if (static_cast<int>(side.size()) != n) return;
		auto at = side.begin();
		if (dir < 0) std::rotate(at + first - 1, at + first, at + last + 1);
		else std::rotate(at + first, at + last + 1, at + last + 2);
	};
	rotate(inputs);
	rotate(Logic::get().poses);
	rotate(Logic::get().inputIds);
	selected += dir;
	if (anchor >= 0) anchor += dir;
	selectedId = Logic::get().inputId(static_cast<size_t>(selected));
	if (anchor >= 0) anchorId = Logic::get().inputId(static_cast<size_t>(anchor));
	syncDraft();
	Logic::get().touchInputs();
	Logic::get().refreshLiveHold();
	Logic::get().autosaveReplay();
}

void GUI::addInput(int frame) {
	auto& value = Logic::get();
	auto& inputs = value.inputs;
	Frame created;
	uint64_t createdId = Logic::get().mintInputId();
	created.number = frame;
	created.holding = true;
	created.player2 = !showEditorP1 && showEditorP2;
	int index = selected;
	auto insertSide = [&](auto& side, auto empty, bool optional) {
		if (side.empty() && optional) return;
		if (side.size() + 1 != inputs.size() && side.size() != inputs.size()) {
			side.clear();
			return;
		}
		if (side.size() < inputs.size()) {
			if (index >= static_cast<int>(side.size())) side.push_back(empty);
			else side.insert(side.begin() + index, empty);
		}
	};
	if (index < 0 || index >= static_cast<int>(inputs.size())) {
		inputs.push_back(created);
		index = static_cast<int>(inputs.size()) - 1;
	} else {
		inputs.insert(inputs.begin() + index, created);
		Logic::get().touchInputs();
	}
	insertSide(value.poses, FramePose{}, true);
	insertSide(value.inputIds, uint64_t{0}, false);
	selectInput(index);
	Logic::get().setInputId(static_cast<size_t>(index), createdId);
	Logic::get().refreshLiveHold();
	plain = false;
	Logic::get().autosaveReplay();
}

void GUI::deleteSelected() {
	auto& value = Logic::get();
	auto& inputs = value.inputs;
	int first = 0;
	int last = 0;
	if (!selectionSpan(first, last)) return;
	auto eraseSpan = [&](auto& side) {
		if (side.size() != inputs.size()) {
			side.clear();
			return;
		}
		side.erase(side.begin() + first, side.begin() + last + 1);
	};
	eraseSpan(value.poses);
	eraseSpan(value.inputIds);
	inputs.erase(inputs.begin() + first, inputs.begin() + last + 1);
	Logic::get().touchInputs();
	Logic::get().refreshLiveHold();
	selectInput(std::min(first, static_cast<int>(inputs.size()) - 1));
	Logic::get().autosaveReplay();
}

void GUI::resetMacro() {
	auto& value = Logic::get();
	Logic::get().clearInputs();
	value.recordingSeconds = 0.0;
	value.attempts = 0;
	selectInput(-1);
	clearCpsMark();
	showEditorP1 = true;
	showEditorP2 = true;
}

void GUI::offsetChosen(int delta) {
	int first = 0;
	int last = 0;
	if (hasRange() && selectionSpan(first, last)) Logic::get().offsetInputs(delta, first, last);
	else Logic::get().offsetInputs(delta);
	syncDraft();
}

void GUI::randomOffsetChosen() {
	int first = 0;
	int last = 0;
	if (hasRange() && selectionSpan(first, last)) Logic::get().randomOffsetInputs(inputOffset, first, last);
	else Logic::get().randomOffsetInputs(inputOffset);
	syncDraft();
}

void GUI::setFrameAdvance(bool on) {
	auto& value = Logic::get();
	value.frameAdvance = on;
	persist();
	value.advanceQueued = false;
	value.holdingAdvance = false;
	if (value.frameAdvance) Logic::get().disarmLengthPauseIfHolding();
	value.message = value.frameAdvance ? "Frame advance on" : "Frame advance off";
	Logic::get().refreshPopup();
}

void GUI::applySeedOverride() {
	auto& value = Logic::get();
	if (!value.seedOverride) return;
	value.levelSeed = value.seedOverrideValue;
	value.levelSeedKnown = true;
	value.levelSeedFromFile = false;
}

void GUI::setSpeed(float speed, bool settle) {
	auto& value = Logic::get();
	value.speedhack = std::clamp(speed, 0.01f, 100.f);
	bool normal = std::abs(value.speedhack - 1.f) < .00001f;
	if (!normal) value.speedhackMemory = value.speedhack;
	Logic::get().syncTimeScale();
	Logic::get().syncPlaybackFps();
	if (settle) persist();
}

void GUI::setPlaybackFps(float fps) {
	Logic::get().playbackFps = std::max(fps, 1.f);
	persist();
	Logic::get().syncPlaybackFps();
}

void GUI::dropCarry() {
	Logic::get().carry = 0.f;
	if (auto* play = PlayLayer::get()) play->m_extraDelta = 0.0;
}

void GUI::applyNoMirror() {
	// Nothing to set: the option only changes drawing (Rendering/no_mirror.cpp). The flip progress
	// is gameplay state and stays as GD has it.
}

std::string GUI::renderBlocker() {
#if defined(GEODE_IS_IOS)
	return "Rendering isn't supported on iOS.";
#else
	if (Logic::get().ffmpegReady()) return {};
#ifdef GEODE_IS_WINDOWS
#if ECHO_FFMPEG_API_ENABLED
	return "ffmpeg.exe was not found. Place it next to Geometry Dash, or install the FFmpeg API mod (eclipse.ffmpeg-api).";
#else
	return "ffmpeg.exe was not found. Place it next to Geometry Dash.";
#endif
#else
#if ECHO_FFMPEG_API_ENABLED
	return "Rendering needs the FFmpeg API mod (eclipse.ffmpeg-api). Install it from the Geode mod index.";
#else
	return "Rendering is currently available on Windows only.";
#endif
#endif
#endif
}

bool GUI::twoPlayerDual() {
	auto* live = PlayLayer::get();
	if (!live) return false;
	bool two = live->m_levelSettings && live->m_levelSettings->m_twoPlayerMode;
	return two && live->m_gameState.m_isDualMode;
}
