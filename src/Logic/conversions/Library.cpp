#include "GUI/gui.hpp"
#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include <Geode/utils/async.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>




using namespace geode::prelude;


namespace {
// Echo counts the first step as frame 1. Files keep the older numbering, where it was frame 0, so
// macros saved before the change and other bots' files play on the same steps.
constexpr int kFileFrameShift = 1;
constexpr char const* kAutosaveSuffix = ".autosave";

std::filesystem::path replaysDir() {
	auto dir = Logic::get().saveRoot() / "replays";
	std::error_code error;
	std::filesystem::create_directories(dir, error);
	return dir;
}

void removeAutosave() {
	auto& value = Logic::get();
	if (value.autosavedPath.empty()) return;
	std::error_code error;
	if (value.autosavedPath != value.macroPath) std::filesystem::remove(value.autosavedPath, error);
	value.autosavedPath.clear();
}
}

bool ReplayIO::saveAs(std::filesystem::path const& path, bool (*write)(std::filesystem::path const&)) {
	auto& value = Logic::get();
	auto inputs = value.inputs;
	auto rates = value.rates;
	std::ranges::for_each(value.inputs, [](Frame& input) { input.number = std::max(input.number - kFileFrameShift, 0); });
	std::ranges::for_each(value.rates, [](RateChange& change) { change.frame = std::max(change.frame - kFileFrameShift, 0); });
	bool ok = write(path);
	value.inputs = std::move(inputs);
	value.rates = std::move(rates);
	value.touchInputs();
	return ok;
}
bool ReplayIO::isAutosave(std::filesystem::path const& path) {
	auto lower = path.filename().string();
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return lower.ends_with(std::string(kAutosaveSuffix) + ".echo");
}
// Writes to a scratch file beside the replays, so a saved macro only changes when the user saves it.
bool Logic::autosaveReplay() {
	auto& value = Logic::get();
	if (value.inputs.empty()) return false;
	value.format = value.saveDebug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
	auto path = replaysDir() / (ReplayIO::sanitize(value.macroName) + kAutosaveSuffix + ".echo");
	if (value.autosavedPath != path) removeAutosave();
	bool ok = ReplayIO::saveAs(path, ReplayIO::writePackedEcho);
	if (ok) value.autosavedPath = path;
	return ok;
}
bool Logic::saveReplay() {
	auto& value = Logic::get();
	if (value.inputs.empty()) {
		Logic::get().notify("Nothing to save", false);
		return false;
	}
	value.format = value.saveDebug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
	auto dir = replaysDir();
	auto base = ReplayIO::sanitize(value.macroName);
	auto direct = dir / (base + ".echo");
	// A path left from the last save would write that same file again. No Overwrite always takes the next free _# name.
	if (value.noOverwrite) value.macroPath = ReplayIO::uniqueFile(dir, base, ".echo");
	else if (value.macroPath.empty() || ReplayIO::lowerExt(value.macroPath) != ".echo" || ReplayIO::isAutosave(value.macroPath)) value.macroPath = direct;
	if (!ReplayIO::saveAs(value.macroPath, ReplayIO::writePackedEcho)) {
		Logic::get().notify("Could not write replay", false);
		return false;
	}
	removeAutosave();
	value.macroName = ReplayIO::importStem(value.macroPath);
	Logic::get().notify(fmt::format("Saved {}", value.macroPath.filename().string()), true);
	return true;
}
bool Logic::exportKind(std::string_view kind) {
	if (kind == "JSON") {
		auto path = ReplayIO::uniqueFile(Logic::get().saveRoot() / "converted", ReplayIO::sanitize(Logic::get().macroName), ".echo.json");
		bool ok = ReplayIO::saveAs(path, ReplayIO::writeEchoJson);
		Logic::get().notify(ok ? fmt::format("Exported {}", path.filename().string()) : "Export failed", ok);
		return ok;
	}
	auto const base = ReplayIO::sanitize(Logic::get().macroName);
	auto converted = Logic::get().saveRoot() / "converted";
	std::filesystem::path path;
	bool ok = false;
	if (kind == "Plain Text") {
		path = ReplayIO::uniqueFile(converted, base, ".txt");
		ok = ReplayIO::saveAs(path, ReplayIO::writePlain);
	} else if (kind == "Silicate") {
		path = ReplayIO::uniqueFile(converted, base, ".slc");
		ok = ReplayIO::saveAs(path, ReplayIO::writeSilicate);
	} else if (kind == "TCBot") {
		path = ReplayIO::uniqueFile(converted, base, ".tcm");
		ok = ReplayIO::saveAs(path, ReplayIO::writeTcbot);
	} else if (kind == "GDR") {
		path = ReplayIO::uniqueFile(converted, base, ".gdr");
		ok = ReplayIO::saveAs(path, ReplayIO::writeGdr);
	} else {
		Logic::get().notify("Unknown convert format", false);
		return false;
	}
	Logic::get().notify(ok ? fmt::format("Exported {}", path.filename().string()) : "Export failed", ok);
	return ok;
}
bool Logic::appendSequenceFile(std::filesystem::path const& path, int amount) {
	if (!std::filesystem::exists(path)) {
		Logic::get().notify("File not found", false);
		return false;
	}
	auto snap = ReplayIO::loadSnap(path);
	if (!snap) {
		Logic::get().notify("Unrecognized replay", false);
		return false;
	}
	auto& value = Logic::get();
	Replay clip;
	auto title = ReplayIO::importStem(path);
	clip.name = title.empty() ? path.stem().string() : title;
	clip.actions = std::move(snap->inputs);
	amount = std::max(amount, 1);
	for (int i = 0; i < amount; ++i) value.sequence.push_back(clip);
	value.message = fmt::format("Queued {} ({} clips)", value.sequence.back().name, value.sequence.size());
	Logic::get().notify(value.message, true);
	return true;
}
std::optional<MacroSnap> ReplayIO::loadSnap(std::filesystem::path const& path) {
	auto saved = ReplayIO::captureMacro();
	if (!ReplayIO::fillReplay(path)) {
		ReplayIO::restoreMacro(saved);
		return std::nullopt;
	}
	auto snap = ReplayIO::captureMacro();
	ReplayIO::restoreMacro(saved);
	return snap;
}
bool ReplayIO::fillReplay(std::filesystem::path const& path) {
	if (!ReplayIO::fillRaw(path)) return false;
	auto& value = Logic::get();
	std::ranges::for_each(value.inputs, [](Frame& input) { input.number += kFileFrameShift; });
	std::ranges::for_each(value.rates, [](RateChange& change) { change.frame += kFileFrameShift; });
	value.touchInputs();
	Logic::get().refreshLiveHold();
	return true;
}
bool ReplayIO::fillRaw(std::filesystem::path const& path) {
	auto ext = ReplayIO::lowerExt(path);
	if (ext == ".txt") return ReplayIO::readPlain(path);
	if (ext == ".slc") return ReplayIO::readSilicate(path);
	if (ext == ".tcm") return ReplayIO::readTcbot(path);
	if (ext == ".gdr") return ReplayIO::readGdr(path);
	if (ext == ".echo") {
		std::ifstream probe(path);
		char first = 0;
		probe.get(first);
		if (first == '{') return ReplayIO::readEchoJson(path);
		if (first == 'E') return ReplayIO::readPackedEcho(path);
		return ReplayIO::readLegacyEcho(path);
	}
	std::ifstream file(path);
	auto parsed = matjson::Value::parse(file);
	if (!parsed) return ReplayIO::readLegacyEcho(path);
	auto const& json = parsed.unwrap();
	if (json["inputs"].isArray() && json.contains("framerate")) return ReplayIO::readGdr(path);
	if (json.contains("inputs")) return ReplayIO::readEchoJson(path);
	return ReplayIO::readLegacyEcho(path);
}
bool Logic::readReplay(std::filesystem::path const& path) {
	if (!std::filesystem::exists(path)) {
		Logic::get().notify("File not found", false);
		return false;
	}
	std::string title = ReplayIO::importStem(path);
	if (title.empty()) title = "Replay";
	Logic::get().notify(fmt::format("Loading {}", title), true);
	// The decode writes through Logic::get(), so the worker points that at its own session.
	// The live one stays with the game thread. A plain thread is used because a runtime
	// task never gets polled unless something is already awaiting it.
	static std::atomic<unsigned> ticket{0};
	unsigned mine = ticket.fetch_add(1) + 1;
	try {
		std::thread([path, title, mine] {
			auto scratch = std::make_unique<Logic>();
			Logic::setLoading(scratch.get());
			std::optional<MacroSnap> snap;
			try {
				snap = ReplayIO::loadSnap(path);
				if (snap) {
					std::stable_sort(snap->inputs.begin(), snap->inputs.end(), [](Frame const& a, Frame const& b) {
						if (a.number == b.number) return a.player2 < b.player2;
						return a.number < b.number;
					});
				}
			} catch (...) {
				snap.reset();
			}
			Logic::setLoading(nullptr);
			geode::queueInMainThread([path, title, mine, snap = std::move(snap)]() mutable {
				if (mine != ticket.load()) return;
				if (!snap) {
					Logic::get().notify("Unrecognized replay", false);
					return;
				}
				auto count = snap->inputs.size();
				ReplayIO::restoreMacro(snap.value());
				Logic::get().macroName = title;
				Logic::get().macroPath = path;
				Logic::get().autosavedPath = ReplayIO::isAutosave(path) ? path : std::filesystem::path{};
				ReplayIO::finishLoad(fmt::format("Loaded {} ({} inputs)", title, count));
				Logic::get().recomputeCps(Logic::get());
			});
		}).detach();
	} catch (...) {
		Logic::get().notify("Could not start loading", false);
		return false;
	}
	return true;
}
void Logic::pickReplay() {
	GUI::get().openMacroPicker(false, 1);
}


MacroSnap ReplayIO::captureMacro() {
	auto const& value = Logic::get();
	return {value.inputs, value.poses, value.format, value.macroFps, value.endX, value.recordingSeconds, value.attempts, value.saveDebug,
		value.levelSeed, value.levelSeedKnown, value.macroUnrounded, value.macroSilicateVariance, value.rates, value.highestCps, value.cpsNotesP1, value.cpsNotesP2};
}
void ReplayIO::restoreMacro(MacroSnap const& snap) {
	auto& value = Logic::get();
	value.inputs = snap.inputs;
	value.poses = snap.poses;
	if (value.poses.size() != value.inputs.size()) value.poses.clear();
	value.inputIds.clear();
	value.touchInputs();
	Logic::get().refreshLiveHold();
	value.format = snap.format;
	value.fps = snap.fps;
	value.macroFps = snap.fps;
	value.rates = snap.rates;
	value.endX = snap.endX;
	value.recordingSeconds = snap.recordingSeconds;
	value.attempts = snap.attempts;
	value.saveDebug = snap.saveDebug;
	value.levelSeed = snap.levelSeed;
	value.levelSeedKnown = snap.levelSeedKnown;
	value.levelSeedFromFile = snap.levelSeedKnown;
	value.macroUnrounded = snap.unrounded;
	value.macroSilicateVariance = snap.silicateVariance;
	value.highestCps = snap.highestCps;
	value.cpsNotesP1 = snap.cpsNotesP1;
	value.cpsNotesP2 = snap.cpsNotesP2;
}
bool ReplayIO::fileIsPacked(std::filesystem::path const& path) {
	std::ifstream file(path, std::ios::binary);
	char magic[4] = {};
	file.read(magic, 4);
	return file && magic[0] == 'E' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == '2';
}
std::string ReplayIO::importStem(std::filesystem::path const& path) {
	std::string file = path.filename().string();
	std::string lower = file;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	auto cut = [&](char const* ext) {
		size_t n = std::char_traits<char>::length(ext);
		if (lower.size() < n || lower.compare(lower.size() - n, n, ext) != 0) return false;
		file.resize(file.size() - n);
		lower.resize(lower.size() - n);
		return true;
	};
	cut(".echo.json");
	cut(".gdr.json");
	cut(".json");
	cut(".echo");
	cut(kAutosaveSuffix);
	cut(".txt");
	cut(".slc");
	cut(".tcm");
	cut(".gdr");
	if (file.empty()) file = path.stem().string();
	return file;
}
int Logic::convertLegacyEchoes() {
	auto dir = Logic::get().saveRoot() / "replays";
	std::error_code error;
	if (!std::filesystem::exists(dir, error)) {
		Logic::get().notify("No replays folder yet", false);
		return 0;
	}
	std::vector<std::filesystem::path> files;
	for (auto const& entry : std::filesystem::directory_iterator(dir, error)) {
		if (error || !entry.is_regular_file()) continue;
		files.push_back(entry.path());
	}
	auto saved = ReplayIO::captureMacro();
	int converted = 0;
	int failed = 0;
	int skipped = 0;
	for (auto const& path : files) {
		std::string lower = path.filename().string();
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if (lower.ends_with(".old.echo")) continue;
		bool legacyEcho = lower.ends_with(".echo") && !lower.ends_with(".json");
		if (legacyEcho && ReplayIO::fileIsPacked(path)) continue;
		bool known = lower.ends_with(".txt") || lower.ends_with(".json") || lower.ends_with(".slc") || lower.ends_with(".tcm") ||
			lower.ends_with(".gdr") || legacyEcho;
		if (!known) continue;
		auto stem = ReplayIO::importStem(path);
		if (!legacyEcho && std::filesystem::exists(dir / (stem + ".echo"))) {
			++skipped;
			continue;
		}
		if (!ReplayIO::fillReplay(path)) {
			++failed;
			ReplayIO::restoreMacro(saved);
			continue;
		}
		if (legacyEcho) {
			std::filesystem::rename(path, dir / (stem + ".old.echo"), error);
			if (error) {
				++failed;
				error.clear();
				ReplayIO::restoreMacro(saved);
				continue;
			}
		}
		auto dest = ReplayIO::uniqueFile(dir, stem, ".echo");
		if (!ReplayIO::saveAs(dest, ReplayIO::writePackedEcho)) {
			++failed;
			ReplayIO::restoreMacro(saved);
			continue;
		}
		++converted;
		ReplayIO::restoreMacro(saved);
	}
	std::string message = fmt::format("Converted {} replay{}", converted, converted == 1 ? "" : "s");
	if (skipped) message += fmt::format(", {} already converted", skipped);
	if (failed) message += fmt::format(", {} failed", failed);
	Logic::get().notify(message, failed == 0);
	return converted;
}
void Logic::importReplays() {
	using namespace geode::utils::file;
	FilePickOptions options;
	options.defaultPath = Logic::get().saveRoot() / "replays";
	options.filters.push_back({"Replays", {"*.echo", "*.json", "*.txt", "*.slc", "*.tcm", "*.gdr"}});
	geode::async::spawn(pickMany(std::move(options)), [](PickManyResult result) {
		if (!result) {
			Logic::get().notify(result.unwrapErr(), false);
			return;
		}
		auto saved = ReplayIO::captureMacro();
		auto dir = Logic::get().saveRoot() / "replays";
		int copied = 0;
		int failed = 0;
		for (auto const& path : result.unwrap()) {
			if (!ReplayIO::fillReplay(path)) {
				++failed;
				ReplayIO::restoreMacro(saved);
				continue;
			}
			auto dest = ReplayIO::uniqueFile(dir, ReplayIO::importStem(path), ".echo");
			if (!ReplayIO::saveAs(dest, ReplayIO::writePackedEcho)) {
				++failed;
				ReplayIO::restoreMacro(saved);
				continue;
			}
			++copied;
			ReplayIO::restoreMacro(saved);
		}
		std::string message = fmt::format("Imported {} replay{}", copied, copied == 1 ? "" : "s");
		if (failed) message += fmt::format(", {} failed", failed);
		Logic::get().notify(message, failed == 0);
	});
}
