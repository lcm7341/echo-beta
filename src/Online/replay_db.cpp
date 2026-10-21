#include "replay_db.hpp"
#include "Logic/logic.hpp"
#include "Logic/conversions/codecs.hpp"
#include "GUI/gui.hpp"
#include <Geode/fmod/fmod.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>
#include <Geode/utils/file.hpp>
#include <argon/argon.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/LevelBrowserLayer.hpp>
#include <atomic>
#include <charconv>
#include <fstream>
#include <thread>

using namespace geode::prelude;
namespace web = geode::utils::web;

#define SERVER "https://echogd.com"
#define SESSION_KEY "replay_db_session"
#define USERNAME_KEY "replay_db_username"
#define REQUEST_SECONDS 30
#define TRANSFER_SECONDS 300
#define PAGE_SIZE 50

ReplayDb& ReplayDb::get() {
    static ReplayDb db;
    return db;
}

bool ReplayDb::signed_in() {
    return !Mod::get()->getSavedValue<std::string>(SESSION_KEY, "").empty();
}

std::string ReplayDb::username() {
    return Mod::get()->getSavedValue<std::string>(USERNAME_KEY, "");
}

void ReplayDb::fail(const std::string& text) {
    status = text;
    status_bad = true;
}

void ReplayDb::succeed(const std::string& text) {
    status = text;
    status_bad = false;
}

web::WebRequest ReplayDb::request(bool authorized) {
    auto request = web::WebRequest();
    request.timeout(std::chrono::seconds(REQUEST_SECONDS));
    request.userAgent(fmt::format("Echo/{}", Mod::get()->getVersion().toVString()));
    if (!authorized) return request;
    request.followRedirects(false);
    request.header("Authorization", "Bearer " + Mod::get()->getSavedValue<std::string>(SESSION_KEY, ""));
    return request;
}

static std::string read_text(const matjson::Value& value) {
    return value.asString().unwrapOrDefault();
}

static std::string server_error(const web::WebResponse& response) {
    if (auto json = response.json(); json.isOk()) {
        if (auto error = read_text(json.unwrap()["error"]); !error.empty()) return error;
    }
    if (response.code() <= 0) return fmt::format("Could not reach the server ({})", response.errorMessage());
    return fmt::format("Server error {}", response.code());
}

bool ReplayDb::session_ok(const web::WebResponse& response) {
    if (response.code() != 401) return true;
    Mod::get()->setSavedValue<std::string>(SESSION_KEY, "");
    fail("Your sign-in ran out, sign in again");
    return false;
}

void ReplayDb::sign_in() {
    if (!argon::signedIn()) {
        fail("Log into your GD account first (Settings > Account)");
        return;
    }
    signing = true;
    succeed("Checking your GD account...");
    auto account = argon::getGameAccountData();
    auth_task.spawn(argon::startAuth(account), [this, account](Result<std::string> result) {
        if (!result) {
            signing = false;
            argon::clearToken();
            fail(fmt::format("GD account check failed: {}", result.unwrapErr()));
            return;
        }
        auto body = matjson::makeObject({
            { "account_id", account.accountId },
            { "user_id", account.userId },
            { "username", account.username },
            { "token", result.unwrap() },
        });
        login_task.spawn(request(false).bodyJSON(body).post(SERVER "/api/auth/login"), [this](web::WebResponse response) {
            signing = false;
            if (!response.ok()) {
                if (response.code() == 401) argon::clearToken();
                fail(server_error(response));
                return;
            }
            auto json = response.json().unwrapOrDefault();
            auto name = read_text(json["user"]["username"]);
            Mod::get()->setSavedValue<std::string>(SESSION_KEY, read_text(json["session"]));
            Mod::get()->setSavedValue<std::string>(USERNAME_KEY, name);
            succeed(fmt::format("Signed in as {}", name));
        });
    });
}

void ReplayDb::sign_out() {
    auth_task.cancel();
    login_task.cancel();
    signing = false;
    Mod::get()->setSavedValue<std::string>(SESSION_KEY, "");
    Mod::get()->setSavedValue<std::string>(USERNAME_KEY, "");
    web_code.clear();
}

static OnlineReplay read_replay(const matjson::Value& json) {
    OnlineReplay replay;
    replay.id = read_text(json["id"]);
    replay.title = read_text(json["title"]);
    replay.notes = read_text(json["notes"]);
    replay.level_id = (int)json["level"]["id"].asInt().unwrapOrDefault();
    replay.level_name = read_text(json["level"]["name"]);
    replay.tps = (float)json["stats"]["tps"].asDouble().unwrapOrDefault();
    replay.inputs = (int)json["stats"]["inputs"].asInt().unwrapOrDefault();
    replay.physics = read_text(json["stats"]["physics"]);
    replay.uploader = read_text(json["uploader"]["username"]);
    replay.claims_completion = json["claims_completion"].asBool().unwrapOrDefault();
    replay.verified = json["verified"].asBool().unwrapOrDefault();
    replay.downloads = (int)json["downloads"].asInt().unwrapOrDefault();
    return replay;
}

static OnlineClickpack read_clickpack(const matjson::Value& json) {
    const std::pair<const char*, const char*> type_names[] = {
        { "clicks", "clicks" }, { "releases", "releases" }, { "soft_clicks", "softs" }, { "hard_clicks", "hards" }, { "micro_clicks", "micros" },
    };
    OnlineClickpack pack;
    pack.id = read_text(json["id"]);
    pack.name = read_text(json["name"]);
    pack.notes = read_text(json["notes"]);
    pack.uploader = read_text(json["uploader"]["username"]);
    pack.downloads = (int)json["downloads"].asInt().unwrapOrDefault();
    pack.has_releases = json["types"]["releases"].asInt().unwrapOrDefault() > 0;
    for (const auto& [key, label] : type_names) {
        auto count = json["types"][key].asInt().unwrapOrDefault();
        if (count <= 0) continue;
        if (!pack.types.empty()) pack.types += " · ";
        pack.types += fmt::format("{} {}", count, label);
    }
    return pack;
}

static std::array<float, 4> read_hex(const std::string& hex) {
    std::array<float, 4> color{ 0.f, 0.f, 0.f, 1.f };
    const int hex_base = 16;
    for (size_t part = 0; part < 4 && part * 2 + 3 <= hex.size(); ++part) {
        const char* first = hex.data() + 1 + part * 2;
        unsigned value = 0;
        std::from_chars(first, first + 2, value, hex_base);
        color[part] = (float)value / 255.f;
    }
    return color;
}

static OnlineTheme read_theme(const matjson::Value& json) {
    OnlineTheme theme;
    theme.id = read_text(json["id"]);
    theme.name = read_text(json["name"]);
    theme.notes = read_text(json["notes"]);
    theme.uploader = read_text(json["uploader"]["username"]);
    theme.downloads = (int)json["downloads"].asInt().unwrapOrDefault();
    for (const char* key : { "popup", "child", "frame", "button", "check", "text" })
        theme.swatches.push_back(read_hex(read_text(json["preview"]["colors"][key])));
    return theme;
}

void ReplayDb::search(OnlineKind kind, const std::string& query, bool verified_only) {
    const char* const paths[] = { "/api/replays", "/api/clickpacks", "/api/themes" };
    looking = true;
    auto request = this->request(false);
    request.param("q", query);
    request.param("per_page", PAGE_SIZE);
    if (verified_only && kind == OnlineKind::Macros) request.param("verified", "1");
    search_task.spawn(request.get(std::string(SERVER) + paths[(int)kind]), [this, kind](web::WebResponse response) {
        looking = false;
        searched = true;
        results.clear();
        clickpacks.clear();
        themes.clear();
        total = 0;
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        auto json = response.json().unwrapOrDefault();
        const char* const lists[] = { "replays", "clickpacks", "themes" };
        for (const auto& item : json[lists[(int)kind]]) {
            if (kind == OnlineKind::Macros) results.push_back(read_replay(item));
            else if (kind == OnlineKind::Clickpacks) clickpacks.push_back(read_clickpack(item));
            else themes.push_back(read_theme(item));
        }
        total = (int)json["total"].asInt().unwrapOrDefault();
        if (status_bad) succeed("");
    });
}

static std::string file_title(const OnlineReplay& replay) {
    if (replay.title == replay.level_name || replay.title.empty()) return fmt::format("{} ({})", replay.level_name, replay.uploader);
    return fmt::format("{} - {} ({})", replay.level_name, replay.title, replay.uploader);
}

static void load_temporary(std::vector<unsigned char> bytes, std::string title, std::string id) {
    auto& logic = Logic::get();
    auto folder = logic.save_root() / "online-temp";
    std::error_code error;
    fs::create_directories(folder, error);
    auto path = folder / ReplayIO::utf8_path(id + ".echo");
    {
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            logic.notify("Could not open a temporary file for the macro", false);
            return;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
    }
    static std::atomic<unsigned> ticket{ 0 };
    unsigned mine = ticket.fetch_add(1) + 1;
    logic.notify(fmt::format("Loading {}", title), true);
    std::thread([path, title, mine] {
        auto macro = ReplayIO::load_macro(path);
        std::error_code ignored;
        fs::remove(path, ignored);
        geode::queueInMainThread([title, mine, macro = std::move(macro)]() mutable {
            if (mine != ticket.load()) return;
            auto& logic = Logic::get();
            if (!macro) {
                logic.notify("That online macro could not be read", false);
                return;
            }
            auto amount = macro->inputs.size();
            ReplayIO::restore_macro(macro.value());
            logic.macro_name = title;
            logic.macro_name_typed = false;
            logic.macro_path.clear();
            logic.autosaved_path.clear();
            ReplayIO::finish_load(fmt::format("Loaded {} ({} inputs, not saved)", title, amount));
            logic.recompute_cps();
        });
    }).detach();
}

void ReplayDb::download(const OnlineReplay& replay, bool keep) {
    fetching = true;
    succeed(fmt::format("Downloading {}...", replay.title));
    auto request = this->request(false);
    request.timeout(std::chrono::seconds(TRANSFER_SECONDS));
    auto url = fmt::format("{}/api/replays/{}/file", SERVER, replay.id);
    download_task.spawn(request.get(url), [this, replay, keep](web::WebResponse response) {
        fetching = false;
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        auto bytes = std::move(response).data();
        auto title = file_title(replay);
        if (!keep) {
            succeed("");
            load_temporary(std::move(bytes), title, replay.id);
            return;
        }
        auto& logic = Logic::get();
        auto folder = logic.save_root() / "replays";
        std::error_code error;
        fs::create_directories(folder, error);
        auto path = ReplayIO::unique_file(folder, ReplayIO::sanitize(title), ".echo");
        std::ofstream file(path, std::ios::binary);
        if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size())) {
            fail("Could not write the macro into your replays folder");
            return;
        }
        file.close();
        succeed(fmt::format("Saved {}", geode::utils::string::pathToString(path.filename())));
        logic.read_file(path);
    });
}

void ReplayDb::play(const OnlineReplay& replay) {
    download(replay, false);
}

void ReplayDb::save(const OnlineReplay& replay) {
    download(replay, true);
}

static GJGameLevel* current_level() {
    if (auto* play = PlayLayer::get()) return play->m_level;
    if (auto* editor = LevelEditorLayer::get()) return editor->m_level;
    return nullptr;
}

bool ReplayDb::open_level(std::string& name, int& id) {
    auto* level = current_level();
    if (!level) return false;
    name = level->m_levelName;
    id = std::max(level->m_levelID.value(), 0);
    return true;
}

static std::string comparable(const std::string& text) {
    std::string out;
    for (unsigned char c : text) {
        if (std::isalnum(c)) out += (char)std::tolower(c);
        else if (c >= 128) out += (char)c;
    }
    return out;
}

static size_t edit_distance(const std::string& a, const std::string& b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t above = row[j];
            row[j] = std::min({ row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1) });
            diagonal = above;
        }
    }
    return row[b.size()];
}

bool ReplayDb::names_match(const std::string& macro_name, const std::string& level_name) {
    const double close_enough = .75;
    auto macro = comparable(macro_name), level = comparable(level_name);
    if (macro.empty() || level.empty()) return false;
    if (macro.find(level) != std::string::npos || level.find(macro) != std::string::npos) return true;
    size_t longest = std::max(macro.size(), level.size());
    return 1.0 - (double)edit_distance(macro, level) / (double)longest >= close_enough;
}

static std::string base64url(const std::string& text) {
    const char* const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (unsigned char c : text) {
        buffer = (buffer << 8) | c;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out += alphabet[(buffer >> bits) & 63];
        }
    }
    if (bits > 0) out += alphabet[(buffer << (6 - bits)) & 63];
    return out;
}

void ReplayDb::upload(const std::string& title, const std::string& notes, bool claims_completion) {
    auto& logic = Logic::get();
    if (logic.inputs.empty()) {
        fail("Record or load a macro first");
        return;
    }
    auto* level = current_level();
    if (!level) {
        fail("Open the level this macro is for, then upload");
        return;
    }
    auto folder = logic.save_root() / "online-temp";
    std::error_code error;
    fs::create_directories(folder, error);
    auto path = folder / "upload.echo";
    if (!ReplayIO::save_as(path, ReplayIO::write_echo)) {
        fail("Could not pack the macro for uploading");
        return;
    }
    auto bytes = ReplayIO::read_all(path);
    fs::remove(path, error);
    int last_frame = logic.inputs.back().number;
    auto meta = matjson::makeObject({
        { "title", title.empty() ? std::string(level->m_levelName) : title },
        { "notes", notes },
        { "level_id", std::max(level->m_levelID.value(), 0) },
        { "level_name", std::string(level->m_levelName) },
        { "level_creator", std::string(level->m_creatorName) },
        { "tps", logic.macro_fps },
        { "input_count", (int)logic.inputs.size() },
        { "duration_seconds", logic.seconds_at(last_frame) },
        { "attempts", 1 },
        { "physics", "2.2" },
        { "echo_version", Mod::get()->getVersion().toVString() },
        { "claims_completion", claims_completion },
    });
    send_file("/api/replays", "X-Replay-Meta", meta.dump(matjson::NO_INDENTATION), std::move(bytes));
}

void ReplayDb::send_file(const std::string& path, const char* meta_header, const std::string& meta, std::vector<unsigned char> bytes) {
    sending = true;
    succeed("Uploading...");
    auto request = this->request(true);
    request.timeout(std::chrono::seconds(TRANSFER_SECONDS));
    request.header("Content-Type", "application/octet-stream");
    request.header(meta_header, base64url(meta));
    request.body(ByteVector(bytes.begin(), bytes.end()));
    upload_task.spawn(request.post(SERVER + path), [this](web::WebResponse response) {
        sending = false;
        if (!session_ok(response)) return;
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        auto json = response.json().unwrapOrDefault();
        if (json.contains("replay")) succeed(fmt::format("Uploaded {} (ID {})", read_text(json["replay"]["title"]), read_text(json["replay"]["id"])));
        for (const char* key : { "clickpack", "theme" }) {
            if (!json.contains(key)) continue;
            succeed(fmt::format("Uploaded {} (ID {})", read_text(json[key]["name"]), read_text(json[key]["id"])));
        }
    });
}

static FMOD::Sound* g_preview_sound = nullptr;

static void play_from_memory(const ByteVector& bytes) {
    auto* system = FMODAudioEngine::get()->m_system;
    if (g_preview_sound) g_preview_sound->release();
    g_preview_sound = nullptr;
    FMOD_CREATESOUNDEXINFO info{};
    info.cbsize = sizeof(info);
    info.length = (unsigned)bytes.size();
    FMOD_MODE mode = FMOD_OPENMEMORY | FMOD_LOOP_OFF | FMOD_2D;
    if (system->createSound(reinterpret_cast<const char*>(bytes.data()), mode, &info, &g_preview_sound) != FMOD_OK) {
        g_preview_sound = nullptr;
        return;
    }
    FMOD::ChannelGroup* group = nullptr;
    system->getMasterChannelGroup(&group);
    system->playSound(g_preview_sound, group, false, nullptr);
}

void ReplayDb::preview_sound(const OnlineClickpack& pack, const char* type) {
    auto url = fmt::format("{}/api/clickpacks/{}/preview/{}", SERVER, pack.id, type);
    preview_task.spawn(request(false).get(url), [this](web::WebResponse response) {
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        play_from_memory(response.data());
    });
}

std::vector<std::string> ReplayDb::installed_clickpacks() {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : fs::directory_iterator(Logic::get().save_root() / "clickpacks", error)) {
        if (entry.is_directory(error)) names.push_back(geode::utils::string::pathToString(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

static fs::path unused_folder(const fs::path& root, const std::string& base) {
    std::error_code error;
    auto path = root / ReplayIO::utf8_path(base);
    for (int copy = 2; fs::exists(path, error); ++copy) path = root / ReplayIO::utf8_path(fmt::format("{} ({})", base, copy));
    return path;
}

static fs::path entry_path(const fs::path& folder, const std::string& name) {
    fs::path path = folder;
    size_t start = 0;
    while (start <= name.size()) {
        size_t end = name.find_first_of("/\\", start);
        if (end == std::string::npos) end = name.size();
        auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return {};
        path /= ReplayIO::utf8_path(ReplayIO::sanitize(part));
        start = end + 1;
    }
    return path;
}

static std::string extract_clickpack(geode::utils::file::Unzip& unzip, const fs::path& folder) {
    std::error_code error;
    fs::create_directories(folder, error);
    for (const auto& entry : unzip.getEntries()) {
        auto name = geode::utils::string::pathToString(entry);
        if (name.ends_with('/') || name.ends_with('\\')) continue;
        auto path = entry_path(folder, name);
        if (path.empty()) return "it has a bad file name";
        auto data = unzip.extract(entry);
        if (!data) return fmt::format("could not unpack {}", name);
        auto bytes = std::move(data).unwrap();
        fs::create_directories(path.parent_path(), error);
        std::ofstream file(path, std::ios::binary);
        if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size()))
            return "its files could not be written";
    }
    return "";
}

void ReplayDb::install_clickpack(const OnlineClickpack& pack, int use_for_player) {
    fetching = true;
    succeed(fmt::format("Downloading {}...", pack.name));
    auto request = this->request(false);
    request.timeout(std::chrono::seconds(TRANSFER_SECONDS));
    download_task.spawn(request.get(fmt::format("{}/api/clickpacks/{}/file", SERVER, pack.id)), [this, pack, use_for_player](web::WebResponse response) {
        fetching = false;
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        auto bytes = std::move(response).data();
        auto& logic = Logic::get();
        auto root = logic.save_root() / "clickpacks";
        std::error_code error;
        fs::create_directories(root, error);
        auto folder = unused_folder(root, ReplayIO::sanitize(pack.name));
        std::string problem;
        try {
            auto unzip = geode::utils::file::Unzip::create(ByteSpan(bytes.data(), bytes.size()));
            if (unzip) problem = extract_clickpack(unzip.unwrap(), folder);
            else problem = "the download was broken";
        }
        catch (...) {
            problem = "it could not be unpacked";
        }
        if (!problem.empty()) {
            fs::remove_all(folder, error);
            fail(fmt::format("Could not install the clickpack: {}", problem));
            return;
        }
        auto name = geode::utils::string::pathToString(folder.filename());
        if (use_for_player == 1) logic.clickpack = geode::utils::string::pathToString(folder);
        if (use_for_player == 2) logic.clickpack_p2 = geode::utils::string::pathToString(folder);
        if (use_for_player != 0) logic.push_runtime_settings();
        if (use_for_player == 0) succeed(fmt::format("Installed {}", name));
        else succeed(fmt::format("Installed {} and set it for player {}", name, use_for_player));
    });
}

void ReplayDb::upload_clickpack(const std::string& folder, const std::string& name, const std::string& notes) {
    auto path = Logic::get().save_root() / "clickpacks" / ReplayIO::utf8_path(folder);
    auto zip = geode::utils::file::Zip::create();
    if (!zip || !zip.unwrap().addAllFrom(path)) {
        fail("Could not pack that clickpack");
        return;
    }
    auto data = zip.unwrap().getData();
    auto meta = matjson::makeObject({ { "name", name.empty() ? folder : name }, { "notes", notes } });
    send_file("/api/clickpacks", "X-Clickpack-Meta", meta.dump(matjson::NO_INDENTATION), std::vector<unsigned char>(data.begin(), data.end()));
}

void ReplayDb::apply_theme(const OnlineTheme& theme) {
    fetching = true;
    download_task.spawn(request(false).get(fmt::format("{}/api/themes/{}/file", SERVER, theme.id)), [this, theme](web::WebResponse response) {
        fetching = false;
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        auto text = response.string().unwrapOrDefault();
        auto folder = Logic::get().save_root() / "themes";
        std::error_code error;
        fs::create_directories(folder, error);
        auto path = ReplayIO::unique_file(folder, ReplayIO::sanitize(theme.name), ".ui.json");
        {
            std::ofstream file(path, std::ios::binary);
            if (!file || !file.write(text.data(), (std::streamsize)text.size())) {
                fail("Could not save the theme");
                return;
            }
        }
        GUI::get().import_theme(path);
        succeed(fmt::format("Applied {}", theme.name));
    });
}

void ReplayDb::upload_theme(const std::string& name, const std::string& notes) {
    auto text = GUI::get().style_json().dump(matjson::NO_INDENTATION);
    auto meta = matjson::makeObject({ { "name", name.empty() ? Logic::get().theme_name : name }, { "notes", notes } });
    send_file("/api/themes", "X-Theme-Meta", meta.dump(matjson::NO_INDENTATION), std::vector<unsigned char>(text.begin(), text.end()));
}

void ReplayDb::open_page(const std::string& path) {
    geode::utils::web::openLinkInBrowser(SERVER + path);
}

void ReplayDb::request_web_code() {
    code_task.spawn(request(true).bodyJSON(matjson::makeObject({})).post(SERVER "/api/auth/web-code"), [this](web::WebResponse response) {
        if (!session_ok(response)) return;
        if (!response.ok()) {
            fail(server_error(response));
            return;
        }
        web_code = read_text(response.json().unwrapOrDefault()["code"]);
        geode::utils::clipboard::write(web_code);
        web_code_ready = true;
        succeed("");
    });
}

void ReplayDb::open_website() {
    geode::utils::web::openLinkInBrowser(SERVER);
}

void ReplayDb::open_sign_in_page() {
    geode::utils::web::openLinkInBrowser(SERVER "/login");
}

void ReplayDb::open_on_website(const OnlineReplay& replay) {
    geode::utils::web::openLinkInBrowser(fmt::format("{}/r/{}", SERVER, replay.id));
}

bool ReplayDb::can_go_to_level(const OnlineReplay& replay) {
    return replay.level_id > 0 && !PlayLayer::get() && !LevelEditorLayer::get();
}

void ReplayDb::go_to_level(const OnlineReplay& replay) {
    const float fade_seconds = .5f;
    auto* search = GJSearchObject::create(SearchType::Search, std::to_string(replay.level_id));
    CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(fade_seconds, LevelBrowserLayer::scene(search)));
}

void ReplayDb::copy_level_id(const OnlineReplay& replay) {
    geode::utils::clipboard::write(std::to_string(replay.level_id));
    succeed(fmt::format("Copied level ID {}", replay.level_id));
}
