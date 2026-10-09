#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <Geode/Bindings.hpp>
#include <algorithm>
#include <bit>
#include <fstream>



using namespace geode::prelude;


class GdrReplay {
public:
	static constexpr float gameVersion = 2.2081f;
	static constexpr float formatVersion = 1.f;
	static constexpr int maxDepth = 64;

	static uint64_t big(ByteCursor& in, size_t width) {
		if (!in.take(width)) return 0;
		uint64_t value = 0;
		for (size_t i = 0; i < width; ++i) value = value << 8 | in.data[in.at - width + i];
		return value;
	}
	static std::string text(ByteCursor& in, size_t size) {
		if (!in.take(size)) return {};
		return {reinterpret_cast<char const*>(in.data.data() + in.at - size), size};
	}
	static matjson::Value decode(ByteCursor& in, int depth = 0) {
		if (depth > maxDepth) {
			in.failed = true;
			return nullptr;
		}
		auto type = static_cast<uint8_t>(in.uint(1));
		if (in.failed) return nullptr;
		if (type <= 0x7f) return matjson::Value(static_cast<std::intmax_t>(type));
		if (type >= 0xe0) return matjson::Value(static_cast<std::intmax_t>(static_cast<int8_t>(type)));
		if ((type & 0xf0) == 0x80) return map(in, type & 0x0f, depth);
		if ((type & 0xf0) == 0x90) return array(in, type & 0x0f, depth);
		if ((type & 0xe0) == 0xa0) return text(in, type & 0x1f);
		switch (type) {
			case 0xc0: return nullptr;
			case 0xc2: return false;
			case 0xc3: return true;
			case 0xc4: case 0xc5: case 0xc6:
				in.take(big(in, size_t(1) << (type - 0xc4)));
				return nullptr;
			case 0xc7: case 0xc8: case 0xc9: {
				auto size = big(in, size_t(1) << (type - 0xc7));
				in.take(size + 1);
				return nullptr;
			}
			case 0xca: return static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(big(in, 4))));
			case 0xcb: return std::bit_cast<double>(big(in, 8));
			case 0xcc: case 0xcd: case 0xce: case 0xcf:
				return matjson::Value(static_cast<std::uintmax_t>(big(in, size_t(1) << (type - 0xcc))));
			case 0xd0: return matjson::Value(static_cast<std::intmax_t>(static_cast<int8_t>(big(in, 1))));
			case 0xd1: return matjson::Value(static_cast<std::intmax_t>(static_cast<int16_t>(big(in, 2))));
			case 0xd2: return matjson::Value(static_cast<std::intmax_t>(static_cast<int32_t>(big(in, 4))));
			case 0xd3: return matjson::Value(static_cast<std::intmax_t>(static_cast<int64_t>(big(in, 8))));
			case 0xd4: case 0xd5: case 0xd6: case 0xd7: case 0xd8:
				in.take((size_t(1) << (type - 0xd4)) + 1);
				return nullptr;
			case 0xd9: case 0xda: case 0xdb: return text(in, big(in, size_t(1) << (type - 0xd9)));
			case 0xdc: case 0xdd: return array(in, big(in, type == 0xdc ? 2 : 4), depth);
			case 0xde: case 0xdf: return map(in, big(in, type == 0xde ? 2 : 4), depth);
			default:
				in.failed = true;
				return nullptr;
		}
	}
	static matjson::Value array(ByteCursor& in, uint64_t count, int depth) {
		auto value = matjson::Value::array();
		for (uint64_t i = 0; i < count && !in.failed; ++i) value.push(decode(in, depth + 1));
		return value;
	}
	static matjson::Value map(ByteCursor& in, uint64_t count, int depth) {
		auto value = matjson::Value::object();
		for (uint64_t i = 0; i < count && !in.failed; ++i) {
			auto key = decode(in, depth + 1);
			auto item = decode(in, depth + 1);
			value[key.isString() ? key.asString().unwrap() : key.dump(matjson::NO_INDENTATION)] = std::move(item);
		}
		return value;
	}

	static void mapHeader(ByteSink& out, size_t size) {
		out.uint(0x80 | size, 1);
	}
	static void arrayHeader(ByteSink& out, size_t size) {
		if (size < 16) {
			out.uint(0x90 | size, 1);
		} else if (size <= 0xffff) {
			out.uint(0xdc, 1);
			bigOut(out, size, 2);
		} else {
			out.uint(0xdd, 1);
			bigOut(out, size, 4);
		}
	}
	static void bigOut(ByteSink& out, uint64_t value, size_t width) {
		for (size_t i = width; i-- > 0;) out.data.push_back(static_cast<uint8_t>(value >> (8 * i)));
	}
	static void string(ByteSink& out, std::string_view value) {
		if (value.size() < 32) {
			out.uint(0xa0 | value.size(), 1);
		} else if (value.size() <= 0xff) {
			out.uint(0xd9, 1);
			bigOut(out, value.size(), 1);
		} else if (value.size() <= 0xffff) {
			out.uint(0xda, 1);
			bigOut(out, value.size(), 2);
		} else {
			out.uint(0xdb, 1);
			bigOut(out, value.size(), 4);
		}
		out.text(value);
	}
	static void integer(ByteSink& out, int64_t value) {
		if (value >= 0 && value <= 0x7f) {
			out.uint(static_cast<uint64_t>(value), 1);
		} else if (value < 0 && value >= -32) {
			out.uint(static_cast<uint8_t>(value), 1);
		} else if (value >= 0 && value <= 0xff) {
			out.uint(0xcc, 1);
			bigOut(out, static_cast<uint64_t>(value), 1);
		} else if (value >= 0 && value <= 0xffff) {
			out.uint(0xcd, 1);
			bigOut(out, static_cast<uint64_t>(value), 2);
		} else if (value >= 0 && value <= 0xffffffffll) {
			out.uint(0xce, 1);
			bigOut(out, static_cast<uint64_t>(value), 4);
		} else {
			out.uint(0xd3, 1);
			bigOut(out, static_cast<uint64_t>(value), 8);
		}
	}
	static void real(ByteSink& out, float value) {
		out.uint(0xca, 1);
		bigOut(out, std::bit_cast<uint32_t>(value), 4);
	}
	static void boolean(ByteSink& out, bool value) {
		out.uint(value ? 0xc3 : 0xc2, 1);
	}
	static void key(ByteSink& out, std::string_view name) {
		string(out, name);
	}
};

bool ReplayIO::readGdr(std::filesystem::path const& path) {
	ByteCursor in{ReplayIO::readAll(path)};
	auto& data = in.data;
	auto start = std::find_if(data.begin(), data.end(), [](uint8_t c) {
		return c != ' ' && c != '\t' && c != '\r' && c != '\n';
	});
	matjson::Value json;
	if (start != data.end() && *start == '{') {
		auto parsed = matjson::parse(std::string_view(reinterpret_cast<char const*>(data.data()), data.size()));
		if (!parsed) return false;
		json = parsed.unwrap();
	} else {
		json = GdrReplay::decode(in);
		if (in.failed) return false;
	}
	if (!json.isObject() || !json["inputs"].isArray()) return false;
	std::vector<Frame> inputs;
	for (auto const& item : json["inputs"]) {
		Frame input;
		input.number = std::max(ReplayIO::readInt(item["frame"], 0), 0);
		input.button = ReplayIO::readInt(item["btn"], 1);
		if (input.button < 1 || input.button > 3) input.button = 1;
		input.player2 = ReplayIO::readBoolish(item["2p"]);
		input.holding = ReplayIO::readBoolish(item["down"]);
		inputs.push_back(input);
	}
	ReplayIO::adoptInputs(std::move(inputs), static_cast<float>(ReplayIO::readNumber(json["framerate"], 240.0)));
	return true;
}
bool ReplayIO::writeGdr(std::filesystem::path const& path) {
	auto const& value = Logic::get();
	auto inputs = ReplayIO::sortedInputs();
	int levelId = 0;
	std::string levelName = value.macroName;
	if (auto* play = PlayLayer::get(); play && play->m_level) {
		levelId = play->m_level->m_levelID.value();
		levelName = play->m_level->m_levelName;
	}
	std::string author = GameManager::sharedState()->m_playerName;
	double duration = inputs.empty() ? 0.0 : inputs.back().number / value.sampleRate();

	ByteSink out;
	GdrReplay::mapHeader(out, 12);
	GdrReplay::key(out, "gameVersion");
	GdrReplay::real(out, GdrReplay::gameVersion);
	GdrReplay::key(out, "description");
	GdrReplay::string(out, "");
	GdrReplay::key(out, "version");
	GdrReplay::real(out, GdrReplay::formatVersion);
	GdrReplay::key(out, "duration");
	GdrReplay::real(out, static_cast<float>(duration));
	GdrReplay::key(out, "author");
	GdrReplay::string(out, author);
	GdrReplay::key(out, "seed");
	GdrReplay::integer(out, 0);
	GdrReplay::key(out, "coins");
	GdrReplay::integer(out, 0);
	GdrReplay::key(out, "ldm");
	GdrReplay::boolean(out, false);
	GdrReplay::key(out, "bot");
	GdrReplay::mapHeader(out, 2);
	GdrReplay::key(out, "name");
	GdrReplay::string(out, "Echo");
	GdrReplay::key(out, "version");
	GdrReplay::string(out, Mod::get()->getVersion().toNonVString());
	GdrReplay::key(out, "level");
	GdrReplay::mapHeader(out, 2);
	GdrReplay::key(out, "id");
	GdrReplay::integer(out, levelId);
	GdrReplay::key(out, "name");
	GdrReplay::string(out, levelName);
	GdrReplay::key(out, "inputs");
	GdrReplay::arrayHeader(out, inputs.size());
	for (auto const& input : inputs) {
		GdrReplay::mapHeader(out, 4);
		GdrReplay::key(out, "frame");
		GdrReplay::integer(out, input.number);
		GdrReplay::key(out, "btn");
		GdrReplay::integer(out, input.button);
		GdrReplay::key(out, "2p");
		GdrReplay::boolean(out, input.player2);
		GdrReplay::key(out, "down");
		GdrReplay::boolean(out, input.holding);
	}
	GdrReplay::key(out, "framerate");
	GdrReplay::real(out, value.fps);
	return out.save(path);
}
