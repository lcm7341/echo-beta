#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>



using namespace geode::prelude;


class SilicateReplay {
public:
	static constexpr char magic[] = "SLC3RPLY";
	static constexpr size_t magicSize = 8;
	static constexpr uint16_t metaSize = 64;
	static constexpr uint32_t metaVersion = 2;
	static constexpr uint32_t build = 81;
	static constexpr uint32_t actionAtom = 1;
	static constexpr uint8_t footer = 0xCC;
	static constexpr uint64_t sizeMask = 0x00FFFFFFFFFFFFFFull;
	static constexpr size_t maxSection = size_t(1) << 15;

	enum class Special : uint8_t { Restart = 0, RestartFull = 1, Death = 2, Tps = 3, Bugpoint = 4 };

	std::vector<Frame> inputs;
	std::vector<RateChange> rates;
	uint64_t frame = 0;
	uint64_t attemptStart = 0;
	uint64_t produced = 0;
	double tps = 240.0;
	double startTps = 240.0;

	void press(int button, bool holding, bool player2) {
		uint64_t local = frame - attemptStart;
		if (local > static_cast<uint64_t>(std::numeric_limits<int>::max())) return;
		Frame input;
		input.number = static_cast<int>(local);
		input.button = button;
		input.holding = holding;
		input.player2 = player2;
		inputs.push_back(input);
	}
	void state(uint64_t value) {
		frame += value >> 4;
		int button = static_cast<int>((value >> 2) & 3);
		bool player2 = (value >> 1) & 1;
		if (button == 0) {
			press(1, true, player2);
			press(1, false, player2);
			produced += 2;
		} else {
			press(button, value & 1, player2);
			produced += 1;
		}
	}
	bool section(ByteCursor& in, size_t end) {
		if (end - in.at < 2) return false;
		auto header = static_cast<uint16_t>(in.uint(2));
		switch (header >> 14) {
			case 0:
			case 1: {
				size_t width = size_t(1) << ((header >> 12) & 3);
				size_t length = size_t(1) << ((header >> 8) & 15);
				size_t repeats = (header >> 14) == 1 ? size_t(1) << ((header >> 3) & 31) : 1;
				if (end - in.at < width * length) return false;
				std::vector<uint64_t> cluster(length);
				for (auto& value : cluster) value = in.uint(width);
				for (size_t r = 0; r < repeats && !in.failed; ++r) {
					for (auto value : cluster) state(value);
				}
				return !in.failed;
			}
			case 2: {
				size_t width = size_t(1) << ((header >> 8) & 3);
				if (end - in.at < width) return false;
				frame += in.uint(width);
				auto type = static_cast<Special>((header >> 10) & 15);
				switch (type) {
					case Special::Tps: {
						if (end - in.at < 8) return false;
						auto value = in.pod<double>();
						if (!std::isfinite(value) || value < 1.0) break;
						tps = value;
						// Silicate applies the rate on the step its inputs of the same frame land, which is
						// the step after Echo's frame of that number, the same as an Echo rate change.
						uint64_t local = frame - attemptStart;
						if (frame <= attemptStart) startTps = value;
						else if (local <= static_cast<uint64_t>(std::numeric_limits<int>::max())) rates.push_back({static_cast<int>(local), static_cast<float>(value)});
						break;
					}
					case Special::Restart:
					case Special::RestartFull:
					case Special::Death:
						if (end - in.at < 8) return false;
						in.uint(8);
						inputs.clear();
						rates.clear();
						startTps = tps;
						attemptStart = frame + 1;
						break;
					case Special::Bugpoint:
						break;
					default:
						return false;
				}
				produced += 1;
				return !in.failed;
			}
			default:
				return false;
		}
	}
	bool actions(ByteCursor& in, size_t end) {
		if (end - in.at < 8) return false;
		uint64_t count = in.uint(8);
		uint64_t target = produced + count;
		while (produced < target) {
			if (in.at >= end || !section(in, end)) return false;
		}
		return true;
	}

	static size_t widthFor(uint64_t delta) {
		if (delta < (1ull << 4)) return 0;
		if (delta < (1ull << 12)) return 1;
		if (delta < (1ull << 28)) return 2;
		return 3;
	}
	static bool swiftPair(std::vector<Frame> const& inputs, size_t i) {
		if (i + 1 >= inputs.size()) return false;
		auto const& a = inputs[i];
		auto const& b = inputs[i + 1];
		return a.button == 1 && b.button == 1 && a.holding && !b.holding && a.player2 == b.player2 && a.number == b.number;
	}
	// One entry per section item: an input state, or a TPS change written as a special.
	struct Item {
		uint64_t state = 0;
		size_t width = 0;
		bool rate = false;
		uint64_t delta = 0;
		double fps = 0.0;
	};
	static size_t specialWidth(uint64_t delta) {
		if (delta <= 0xFFull) return 0;
		if (delta <= 0xFFFFull) return 1;
		if (delta <= 0xFFFFFFFFull) return 2;
		return 3;
	}
	static void writeActions(ByteSink& out, std::vector<Frame> const& inputs, std::vector<RateChange> const& rates) {
		std::vector<Item> items;
		int previous = 0;
		size_t nextRate = 0;
		auto placeRates = [&](int upTo) {
			for (; nextRate < rates.size() && rates[nextRate].frame <= upTo; ++nextRate) {
				auto const& change = rates[nextRate];
				uint64_t delta = static_cast<uint64_t>(std::max(change.frame - previous, 0));
				previous = std::max(change.frame, previous);
				items.push_back({0, specialWidth(delta), true, delta, static_cast<double>(change.fps)});
			}
		};
		for (size_t i = 0; i < inputs.size(); ++i) {
			auto const& input = inputs[i];
			placeRates(input.number);
			uint64_t delta = static_cast<uint64_t>(input.number - previous);
			previous = input.number;
			bool swift = swiftPair(inputs, i);
			uint64_t button = swift ? 0 : static_cast<uint64_t>(input.button);
			items.push_back({delta << 4 | button << 2 | uint64_t(input.player2) << 1 | uint64_t(input.holding && !swift), widthFor(delta)});
			if (swift) ++i;
		}
		placeRates(std::numeric_limits<int>::max());
		out.uint(inputs.size() + rates.size(), 8);
		size_t i = 0;
		while (i < items.size()) {
			auto const& item = items[i];
			if (item.rate) {
				out.uint(static_cast<uint16_t>(2u << 14 | static_cast<uint16_t>(Special::Tps) << 10 | item.width << 8), 2);
				out.uint(item.delta, size_t(1) << item.width);
				out.pod(item.fps);
				++i;
				continue;
			}
			size_t run = 1;
			while (i + run < items.size() && run < maxSection && !items[i + run].rate && items[i + run].width == item.width) ++run;
			auto chunk = std::bit_floor(run);
			auto exponent = static_cast<uint16_t>(std::countr_zero(chunk));
			out.uint(static_cast<uint16_t>(item.width << 12 | exponent << 8), 2);
			for (size_t j = 0; j < chunk; ++j) out.uint(items[i + j].state, size_t(1) << item.width);
			i += chunk;
		}
	}
};

bool ReplayIO::readSilicate(std::filesystem::path const& path) {
	ByteCursor in{ReplayIO::readAll(path)};
	auto& data = in.data;
	if (data.size() < SilicateReplay::magicSize + 2 + 16 + 1) return false;
	if (std::memcmp(data.data(), SilicateReplay::magic, SilicateReplay::magicSize) != 0) return false;
	in.at = SilicateReplay::magicSize;
	auto metaSize = static_cast<size_t>(in.uint(2));
	if (metaSize < 16 || in.left() < metaSize + 1) return false;
	size_t metaStart = in.at;
	SilicateReplay replay;
	replay.tps = in.pod<double>();
	if (!std::isfinite(replay.tps) || replay.tps < 1.0) replay.tps = 240.0;
	replay.startTps = replay.tps;
	// Silicate starts every attempt's random state from this seed (0 included), as Echo's level seed does.
	uint64_t seed = in.uint(8);
	in.at = metaStart + metaSize;
	size_t end = data.back() == SilicateReplay::footer ? data.size() - 1 : data.size();
	while (end - in.at >= 12) {
		auto id = static_cast<uint32_t>(in.uint(4));
		uint64_t size = in.uint(8) & SilicateReplay::sizeMask;
		if (size > end - in.at) return false;
		size_t atomEnd = size == 0 ? end : in.at + static_cast<size_t>(size);
		if (id == SilicateReplay::actionAtom && !replay.actions(in, atomEnd)) return false;
		in.at = atomEnd;
	}
	if (in.failed) return false;
	ReplayIO::adoptInputs(std::move(replay.inputs), static_cast<float>(replay.startTps));
	Logic::get().rates = std::move(replay.rates);
	Logic::get().normalizeRates();
	Logic::get().levelSeed = seed;
	Logic::get().levelSeedKnown = true;
	Logic::get().levelSeedFromFile = true;
	Logic::get().macroSilicateVariance = true;
	return true;
}
bool ReplayIO::writeSilicate(std::filesystem::path const& path) {
	auto inputs = ReplayIO::sortedInputs();
	ByteSink out;
	out.text({SilicateReplay::magic, SilicateReplay::magicSize});
	out.uint(SilicateReplay::metaSize, 2);
	size_t metaStart = out.data.size();
	out.pod(static_cast<double>(Logic::get().rateAt(0)));
	out.uint(Logic::get().levelSeedKnown ? Logic::get().levelSeed : 0, 8);
	out.uint(SilicateReplay::metaVersion, 4);
	out.uint(SilicateReplay::build, 4);
	out.uint(0, 4);
	out.data.resize(metaStart + SilicateReplay::metaSize, 0);
	out.uint(SilicateReplay::actionAtom, 4);
	size_t sizeAt = out.data.size();
	out.uint(0, 8);
	auto rates = Logic::get().rates;
	std::stable_sort(rates.begin(), rates.end(), [](RateChange const& a, RateChange const& b) { return a.frame < b.frame; });
	std::erase_if(rates, [](RateChange const& change) { return change.frame <= 0 || !std::isfinite(change.fps) || change.fps < 1.f; });
	SilicateReplay::writeActions(out, inputs, rates);
	uint64_t atomSize = out.data.size() - sizeAt - 8;
	for (size_t i = 0; i < 8; ++i) out.data[sizeAt + i] = static_cast<uint8_t>(atomSize >> (8 * i));
	out.uint(SilicateReplay::footer, 1);
	return out.save(path);
}
