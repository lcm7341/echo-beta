// saving & loading macros. write_file / read_file are the way in, the formats themselves are the other files in here
// also autosave, merging 2 macros & importing other bots formats
// a file is never trusted: if it fails to parse it is rejected & the macro thats loaded is left alone
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

namespace fs = std::filesystem;
namespace chrono = std::chrono;

using namespace geode::prelude;

namespace {
    constexpr int FILE_FRAME_SHIFT = 1;
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
    auto lower = path.filename().string();
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower.ends_with(std::string(AUTOSAVE_SUFFIX) + ".echo");
}

bool Logic::autosave_replay() {
    auto& logic = Logic::get();
    if (logic.inputs.empty())
        return false;
    logic.format = logic.save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
    auto path = replays_dir() / (ReplayIO::sanitize(logic.macro_name) + AUTOSAVE_SUFFIX + ".echo");
    if (logic.autosaved_path != path)
        remove_autosave();
    bool ok = ReplayIO::save_as(path, ReplayIO::write_packed_echo);
    if (ok)
        logic.autosaved_path = path;
    return ok;
}

void Logic::poll_autosave() {
    auto& logic = Logic::get();
    if (!logic.is_recording() || logic.inputs.empty())
        return;
    static auto last = chrono::steady_clock::now();
    static size_t saved_count = 0;
    static uint64_t saved_revision = 0;
    auto now = chrono::steady_clock::now();
    if (now - last < chrono::seconds(30))
        return;
    last = now;
    if (logic.inputs.size() == saved_count && logic.inputs_revision == saved_revision)
        return;
    saved_count = logic.inputs.size();
    saved_revision = logic.inputs_revision;
    Logic::get().autosave_replay();
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
    // the name wins. the old path was still the LAST macro so a new recording saved over it
    else if (logic.macro_path.empty() || ReplayIO::lower_ext(logic.macro_path) != ".echo" || ReplayIO::is_autosave(logic.macro_path) ||
             ReplayIO::sanitize(ReplayIO::import_stem(logic.macro_path)) != base)
        logic.macro_path = direct;
    if (!ReplayIO::save_as(logic.macro_path, ReplayIO::write_packed_echo)) {
        Logic::get().notify("Could not write replay", false);
        return false;
    }
    remove_autosave();
    logic.macro_name = ReplayIO::import_stem(logic.macro_path);
    logic.macro_name_typed = false;
    Logic::get().notify(fmt::format("Saved {}", logic.macro_path.filename().string()), true);
    logic.saved_at = std::chrono::steady_clock::now();
    return true;
}

bool Logic::export_kind(std::string_view kind) {
    if (kind == "JSON") {
        auto path = ReplayIO::unique_file(Logic::get().save_root() / "converted", ReplayIO::sanitize(Logic::get().macro_name), ".echo.json");
        bool ok = ReplayIO::save_as(path, ReplayIO::write_echo_json);
        Logic::get().notify(ok ? fmt::format("Exported {}", path.filename().string()) : "Export failed", ok);
        return ok;
    }
    auto const base = ReplayIO::sanitize(Logic::get().macro_name);
    auto converted = Logic::get().save_root() / "converted";
    fs::path path;
    bool ok = false;
    if (kind == "Plain Text") {
        path = ReplayIO::unique_file(converted, base, ".txt");
        ok = ReplayIO::save_as(path, ReplayIO::write_plain);
    }
    else if (kind == "Silicate") {
        path = ReplayIO::unique_file(converted, base, ".slc");
        ok = ReplayIO::save_as(path, ReplayIO::write_silicate);
    }
    else if (kind == "TCBot") {
        path = ReplayIO::unique_file(converted, base, ".tcm");
        ok = ReplayIO::save_as(path, ReplayIO::write_tcbot);
    }
    else if (kind == "GDR") {
        path = ReplayIO::unique_file(converted, base, ".gdr");
        ok = ReplayIO::save_as(path, ReplayIO::write_gdr);
    }
    else {
        Logic::get().notify("Unknown convert format", false);
        return false;
    }
    Logic::get().notify(ok ? fmt::format("Exported {}", path.filename().string()) : "Export failed", ok);
    return ok;
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
    // files are NOT trusted. if reading throws the file fails & the game lives
    bool filled = false;
    try {
        filled = ReplayIO::fill_raw(path);
    }
    catch (...) {
        filled = false;
    }
    if (!filled)
        return false;
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
    if (ext == ".txt")
        return ReplayIO::read_plain(path);
    if (ext == ".slc")
        return ReplayIO::read_silicate(path);
    if (ext == ".tcm")
        return ReplayIO::read_tcbot(path);
    if (ext == ".gdr")
        return ReplayIO::read_gdr(path);
    if (ext == ".echo") {
        std::ifstream probe(path);
        char first = 0;
        probe.get(first);
        if (first == '{')
            return ReplayIO::read_echo_json(path);
        if (first == 'E')
            return ReplayIO::read_packed_echo(path);
        return ReplayIO::read_legacy_echo(path);
    }
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed)
        return ReplayIO::read_legacy_echo(path);
    auto const& json = parsed.unwrap();
    if (json["inputs"].isArray() && json.contains("framerate"))
        return ReplayIO::read_gdr(path);
    if (json.contains("inputs"))
        return ReplayIO::read_echo_json(path);
    return ReplayIO::read_legacy_echo(path);
}

bool Logic::read_file(fs::path const& path) {
    if (!fs::exists(path)) {
        Logic::get().notify("File not found", false);
        return false;
    }
    std::string title = ReplayIO::import_stem(path);
    if (title.empty())
        title = "Replay";
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
    auto& logic = Logic::get();
    if (logic.is_recording() || logic.is_playing()) {
        Logic::get().notify("Stop recording and playing to merge", false);
        return false;
    }
    if (!fs::exists(path)) {
        Logic::get().notify("File not found", false);
        return false;
    }
    if (logic.inputs.empty())
        return Logic::get().read_file(path);
    auto snap = ReplayIO::load_snap(path);
    if (!snap) {
        Logic::get().notify("Unrecognized replay", false);
        return false;
    }
    auto title = ReplayIO::import_stem(path);
    if (title.empty())
        title = path.stem().string();
    if (std::abs(snap->fps - logic.macro_fps) > .001f || !snap->rates.empty() || !logic.rates.empty()) {
        Logic::get().notify(fmt::format("Can't merge {}: it needs the same TPS ({:g} vs {:g}) and no TPS changes", title, snap->fps, logic.macro_fps), false);
        return false;
    }
    auto const& mine = logic.inputs;
    auto const& theirs = snap->inputs;
    bool keep_poses = logic.poses.size() == mine.size() && snap->poses.size() == theirs.size();
    std::vector<Frame> merged;
    std::vector<FramePose> poses;
    merged.reserve(mine.size() + theirs.size());
    if (keep_poses)
        poses.reserve(mine.size() + theirs.size());
    size_t a = 0, b = 0;
    while (a < mine.size() || b < theirs.size()) {
        bool take_mine = b >= theirs.size() || (a < mine.size() && mine[a].number <= theirs[b].number);
        if (take_mine) {
            merged.push_back(mine[a]);
            if (keep_poses)
                poses.push_back(logic.poses[a]);
            ++a;
        }
        else {
            merged.push_back(theirs[b]);
            if (keep_poses)
                poses.push_back(snap->poses[b]);
            ++b;
        }
    }
    size_t added = theirs.size();
    logic.inputs = std::move(merged);
    logic.poses = std::move(poses);
    logic.input_ids.clear();
    logic.touch_inputs();
    Logic::get().refresh_live_hold();
    Logic::get().recompute_cps(logic);
    Logic::get().autosave_replay();
    Logic::get().notify(fmt::format("Merged {} ({} inputs added, {} total)", title, added, logic.inputs.size()), true);
    return true;
}

void Logic::pick_replay() {
    GUI::get().open_macro_picker(false, 1);
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
    if (logic.poses.size() != logic.inputs.size())
        logic.poses.clear();
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
    std::ifstream file(path, std::ios::binary);
    char magic[4] = {};
    file.read(magic, 4);
    return file && magic[0] == 'E' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == '2';
}

std::string ReplayIO::import_stem(fs::path const& path) {
    std::string file = path.filename().string();
    std::string lower = file;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    auto cut = [&](char const* ext) {
        size_t n = std::char_traits<char>::length(ext);
        if (lower.size() < n || lower.compare(lower.size() - n, n, ext) != 0)
            return false;
        file.resize(file.size() - n);
        lower.resize(lower.size() - n);
        return true;
    };
    cut(".echo.json");
    cut(".gdr.json");
    cut(".json");
    cut(".echo");
    cut(AUTOSAVE_SUFFIX);
    cut(".txt");
    cut(".slc");
    cut(".tcm");
    cut(".gdr");
    if (file.empty())
        file = path.stem().string();
    return file;
}

int Logic::convert_legacy_echoes() {
    auto dir = Logic::get().save_root() / "replays";
    std::error_code error;
    if (!fs::exists(dir, error)) {
        Logic::get().notify("No replays folder yet", false);
        return 0;
    }
    std::vector<fs::path> files;
    for (auto const& entry : fs::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file())
            continue;
        files.push_back(entry.path());
    }
    auto saved = ReplayIO::capture_macro();
    int converted = 0;
    int failed = 0;
    int skipped = 0;
    for (auto const& path : files) {
        std::string lower = path.filename().string();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lower.ends_with(".old.echo"))
            continue;
        bool legacy_echo = lower.ends_with(".echo") && !lower.ends_with(".json");
        if (legacy_echo && ReplayIO::file_is_packed(path))
            continue;
        bool known = lower.ends_with(".txt") || lower.ends_with(".json") || lower.ends_with(".slc") || lower.ends_with(".tcm") ||
                     lower.ends_with(".gdr") || legacy_echo;
        if (!known)
            continue;
        auto stem = ReplayIO::import_stem(path);
        if (!legacy_echo && fs::exists(dir / (stem + ".echo"))) {
            ++skipped;
            continue;
        }
        if (!ReplayIO::fill_replay(path)) {
            ++failed;
            ReplayIO::restore_macro(saved);
            continue;
        }
        if (legacy_echo) {
            fs::rename(path, dir / (stem + ".old.echo"), error);
            if (error) {
                ++failed;
                error.clear();
                ReplayIO::restore_macro(saved);
                continue;
            }
        }
        auto dest = ReplayIO::unique_file(dir, stem, ".echo");
        if (!ReplayIO::save_as(dest, ReplayIO::write_packed_echo)) {
            ++failed;
            ReplayIO::restore_macro(saved);
            continue;
        }
        ++converted;
        ReplayIO::restore_macro(saved);
    }
    std::string message = fmt::format("Converted {} replay{}", converted, converted == 1 ? "" : "s");
    if (skipped)
        message += fmt::format(", {} already converted", skipped);
    if (failed)
        message += fmt::format(", {} failed", failed);
    Logic::get().notify(message, failed == 0);
    return converted;
}

void Logic::import_replays() {
    using namespace geode::utils::file;
    FilePickOptions options;
    options.defaultPath = Logic::get().save_root() / "replays";
    options.filters.push_back({ "Replays", { "*.echo", "*.json", "*.txt", "*.slc", "*.tcm", "*.gdr" } });
    geode::async::spawn(pickMany(std::move(options)), [](PickManyResult result) {
        if (!result) {
            Logic::get().notify(result.unwrapErr(), false);
            return;
        }
        auto saved = ReplayIO::capture_macro();
        auto dir = Logic::get().save_root() / "replays";
        int copied = 0;
        int failed = 0;
        for (auto const& path : result.unwrap()) {
            if (!ReplayIO::fill_replay(path)) {
                ++failed;
                ReplayIO::restore_macro(saved);
                continue;
            }
            auto dest = ReplayIO::unique_file(dir, ReplayIO::import_stem(path), ".echo");
            if (!ReplayIO::save_as(dest, ReplayIO::write_packed_echo)) {
                ++failed;
                ReplayIO::restore_macro(saved);
                continue;
            }
            ++copied;
            ReplayIO::restore_macro(saved);
        }
        std::string message = fmt::format("Imported {} replay{}", copied, copied == 1 ? "" : "s");
        if (failed)
            message += fmt::format(", {} failed", failed);
        Logic::get().notify(message, failed == 0);
    });
}
