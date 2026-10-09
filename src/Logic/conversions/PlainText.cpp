#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <Geode/utils/async.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>




using namespace geode::prelude;


bool ReplayIO::readPlain(std::filesystem::path const& path) {
	std::ifstream file(path);
	if (!file) return false;
	auto& value = Logic::get();
	file >> value.fps;
	if (!file) return false;
	value.inputs.clear();
	value.clearSides();
	value.touchInputs();
	Logic::get().refreshLiveHold();
	while (file) {
		Frame input;
		int holding = 0;
		int player2 = 0;
		file >> input.number >> holding >> player2;
		if (!file) break;
		input.holding = holding != 0;
		input.player2 = player2 != 0;
		value.inputs.push_back(input);
	}
	return true;
}
bool ReplayIO::writePlain(std::filesystem::path const& path) {
	std::ofstream file(path);
	if (!file) return false;
	auto const& value = Logic::get();
	file << (value).sampleRate() << "\n";
	for (auto const& input : value.inputs) {
		file << input.number << " " << (input.holding ? 1 : 0) << " " << (input.player2 ? 1 : 0) << "\n";
	}
	return true;
}

