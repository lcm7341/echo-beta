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

constexpr int FILE_FRAME_SHIFT = 1;
constexpr const char* AUTOSAVE_SUFFIX = ".autosave";

static fs::path replays_dir() {
    auto dir = Logic::get().save_root() / "replays";
    std::error_code error;
    fs::create_directories(dir, error);
    return dir;
}

static void remove_autosave() {
    auto& logic = Logic::get();
    if (logic.autosaved_path.empty())
        return;
    std::error_code error;
    if (logic.autosaved_path != logic.macro_path)
        fs::remove(logic.autosaved_path, error);
    logic.autosaved_path.clear();
}

bool ReplayIO::save_as(const fs::path& path, bool (*write)(const fs::path&, const MacroSnap&)) {
    return ReplayIO::save_snap(path, ReplayIO::capture_macro(), write);
}

bool ReplayIO::save_snap(const fs::path& path, MacroSnap snap, bool (*write)(const fs::path&, const MacroSnap&)) {
    for (auto& input : snap.inputs)
        input.number = std::max(input.number - FILE_FRAME_SHIFT, 0);
    for (auto& change : snap.rates)
        change.frame = std::max(change.frame - FILE_FRAME_SHIFT, 0);
    return write(path, snap);
}

bool ReplayIO::is_autosave(const fs::path& path) {
    auto lower = path.filename().string();
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return lower.ends_with(std::string(AUTOSAVE_SUFFIX) + ".echo");
}

bool Logic::autosave_replay() {
    if (inputs.empty())
        return false;
    format = save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
    auto path = replays_dir() / (ReplayIO::sanitize(macro_name) + AUTOSAVE_SUFFIX + ".echo");
    if (autosaved_path != path)
        remove_autosave();
    bool ok = ReplayIO::save_as(path, ReplayIO::write_echo);
    if (ok)
        autosaved_path = path;
    return ok;
}

void Logic::poll_autosave() {
    if (!is_recording() || inputs.empty())
        return;
    static auto last = chrono::steady_clock::now();
    static size_t saved_count = 0;
    static unsigned long long saved_revision = 0;
    auto now = chrono::steady_clock::now();
    if (now - last < chrono::seconds(30))
        return;
    last = now;
    if (inputs.size() == saved_count && inputs_revision == saved_revision)
        return;
    saved_count = inputs.size();
    saved_revision = inputs_revision;
    autosave_replay();
}

bool Logic::write_file() {
    if (inputs.empty()) {
        notify("Nothing to save", false);
        return false;
    }
    format = save_debug ? ReplayFormat::MetaDebug : ReplayFormat::Meta;
    auto dir = replays_dir();
    auto base = ReplayIO::sanitize(macro_name);
    auto direct = dir / (base + ".echo");
    if (no_overwrite)
        macro_path = ReplayIO::unique_file(dir, base, ".echo");
    else if (macro_path.empty() || ReplayIO::lower_ext(macro_path) != ".echo" || ReplayIO::is_autosave(macro_path) ||
             ReplayIO::sanitize(ReplayIO::import_stem(macro_path)) != base)
        macro_path = direct;
    if (!ReplayIO::save_as(macro_path, ReplayIO::write_echo)) {
        notify("Could not write replay", false);
        return false;
    }
    remove_autosave();
    macro_name = ReplayIO::import_stem(macro_path);
    macro_name_typed = false;
    notify(fmt::format("Saved {}", macro_path.filename().string()), true);
    saved_at = std::chrono::steady_clock::now();
    return true;
}

bool Logic::export_kind(std::string_view kind) {
    if (kind == "JSON") {
        auto path = ReplayIO::unique_file(save_root() / "converted", ReplayIO::sanitize(macro_name), ".echo.json");
        bool ok = ReplayIO::save_as(path, ReplayIO::write_echo_json);
        notify(ok ? fmt::format("Exported {}", path.filename().string()) : "Export failed", ok);
        return ok;
    }
    const auto base = ReplayIO::sanitize(macro_name);
    auto converted = save_root() / "converted";
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
        notify("Unknown convert format", false);
        return false;
    }
    notify(ok ? fmt::format("Exported {}", path.filename().string()) : "Export failed", ok);
    return ok;
}

bool Logic::append_sequence_file(const fs::path& path, int amount) {
    if (!fs::exists(path)) {
        notify("File not found", false);
        return false;
    }
    auto snap = ReplayIO::load_snap(path);
    if (!snap) {
        notify("Unrecognized replay", false);
        return false;
    }
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
        sequence.push_back(clip);
    message = fmt::format("Queued {} ({} clips)", sequence.back().name, sequence.size());
    notify(message, true);
    return true;
}

std::optional<MacroSnap> ReplayIO::load_snap(const fs::path& path) {
    MacroSnap snap;
    if (!ReplayIO::read_any(path, snap))
        return std::nullopt;
    return snap;
}

static bool read_by_extension(const fs::path& path, MacroSnap& out) {
    auto ext = ReplayIO::lower_ext(path);
    if (ext == ".txt")
        return ReplayIO::read_plain(path, out);
    if (ext == ".slc")
        return ReplayIO::read_silicate(path, out);
    if (ext == ".tcm")
        return ReplayIO::read_tcbot(path, out);
    if (ext == ".gdr")
        return ReplayIO::read_gdr(path, out);
    if (ext == ".echo") {
        std::ifstream probe(path);
        char first = 0;
        probe.get(first);
        if (first == '{')
            return ReplayIO::read_echo_json(path, out);
        if (first == 'E')
            return ReplayIO::read_echo(path, out);
        return ReplayIO::read_legacy_echo(path, out);
    }
    std::ifstream file(path);
    auto parsed = matjson::Value::parse(file);
    if (!parsed)
        return ReplayIO::read_legacy_echo(path, out);
    const auto& json = parsed.unwrap();
    if (json["inputs"].isArray() && json.contains("framerate"))
        return ReplayIO::read_gdr(path, out);
    if (json.contains("inputs"))
        return ReplayIO::read_echo_json(path, out);
    return ReplayIO::read_legacy_echo(path, out);
}

bool ReplayIO::read_any(const fs::path& path, MacroSnap& out) {
    // files are NOT trusted. if reading throws the file fails & the game lives
    out = MacroSnap{};
    bool read = false;
    try {
        read = read_by_extension(path, out);
    }
    catch (...) {
        read = false;
    }
    if (!read)
        return false;
    for (auto& input : out.inputs)
        input.number += FILE_FRAME_SHIFT;
    for (auto& change : out.rates)
        change.frame += FILE_FRAME_SHIFT;
    return true;
}

bool Logic::read_file(const fs::path& path) {
    if (!fs::exists(path)) {
        notify("File not found", false);
        return false;
    }
    std::string title = ReplayIO::import_stem(path);
    if (title.empty())
        title = "Replay";
    notify(fmt::format("Loading {}", title), true);
    static std::atomic<unsigned> ticket{ 0 };
    unsigned mine = ticket.fetch_add(1) + 1;
    try {
        std::thread([path, title, mine] {
            auto snap = ReplayIO::load_snap(path);
            if (snap) {
                std::stable_sort(snap->inputs.begin(), snap->inputs.end(), [](const Frame& a, const Frame& b) {
                    return a.number < b.number;
                });
            }
            geode::queueInMainThread([path, title, mine, snap = std::move(snap)]() mutable {
                if (mine != ticket.load())
                    return;
                auto& logic = Logic::get();
                if (!snap) {
                    logic.notify("Unrecognized replay", false);
                    return;
                }
                auto amt = snap->inputs.size();
                ReplayIO::restore_macro(snap.value());
                logic.macro_name = title;
                logic.macro_name_typed = false;
                logic.macro_path = path;
                logic.autosaved_path = ReplayIO::is_autosave(path) ? path : fs::path{};
                ReplayIO::finish_load(fmt::format("Loaded {} ({} inputs)", title, amt));
                logic.recompute_cps();
            });
        }).detach();
    }
    catch (...) {
        notify("Could not start loading", false);
        return false;
    }
    return true;
}

bool Logic::merge_replay(const fs::path& path) {
    if (is_recording() || is_playing()) {
        notify("Stop recording and playing to merge", false);
        return false;
    }
    if (!fs::exists(path)) {
        notify("File not found", false);
        return false;
    }
    if (inputs.empty())
        return read_file(path);
    auto snap = ReplayIO::load_snap(path);
    if (!snap) {
        notify("Unrecognized replay", false);
        return false;
    }
    auto title = ReplayIO::import_stem(path);
    if (title.empty())
        title = path.stem().string();
    if (std::abs(snap->fps - macro_fps) > .001f || !snap->rates.empty() || !rates.empty()) {
        notify(fmt::format("Can't merge {}: it needs the same TPS ({:g} vs {:g}) and no TPS changes", title, snap->fps, macro_fps), false);
        return false;
    }
    const auto& mine = inputs;
    const auto& theirs = snap->inputs;
    bool keep_poses = poses.size() == mine.size() && snap->poses.size() == theirs.size();
    std::vector<Frame> merged;
    std::vector<FramePose> merged_poses;
    merged.reserve(mine.size() + theirs.size());
    if (keep_poses)
        merged_poses.reserve(mine.size() + theirs.size());
    size_t a = 0, b = 0;
    while (a < mine.size() || b < theirs.size()) {
        bool take_mine = b >= theirs.size() || (a < mine.size() && mine[a].number <= theirs[b].number);
        if (take_mine) {
            merged.push_back(mine[a]);
            if (keep_poses)
                merged_poses.push_back(poses[a]);
            ++a;
        }
        else {
            merged.push_back(theirs[b]);
            if (keep_poses)
                merged_poses.push_back(snap->poses[b]);
            ++b;
        }
    }
    size_t added = theirs.size();
    inputs = std::move(merged);
    poses = std::move(merged_poses);
    input_ids.clear();
    touch_inputs();
    refresh_live_hold();
    recompute_cps();
    autosave_replay();
    notify(fmt::format("Merged {} ({} inputs added, {} total)", title, added, inputs.size()), true);
    return true;
}

void Logic::pick_replay() {
    GUI::get().open_macro_picker(false, 1);
}

MacroSnap ReplayIO::capture_macro() {
    const auto& logic = Logic::get();
    MacroSnap snap;
    snap.name = logic.macro_name;
    snap.inputs = logic.inputs;
    snap.poses = logic.poses;
    snap.format = logic.format;
    snap.fps = logic.macro_fps;
    snap.end_x = logic.end_x;
    snap.recording_seconds = logic.recording_seconds;
    snap.attempts = logic.attempts;
    snap.save_debug = logic.save_debug;
    snap.lvl_seed = logic.lvl_seed;
    snap.lvl_seed_known = logic.lvl_seed_known;
    snap.unrounded = logic.macro_unrounded;
    snap.silicate_variance = logic.macro_silicate_variance;
    snap.ldm = logic.macro_ldm;
    snap.ldm_known = logic.macro_ldm_known;
    snap.rates = logic.rates;
    snap.highest_cps = logic.highest_cps;
    snap.cps_notes_p1 = logic.cps_notes_p1;
    snap.cps_notes_p2 = logic.cps_notes_p2;
    return snap;
}

void ReplayIO::restore_macro(const MacroSnap& snap) {
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
    logic.macro_ldm = snap.ldm;
    logic.macro_ldm_known = snap.ldm_known;
    logic.highest_cps = snap.highest_cps;
    logic.cps_notes_p1 = snap.cps_notes_p1;
    logic.cps_notes_p2 = snap.cps_notes_p2;
}

bool ReplayIO::is_echo_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    char magic[4] = {};
    file.read(magic, 4);
    return file && magic[0] == 'E' && magic[1] == 'C' && magic[2] == 'H' && magic[3] == '2';
}

std::string ReplayIO::import_stem(const fs::path& path) {
    std::string file = path.filename().string();
    std::string lower = file;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    auto cut = [&](const char* ext) {
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
    auto dir = save_root() / "replays";
    std::error_code error;
    if (!fs::exists(dir, error)) {
        notify("No replays folder yet", false);
        return 0;
    }
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(dir, error)) {
        if (error || !entry.is_regular_file())
            continue;
        files.push_back(entry.path());
    }
    int converted = 0;
    int failed = 0;
    int skipped = 0;
    for (const auto& path : files) {
        std::string lower = path.filename().string();
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return (char)std::tolower(c);
        });
        if (lower.ends_with(".old.echo"))
            continue;
        bool legacy_echo = lower.ends_with(".echo") && !lower.ends_with(".json");
        if (legacy_echo && ReplayIO::is_echo_file(path))
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
        MacroSnap snap;
        if (!ReplayIO::read_any(path, snap)) {
            ++failed;
            continue;
        }
        if (legacy_echo) {
            fs::rename(path, dir / (stem + ".old.echo"), error);
            if (error) {
                ++failed;
                error.clear();
                continue;
            }
        }
        snap.name = stem;
        snap.save_debug = save_debug;
        auto dest = ReplayIO::unique_file(dir, stem, ".echo");
        if (!ReplayIO::save_snap(dest, snap, ReplayIO::write_echo)) {
            ++failed;
            continue;
        }
        ++converted;
    }
    std::string message = fmt::format("Converted {} replay{}", converted, converted == 1 ? "" : "s");
    if (skipped)
        message += fmt::format(", {} already converted", skipped);
    if (failed)
        message += fmt::format(", {} failed", failed);
    notify(message, failed == 0);
    return converted;
}

void Logic::import_replays() {
    using namespace geode::utils::file;
    FilePickOptions options;
    options.defaultPath = save_root() / "replays";
    options.filters.push_back({ "Replays", { "*.echo", "*.json", "*.txt", "*.slc", "*.tcm", "*.gdr" } });
    geode::async::spawn(pickMany(std::move(options)), [](PickManyResult result) {
        if (!result) {
            Logic::get().notify(result.unwrapErr(), false);
            return;
        }
        auto dir = Logic::get().save_root() / "replays";
        int copied = 0;
        int failed = 0;
        for (const auto& path : result.unwrap()) {
            MacroSnap snap;
            if (!ReplayIO::read_any(path, snap)) {
                ++failed;
                continue;
            }
            snap.name = ReplayIO::import_stem(path);
            snap.save_debug = Logic::get().save_debug;
            auto dest = ReplayIO::unique_file(dir, snap.name, ".echo");
            if (!ReplayIO::save_snap(dest, snap, ReplayIO::write_echo)) {
                ++failed;
                continue;
            }
            ++copied;
        }
        std::string message = fmt::format("Imported {} replay{}", copied, copied == 1 ? "" : "s");
        if (failed)
            message += fmt::format(", {} failed", failed);
        Logic::get().notify(message, failed == 0);
    });
}
