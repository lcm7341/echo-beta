#include "Logic/build_info.hpp"
#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>
#include "Hooks/step.hpp"
#include "Logic/logic.hpp"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

using namespace geode::prelude;

#ifdef GEODE_IS_ANDROID64
extern const char* sort_status;
extern unsigned long sort_calls;
#endif

// dumps p1 every step to logs/steps.bin, only w/ the logging setting on
struct TraceStep {
    int frame;
    float x, y;
    float rotation;
    double y_velocity;
    double level_time;
    unsigned char held;
    unsigned char dead;
    unsigned short batch;
    unsigned pad;
};

static_assert(sizeof(TraceStep) == 40);
const size_t TRACE_CAP = 400000;
static std::vector<TraceStep> g_trace_p1;
static std::vector<TraceStep> g_trace_p2;
static std::string g_events;
static std::string g_pace;
static std::string g_header;
static unsigned g_last_ms = 0;
static bool g_finished = false;
const char* const PREFIX[] = { "", "record-", "attempt-" };
static int g_trace_type = 0;
static bool g_began = false;
static bool g_dead_noted = false;

static bool trace_logging_on() {
    auto& logic = Logic::get();
    return logic.trace_logging && !logic.predicting;
}

static std::string trace_header(GJBaseGameLayer* self) {
    auto& logic = Logic::get();
    std::string out = fmt::format("I echo={} build={} geode={} gd={} platform={}\n", Mod::get()->getVersion().toVString(), build_id(),
        Loader::get()->getVersion().toVString(), GEODE_GD_VERSION_STRING, GEODE_PLATFORM_NAME);
    auto win = CCDirector::sharedDirector()->getWinSize();
    auto frame = CCEGLView::sharedOpenGLView()->getFrameSize();
    out += fmt::format("I window={}x{} pixels={}x{} interval_ms={:.3f}\n", win.width, win.height, frame.width, frame.height,
        CCDirector::sharedDirector()->getAnimationInterval() * 1000.0);
    if (auto* play = Logic::as_play(self); play && play->m_level) {
        auto* level = play->m_level;
        auto* settings = play->m_levelSettings;
        out += fmt::format("I level id={} name=\"{}\" version={} objects={} ldm={} two_player={} platformer={} practice={} test_mode={} attempt={}\n",
            level->m_levelID.value(), std::string(level->m_levelName), level->m_levelVersion, play->m_objects ? play->m_objects->count() : 0,
            level->m_lowDetailModeToggled, settings && settings->m_twoPlayerMode, play->m_isPlatformer, play->m_isPracticeMode,
            play->m_isTestMode, play->m_attempts);
    }
    out += fmt::format("I macro name=\"{}\" inputs={} start_tps={:.3f} tps_changes={} ldm_known={} ldm={} seed={} recording={}\n", logic.macro_name,
        logic.inputs.size(), logic.macro_fps, logic.rates.size(), logic.macro_ldm_known, logic.macro_ldm, logic.lvl_seed, logic.is_recording());
    for (auto* mod : Loader::get()->getAllMods())
        if (mod->isLoaded())
            out += fmt::format("M {} {}\n", mod->getID(), mod->getVersion().toVString());
    std::ifstream file(logic.save_root() / "settings.json");
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::erase_if(text, [](char c) {
        return c == '\n' || c == '\r' || c == '\t';
    });
    out += "S " + text + "\n";
    size_t shown = 0;
    for (const auto& input : logic.inputs) {
        if (++shown > 4000) break;
        out += fmt::format("IN frame={} down={} p2={} button={}\n", input.number, input.holding, input.player2, (int)input.button);
    }
    return out;
}

static size_t g_saved_p1 = 0, g_saved_p2 = 0;
static unsigned long long g_steps = 0;
static unsigned g_text_ms = 0;

static void write_rows(const char* name, const std::vector<TraceStep>& rows, size_t& saved) {
    std::ofstream out(Logic::get().save_root() / "logs" / name, std::ios::binary | (saved == 0 ? std::ios::trunc : std::ios::app));
    if (rows.size() > saved)
        out.write(reinterpret_cast<const char*>(rows.data() + saved), (std::streamsize)((rows.size() - saved) * sizeof(TraceStep)));
    saved = rows.size();
}

static bool g_rotate = false;

static void rotate_files() {
    std::string prefix = PREFIX[g_trace_type];
    auto dir = Logic::get().save_root() / "logs";
    for (const char* name : { "buttons.txt", "steps.bin", "steps-p2.bin" }) {
        auto from = dir / (prefix + name);
        auto stem = std::filesystem::path(name).stem().string(), ext = std::filesystem::path(name).extension().string();
        std::error_code ec;
        if (std::filesystem::exists(from, ec))
            std::filesystem::rename(from, dir / (prefix + stem + "-prev" + ext), ec);
    }
}

static void write_out() {
    if (std::exchange(g_rotate, false)) rotate_files();
    {
        std::ofstream events(Logic::get().save_root() / "logs" / (std::string(PREFIX[g_trace_type]) + "buttons.txt"), std::ios::trunc);
        events << g_header << g_events << g_pace;
    }
    write_rows((std::string(PREFIX[g_trace_type]) + "steps.bin").c_str(), g_trace_p1, g_saved_p1);
    write_rows((std::string(PREFIX[g_trace_type]) + "steps-p2.bin").c_str(), g_trace_p2, g_saved_p2);
}
static std::chrono::steady_clock::time_point g_start;
static int g_event_count = 0;
static bool g_written = false;

void trace_note(const std::string& line) {
    if (Logic::get().trace_logging && g_pace.size() < 400000) g_pace += line;
}

bool tracing() {
    return trace_logging_on() && !g_written && g_event_count < 20000;
}

void trace_event(const std::string& line) {
    if (!tracing()) return;
    ++g_event_count;
    g_events += line;
}

void trace_flush() {
    if (Logic::get().trace_logging && g_began && !g_written) write_out();
}

void trace_button(char what, bool down, int extra) {
    auto& logic = Logic::get();
    if (!tracing()) return;
    ++g_event_count;
    g_events += fmt::format("{} frame={} down={} playback={} restoring={} extra={}\n", what, logic.step_frame + 1, down, logic.applying_playback, logic.restoring_holds, extra);
}

void trace_step(GJBaseGameLayer* self) {
    auto& logic = Logic::get();
    auto* player = self->m_player1;
    if (!player || !trace_logging_on()) return;
    // a step back replays the macro up to the frame it wants (state goes to playing for that bit). that is still the
    // same recording, w/out ts every back / best wiped the recordings trace & started a playback one D:
    int trace_type = logic.backstep_target >= 0 && g_began ? g_trace_type : logic.is_recording() ? 1 : logic.is_playing() ? 0 : 2;
    if ((logic.step_frame <= 1 && !self->m_playerDied) || !g_began || trace_type != g_trace_type) {
        g_trace_type = trace_type;
        g_rotate = true;
        g_began = true;
        g_dead_noted = false;
        g_trace_p1.clear();
        g_trace_p2.clear();
        g_events.clear();
        g_pace.clear();
        g_start = std::chrono::steady_clock::now();
        g_event_count = 0;
        g_written = false;
        g_finished = false;
        g_last_ms = 0;
        g_saved_p1 = g_saved_p2 = 0;
        g_steps = 0;
        g_text_ms = 0;
        g_header = trace_header(self);
    }
    if (g_written) return;
    auto ms = (unsigned)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
    if (g_trace_p1.size() < TRACE_CAP) {
        g_trace_p1.push_back({ logic.step_frame, player->getPositionX(), player->getPositionY(), player->getRotation(),
            player->m_yVelocity, self->m_gameState.m_levelTime, Logic::button_held(player, JUMP),
            self->m_playerDied, (unsigned short)(std::min<unsigned>(logic.steps_this_tick, 65535)), ms });
        if (auto* other = self->m_player2)
            g_trace_p2.push_back({ logic.step_frame, other->getPositionX(), other->getPositionY(), other->getRotation(),
                other->m_yVelocity, self->m_gameState.m_levelTime, Logic::button_held(other, JUMP),
                self->m_playerDied, (unsigned short)((self->m_gameState.m_isDualMode ? 1 : 0) | (other->m_isDead ? 2 : 0) | (other->m_isUpsideDown ? 4 : 0) | (other->m_isOnGround ? 8 : 0)), ms });
    }
    auto every = (unsigned long long)std::max(240.0, std::round(logic.sample_rate()));
    if (g_steps++ % every == 0 && g_pace.size() < 200000)
        g_pace += fmt::format("T frame={} ms={} {}\n", logic.step_frame, ms, step_pace());
    {
        if (g_steps % every == 1 && g_pace.size() < 200000) {
            auto* engine = FMODAudioEngine::get();
            unsigned raw = 0;
            if (auto* channel = engine ? engine->getActiveMusicChannel(0) : nullptr)
                channel->getPosition(&raw, FMOD_TIMEUNIT_MS);
            g_pace += fmt::format("U frame={} level_time={:.4f} song={}ms raw_song={}ms x={:.3f}\n", logic.step_frame, self->m_gameState.m_levelTime,
                engine ? engine->getMusicTimeMS(0) : 0, raw, player->getPositionX());
        }
    }
    if (ms > g_last_ms + 100 && g_pace.size() < 200000)
        g_pace += fmt::format("L frame={} ms={} gap_ms={}\n", logic.step_frame, ms, ms - g_last_ms);
    g_last_ms = ms;
    auto* play = Logic::as_play(self);
    bool finished = play && (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) && !g_finished;
    if (finished) {
        g_finished = true;
        g_pace += fmt::format("E frame={} ms={} x={} y={} level complete\n", logic.step_frame, ms, player->getPositionX(), player->getPositionY());
    }
    bool note_death = self->m_playerDied && !g_dead_noted;
    g_dead_noted = self->m_playerDied;
    if (note_death)
        g_pace += fmt::format("D frame={} ms={} x={} y={} yvel={} rot={} upside_down={} mode={} {}\n", logic.step_frame, ms, player->getPositionX(),
            player->getPositionY(), player->m_yVelocity, player->getRotation(), player->m_isUpsideDown,
            player->m_isShip ? "ship" : player->m_isBall ? "ball" : player->m_isBird ? "ufo" : player->m_isDart ? "wave" : player->m_isRobot ? "robot" : player->m_isSpider ? "spider" : player->m_isSwing ? "swing" : "cube",
            step_pace());
    if (note_death)
        if (auto* other = self->m_player2)
            g_pace += fmt::format("D2 dual={} p1_dead={} p2_dead={} p2_x={} p2_y={} p2_yvel={} p2_rot={} p2_upside_down={} p2_mode={}\n", self->m_gameState.m_isDualMode,
                player->m_isDead, other->m_isDead, other->getPositionX(), other->getPositionY(), other->m_yVelocity, other->getRotation(), other->m_isUpsideDown,
                other->m_isShip ? "ship" : other->m_isBall ? "ball" : other->m_isBird ? "ufo" : other->m_isDart ? "wave" : other->m_isRobot ? "robot" : other->m_isSpider ? "spider" : other->m_isSwing ? "swing" : "cube");
    bool full = (g_trace_p1.size() - g_saved_p1 >= 4000 && ms >= g_text_ms + 250) || ms >= g_text_ms + 2000;
    if (full) g_text_ms = ms;
    // ts runs every step thru the whole death animation too. w/ m_playerDied here instead a recording rewrote
    // the whole text log every dead step, thousands of times a death at high tps
    if (!note_death && !full && !finished) return;
    if (self->m_playerDied && g_trace_type != 1) g_written = true;
#ifdef GEODE_IS_ANDROID64
    g_pace += fmt::format("A sort={} calls={}\n", sort_status, sort_calls);
#endif
    write_out();
}
