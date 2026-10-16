#pragma once

#include "Logic/logic.hpp"

#include <span>
#include <string>
#include <vector>

struct LabelInfo {
	char const* title;
	char const* key;
	bool Logic::* shown;
	LabelPlacement Logic::* place;
	int tag;
};

std::span<LabelInfo const> label_infos();
LabelInfo const* label_info(std::string const& key);

std::vector<LabelInfo const*> ordered_labels(Logic const& value);
void move_label(Logic& value, LabelInfo const& label, int delta);

std::vector<std::string> const& label_font_names();
std::string label_font_file(int index);
int label_font_index(std::string const& file);

inline constexpr char const* LABEL_CORNERS[] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
