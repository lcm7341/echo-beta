#pragma once

#include "Logic/logic.hpp"

#include <string>
#include <vector>

namespace ScriptLabels {
	struct Look {
		LabelPlacement place{ 3.f, 100.f, 0.4f, 70 };
		cocos2d::ccColor3B color{ 255, 255, 255 };
		bool placed = false;
	};

	bool set(int owner, const std::string& name, const std::string& text, const Look* look);
	bool remove(int owner, const std::string& name);
	void clear(int owner);
	void clear_all();
	bool set_override(int owner, const std::string& key, const std::string* text);
	const std::string* override_text(int tag);
	void sync(PlayLayer* play, bool shown);
	void set_running_scripts(const std::vector<std::string>& names);
}

#define SCRIPTS_TAG 60452
#define SCRIPT_FIRST_TAG 60453
#define SCRIPT_LAST_TAG 60500
