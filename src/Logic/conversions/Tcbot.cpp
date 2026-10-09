#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>



using namespace geode::prelude;


class TcbotReplay {
public:
	static constexpr std::array<uint8_t, 16> header = {
		0x9f, 0x88, 0x89, 0x84, 0x9f, 0x3b, 0x1d, 0xd8, 0xcc, 0xa1, 0x86, 0x8a, 0x88, 0x99, 0x84, 0x00,
	};
	static constexpr size_t metaSize = 64;
	static constexpr uint8_t tpsFlag = 1 << 1;
	static constexpr uint8_t endOfInputs = 0xCC;

	enum class Blob { Action, Delta, Tps, Seed };

	std::vector<Frame> inputs;

	void push(uint64_t frame, int button, bool holding, bool player2) {
		if (frame > static_cast<uint64_t>(std::numeric_limits<int>::max())) return;
		Frame input;
		input.number = static_cast<int>(frame);
		input.button = button;
		input.holding = holding;
		input.player2 = player2;
		inputs.push_back(input);
	}

	bool readV1(ByteCursor& in) {
		auto count = in.leb128();
		for (uint64_t i = 0; i < count && !in.failed; ++i) {
			auto frame = 1 + in.leb128();
			auto data = static_cast<uint8_t>(in.uint(1));
			if (in.failed) return false;
			int input = data & 0b111;
			if (input < 3) push(frame, input + 1, data & 0x80, data & 0x40);
			else if (input <= 5) inputs.clear();
			else return false;
		}
		return !in.failed;
	}

	static uint64_t readDelta(ByteCursor& in, uint8_t info, uint64_t& lastDelta) {
		bool magic = info & 1;
		int blob = info >> 1;
		uint64_t base = magic ? lastDelta : 0;
		if (blob == 0) return base;
		size_t width = blob == 3 ? 4 : static_cast<size_t>(blob);
		uint64_t result = base + in.uint(width);
		if (result != 0) lastDelta = result;
		return result;
	}
	bool readV2(ByteCursor& in) {
		uint64_t frame = 1 + in.leb128();
		if (in.failed) return false;
		uint64_t lastDelta = 0;
		uint8_t deltaInfo = 0;
		auto next = Blob::Action;
		for (;;) {
			switch (next) {
				case Blob::Action: {
					if (in.left() == 0) return true;
					auto byte = static_cast<uint8_t>(in.uint(1));
					deltaInfo = byte >> 5;
					int button = byte & 0b11;
					bool extra = byte & 0x10; 
					if (button > 0) {
						bool holding = byte & 0x04;
						bool player2 = byte & 0x08;
						push(frame, button, holding, player2);
						if (extra) push(frame, button, !holding, player2);
						next = Blob::Delta;
						break;
					}
					int custom = (byte >> 2) & 0b11;
					if (custom == 3) {
						next = extra ? Blob::Delta : Blob::Tps;
						break;
					}
					inputs.clear();
					frame = 0;
					next = extra ? Blob::Seed : Blob::Delta;
					break;
				}
				case Blob::Delta:
					frame += readDelta(in, deltaInfo, lastDelta);
					if (in.failed) return true;
					next = Blob::Action;
					break;
				case Blob::Tps:
					in.uint(4);
					if (in.failed) return true;
					next = Blob::Delta;
					break;
				case Blob::Seed:
					in.uint(8);
					if (in.failed) return true;
					next = Blob::Delta;
					break;
			}
		}
	}

	static uint8_t deltaInfo(uint64_t delta, uint64_t lastDelta, uint64_t& written) {
		constexpr std::array<uint64_t, 4> limits = {0, 0xff, 0xffff, 0xffffffff};
		for (uint8_t blob = 0; blob < 4; ++blob) {
			if (delta <= limits[blob]) {
				written = delta;
				return static_cast<uint8_t>(blob << 1);
			}
			if (lastDelta != 0 && lastDelta <= delta && delta - lastDelta <= limits[blob]) {
				written = delta - lastDelta;
				return static_cast<uint8_t>(blob << 1 | 1);
			}
		}
		written = std::min<uint64_t>(delta, limits[3]);
		return 3 << 1;
	}
	static void writeV2(ByteSink& out, std::vector<Frame> const& inputs) {
		out.leb128(inputs.empty() ? 0 : static_cast<uint64_t>(inputs.front().number));
		uint64_t lastDelta = 0;
		for (size_t i = 0; i < inputs.size(); ++i) {
			auto const& input = inputs[i];
			bool swift = i + 1 < inputs.size() && inputs[i + 1].number == input.number && inputs[i + 1].button == input.button &&
				inputs[i + 1].holding != input.holding && inputs[i + 1].player2 == input.player2;
			size_t following = swift ? i + 2 : i + 1;
			uint64_t delta = following < inputs.size() ? static_cast<uint64_t>(inputs[following].number - input.number - 1) : 0;
			uint64_t written = 0;
			uint8_t info = deltaInfo(delta, lastDelta, written);
			if (delta != 0) lastDelta = delta;
			uint8_t byte = static_cast<uint8_t>(input.button & 0b11) | uint8_t(input.holding) << 2 | uint8_t(input.player2) << 3 | uint8_t(swift) << 4 | info << 5;
			out.uint(byte, 1);
			int blob = info >> 1;
			if (blob != 0) out.uint(written, blob == 3 ? 4 : static_cast<size_t>(blob));
			if (swift) ++i;
		}
	}
};

bool ReplayIO::readTcbot(std::filesystem::path const& path) {
	ByteCursor in{ReplayIO::readAll(path)};
	if (in.data.size() < TcbotReplay::header.size() + TcbotReplay::metaSize) return false;
	if (std::memcmp(in.data.data(), TcbotReplay::header.data(), TcbotReplay::header.size()) != 0) return false;
	size_t meta = TcbotReplay::header.size();
	uint8_t version = in.data[meta];
	uint8_t flags = in.data[meta + 2];
	float rate = 0.f;
	std::memcpy(&rate, in.data.data() + meta + 4, sizeof(rate));
	float tps = rate;
	if (version == 2 && !(flags & TcbotReplay::tpsFlag) && rate > 0.f) {
		tps = 1.f / rate;
		float whole = std::round(tps);
		if (whole > 0.f && static_cast<float>(1.0 / whole) == rate) tps = whole;
	}
	in.at = meta + TcbotReplay::metaSize;
	TcbotReplay replay;
	bool ok = version == 1 ? replay.readV1(in) : version == 2 ? replay.readV2(in) : false;
	if (!ok) return false;
	// TCBot counts two steps later than Echo. Pull every click back so it lands on the same step.
	for (auto& input : replay.inputs) input.number = std::max(input.number - 2, 0);
	ReplayIO::adoptInputs(std::move(replay.inputs), tps);
	return true;
}
bool ReplayIO::writeTcbot(std::filesystem::path const& path) {
	ByteSink out;
	out.data.assign(TcbotReplay::header.begin(), TcbotReplay::header.end());
	size_t meta = out.data.size();
	out.data.resize(meta + TcbotReplay::metaSize, 0);
	out.data[meta] = 2;
	out.data[meta + 2] = TcbotReplay::tpsFlag;
	float tps = Logic::get().fps;
	std::memcpy(out.data.data() + meta + 4, &tps, sizeof(tps));
	auto inputs = ReplayIO::sortedInputs();
	for (auto& input : inputs) {
		long long shifted = static_cast<long long>(input.number) + 2;
		if (shifted > std::numeric_limits<int>::max()) shifted = std::numeric_limits<int>::max();
		input.number = static_cast<int>(shifted);
	}
	TcbotReplay::writeV2(out, inputs);
	return out.save(path);
}
