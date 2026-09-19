// saving & loading macros. write_file / read_file are the way in, the formats themselves are the other files in here
// also autosave, merging 2 macros & importing other bots formats
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

namespace fs = std::filesystem;
namespace chrono = std::chrono;

using namespace geode::prelude;

namespace {
    constexpr int FILE_FRAME_SHIFT = 0;
    constexpr char const* AUTOSAVE_SUFFIX = ".autosave";

    fs::path replays_dir() {
        auto dir = Logic::get().save_root() / "replays";
        std::error_code error;
        fs::create_directories(dir, error);
        return dir;
    }

    void remove_autosave() {
        auto& logic = Logic::get();
        if (logic.autosaved_path.empty())
            return;
        std::error_code error;
        if (logic.autosaved_path != logic.macro_path)
            fs::remove(logic.autosaved_path, error);
        logic.autosaved_path.clear();
    }
}

bool ReplayIO::save_as(fs::path const& path, bool (*write)(fs::path const&)) {
    auto& logic = Logic::get();
    auto inputs = logic.inputs;
    auto rates = logic.rates;
    std::ranges::for_each(logic.inputs, [](Frame& input) {
        input.number = std::max(input.number - FILE_FRAME_SHIFT, 0);
    });
    std::ranges::for_each(logic.rates, [](RateChange& change) {
        change.frame = std::max(change.frame - FILE_FRAME_SHIFT, 0);
    });
    bool ok = write(path);
    logic.inputs = std::move(inputs);
    logic.rates = std::move(rates);
    logic.touch_inputs();
    return ok;
}

bool ReplayIO::is_autosave(fs::path const& path) {
    // later
    return {};
}

bool Logic::autosave_replay() {
    // TODO
    return {};
}

void Logic::poll_autosave() {
    // TODO
}

bool Logic::write_file() {
    auto& logic = Logic::get();
    if (logic.inputs.empty()) {
        Logic::get().notify("Nothing to save", false);
        return false;
    }
    logic.format = logic.save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
    auto dir = replays_dir();
    auto base = ReplayIO::sanitize(logic.macro_name);
    auto direct = dir / (base + ".echo");
    if (logic.no_overwrite)
        logic.macro_path = ReplayIO::unique_file(dir, base, ".echo");
    else if (logic.macro_path.empty() || ReplayIO::lower_ext(logic.macro_path) != ".echo" || ReplayIO::is_autosave(logic.macro_path))
        logic.macro_path = direct;
    if (!ReplayIO::save_as(logic.macro_path, ReplayIO::write_packed_echo)) {
        Logic::get().notify("Could not write replay", false);
        return false;
    }
    remove_autosave();
    logic.macro_name = ReplayIO::import_stem(logic.macro_path);
    logic.macro_name_typed = false;
    Logic::get().notify(fmt::format("Saved {}", logic.macro_path.filename().string()), true);
    return true;
}

bool Logic::export_kind(std::string_view kind) {
    // TODO
    return {};
}

bool Logic::append_sequence_file(fs::path const& path, int amount) {
    if (!fs::exists(path)) {
        Logic::get().notify("File not found", false);
        return false;
    }
    auto snap = ReplayIO::load_snap(path);
    if (!snap) {
        Logic::get().notify("Unrecognized replay", false);
        return false;
    }
    auto& logic = Logic::get();
    Replay clip;
    auto title = ReplayIO::import_stem(path);
    clip.name = title.empty() ? path.stem().string() : title;
    clip.actions = std::move(snap->inputs);
    clip.fps = snap->fps;
    clip.rates = snap->rates;
    clip.lvl_seed = snap->lvl_seed;
    clip.lvl_seed_known = snap->lvl_seed_known;
    clip.unrounded = snap->unrounded;
    clip.silicate_variance = snap->silicate_variance;
    amount = std::max(amount, 1);
    for (int i = 0; i < amount; ++i)
        logic.sequence.push_back(clip);
    logic.message = fmt::format("Queued {} ({} clips)", logic.sequence.back().name, logic.sequence.size());
    Logic::get().notify(logic.message, true);
    return true;
}

std::optional<MacroSnap> ReplayIO::load_snap(fs::path const& path) {
    auto saved = ReplayIO::capture_macro();
    if (!ReplayIO::fill_replay(path)) {
        ReplayIO::restore_macro(saved);
        return std::nullopt;
    }
    auto snap = ReplayIO::capture_macro();
    ReplayIO::restore_macro(saved);
    return snap;
}

bool ReplayIO::fill_replay(fs::path const& path) {
    bool filled = false;
    try {
        filled = ReplayIO::fill_raw(path);
    }
    catch (...) {
        filled = false;
    }
    if (!filled) {
        return false;
    }
    auto& logic = Logic::get();
    std::ranges::for_each(logic.inputs, [](Frame& input) {
        input.number += FILE_FRAME_SHIFT;
    });
    std::ranges::for_each(logic.rates, [](RateChange& change) {
        change.frame += FILE_FRAME_SHIFT;
    });
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    return true;
}

bool ReplayIO::fill_raw(fs::path const& path) {
    auto ext = ReplayIO::lower_ext(path);
    if (ext == ".txt") {
        return ReplayIO::read_plain(path);
    }
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    auto const& json = parsed.unwrap();
    if (json.contains("inputs")) {
        return ReplayIO::read_echo_json(path);
    }
    return ReplayIO::read_legacy_echo(path);
}

bool Logic::read_file(fs::path const& path) {
    log::debug("read file");
    if (!fs::exists(path)) {
        Logic::get().notify("File not found", false);
        return false;
    }
    std::string title = ReplayIO::import_stem(path);
    if (title.empty()) {
        title = "Replay";
    }
    Logic::get().notify(fmt::format("Loading {}", title), true);
    static std::atomic<unsigned> ticket{ 0 };
    unsigned mine = ticket.fetch_add(1) + 1;
    try {
        std::thread([path, title, mine] {
            auto scratch = std::make_unique<Logic>();
            Logic::set_loading(scratch.get());
            std::optional<MacroSnap> snap;
            try {
                snap = ReplayIO::load_snap(path);
                if (snap) {
                    std::stable_sort(snap->inputs.begin(), snap->inputs.end(), [](Frame const& a, Frame const& b) {
                        return a.number < b.number;
                    });
                }
            }
            catch (...) {
                snap.reset();
            }
            Logic::set_loading(nullptr);
            geode::queueInMainThread([path, title, mine, snap = std::move(snap)]() mutable {
                if (mine != ticket.load())
                    return;
                if (!snap) {
                    Logic::get().notify("Unrecognized replay", false);
                    return;
                }
                auto amt = snap->inputs.size();
                ReplayIO::restore_macro(snap.value());
                Logic::get().macro_name = title;
                Logic::get().macro_name_typed = false;
                Logic::get().macro_path = path;
                Logic::get().autosaved_path = ReplayIO::is_autosave(path) ? path : fs::path{};
                ReplayIO::finish_load(fmt::format("Loaded {} ({} inputs)", title, amt));
                Logic::get().recompute_cps(Logic::get());
            });
        }).detach();
    }
    catch (...) {
        Logic::get().notify("Could not start loading", false);
        return false;
    }
    return true;
}

bool Logic::merge_replay(fs::path const& path) {
    // TODO
    return {};
}

void Logic::pick_replay() {
}

MacroSnap ReplayIO::capture_macro() {
    auto const& logic = Logic::get();
    return { logic.inputs, logic.poses, logic.format, logic.macro_fps, logic.end_x, logic.recording_seconds, logic.attempts, logic.save_debug,
        logic.lvl_seed, logic.lvl_seed_known, logic.macro_unrounded, logic.macro_silicate_variance, logic.rates, logic.highest_cps, logic.cps_notes_p1, logic.cps_notes_p2 };
}

void ReplayIO::restore_macro(MacroSnap const& snap) {
    auto& logic = Logic::get();
    logic.inputs = snap.inputs;
    logic.poses = snap.poses;
    if (logic.poses.size() != logic.inputs.size()) {
        logic.poses.clear();
    }
    logic.input_ids.clear();
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    logic.format = snap.format;
    logic.fps = snap.fps;
    logic.macro_fps = snap.fps;
    logic.rates = snap.rates;
    logic.end_x = snap.end_x;
    logic.recording_seconds = snap.recording_seconds;
    logic.attempts = snap.attempts;
    logic.lvl_seed = snap.lvl_seed;
    logic.lvl_seed_known = snap.lvl_seed_known;
    logic.lvl_seed_from_file = snap.lvl_seed_known;
    logic.macro_unrounded = snap.unrounded;
    logic.macro_silicate_variance = snap.silicate_variance;
    logic.highest_cps = snap.highest_cps;
    logic.cps_notes_p1 = snap.cps_notes_p1;
    logic.cps_notes_p2 = snap.cps_notes_p2;
}

bool ReplayIO::file_is_packed(fs::path const& path) {
    log::debug("file is packed");
    std::ifstream file(path, std::ios::binary);
    char magic[4] = {};
    file.read(magic, 4);
    return file && magic[0] == 'E' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == '2';
}

std::string ReplayIO::import_stem(fs::path const& path) {
    // TODO
    return {};
}

int Logic::convert_legacy_echoes() {
    // todo
    return {};
}

void Logic::import_replays() {
    // TODO
}
