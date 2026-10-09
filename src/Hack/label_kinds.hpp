#pragma once

#include "Logic/logic.hpp"

#include <span>
#include <string>
#include <vector>

// Every in-game label: its menu title, settings key, toggle, placement and node tag.
struct LabelInfo {
	char const* title;
	char const* key;
	bool Session::* shown;
	LabelPlacement Session::* place;
	int tag;
};

std::span<LabelInfo const> labelInfos();
LabelInfo const* labelInfo(std::string const& key);

// The labels top to bottom for Auto Position: the saved order, then any label it does not name.
std::vector<LabelInfo const*> orderedLabels(Session const& value);
// Moves a label one place up (-1) or down (+1) in that order.
void moveLabel(Session& value, LabelInfo const& label, int delta);

// The game's bitmap fonts, as menu names and .fnt files.
std::vector<std::string> const& labelFontNames();
std::string labelFontFile(int index);
int labelFontIndex(std::string const& file);

inline constexpr char const* kLabelCorners[] = {"Top Left", "Top Right", "Bottom Left", "Bottom Right"};
