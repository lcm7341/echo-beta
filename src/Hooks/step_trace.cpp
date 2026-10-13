#include <Geode/Geode.hpp>
#include <Geode/fmod/fmod.hpp>
#include "Hooks/step.hpp"
#include "Logic/logic.hpp"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

#ifdef GEODE_IS_ANDROID64
namespace echo::sort {
    extern char const* status;
    extern unsigned long calls;
}
#endif

// dumps p1 every step to logs/steps.bin, only w/ the logging setting on
namespace {
    struct TraceStep {
        std::int32_t frame;
        float x, y;
        float rotation;
        double yVelocity;
        double levelTime;
        std::uint8_t held;
        std::uint8_t dead;
        std::uint16_t batch;
        std::uint32_t pad;
    };

    static_assert(sizeof(TraceStep) == 40);
    constexpr size_t TRACE_CAP = 400000;
    std::vector<TraceStep> g_trace;
    std::vector<TraceStep> g_trace2; // player 2, same rows (2 player lvls die on EITHER icon)
    std::string g_events;
    std::string g_pace;
    std::string g_head;
    std::uint32_t g_lastMs = 0;
    bool g_finished = false;
    bool g_record = false; // ts trace is a recording, it goes to its own files so playing it back after doesnt wipe it
    bool g_began = false;
    bool g_deadNoted = false;

    bool tracing() {
        auto& logic = Logic::get();
        return logic.trace_logging && (logic.is_playing() || logic.is_recording()) && !logic.predicting;
    }

    // everything about the setup, once per attempt. costs nothing while playing
    std::string head(GJBaseGameLayer* self) {
        auto& logic = Logic::get();
        std::string out = fmt::format("I echo={} geode={} gd={} platform={}\n", Mod::get()->getVersion().toVString(),
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
        // the whole settings file, 1 line
        std::ifstream file(logic.save_root() / "settings.json");
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::erase_if(text, [](char c) {
            return c == '\n' || c == '\r' || c == '\t';
        });
        out += "S " + text + "\n";
        // what the macro holds when the attempt starts. playing = the whole thing, recording = whatever was kept
        size_t shown = 0;
        for (auto const& input : logic.inputs) {
            if (++shown > 4000)
                break;
            out += fmt::format("IN frame={} down={} p2={} button={}\n", input.number, input.holding, input.player2, static_cast<int>(input.button));
        }
        return out;
    }

    size_t g_saved = 0, g_saved2 = 0; // rows already in the files
    std::uint64_t g_steps = 0; // every step, also the ones past the cap
    std::uint32_t g_textMs = 0;

    // only the NEW rows go out. it used to write the whole thing again each time, & once the cap was hit that was
    // 32mb EVERY step (the crazy slow at frame 400k)
    void write_rows(char const* name, std::vector<TraceStep> const& rows, size_t& saved) {
        std::ofstream out(Logic::get().save_root() / "logs" / name, std::ios::binary | (saved == 0 ? std::ios::trunc : std::ios::app));
        if (rows.size() > saved)
            out.write(reinterpret_cast<char const*>(rows.data() + saved), static_cast<std::streamsize>((rows.size() - saved) * sizeof(TraceStep)));
        saved = rows.size();
    }

    void write_out() {
        {
            std::ofstream events(Logic::get().save_root() / "logs" / (g_record ? "record-buttons.txt" : "buttons.txt"), std::ios::trunc);
            events << g_head << g_events << g_pace;
        }
        write_rows(g_record ? "record-steps.bin" : "steps.bin", g_trace, g_saved);
        write_rows(g_record ? "record-steps-p2.bin" : "steps-p2.bin", g_trace2, g_saved2);
    }
    std::chrono::steady_clock::time_point g_start;
    int g_eventCount = 0;
    bool g_written = false;
}

namespace echo::step {
    // a free form line in the trace, for whatever is being chased
    void trace_note(std::string const& line) {
        if (Logic::get().trace_logging && g_pace.size() < 400000)
            g_pace += line;
    }

    bool tracing() {
        return ::tracing() && !g_written && g_eventCount < 20000;
    }

    // same as a button line but free form, & in the same list so the order inside a step is kept
    void trace_event(std::string const& line) {
        if (!tracing())
            return;
        ++g_eventCount;
        g_events += line;
    }

    // pause / quit, so the last second of a recording is in the file
    void trace_flush() {
        if (Logic::get().trace_logging && g_began && !g_written)
            write_out();
    }

    void trace_button(char what, bool down, int extra) {
        auto& logic = Logic::get();
        if (!tracing())
            return;
        ++g_eventCount;
        g_events += fmt::format("{} frame={} down={} playback={} restoring={} extra={}\n", what, logic.step_frame + 1, down, logic.applying_playback, logic.restoring_holds, extra);
    }
}

namespace echo::step {
    void trace(GJBaseGameLayer* self) {
        auto& logic = Logic::get();
        auto* player = self->m_player1;
        if (!player || !::tracing())
            return;
        // a step back replays the macro up to the frame it wants (state goes to playing for that bit). that is still the
        // same recording, w/out ts every back / best wiped the recordings trace & started a playback one D:
        bool record = logic.backstep_target >= 0 && g_began ? g_record : logic.is_recording();
        if ((logic.step_frame <= 1 && !self->m_playerDied) || !g_began || record != g_record) {
            g_record = record;
            g_began = true;
            g_deadNoted = false;
            g_trace.clear();
            g_trace2.clear();
            g_events.clear();
            g_pace.clear();
            g_start = std::chrono::steady_clock::now();
            g_eventCount = 0;
            g_written = false;
            g_finished = false;
            g_lastMs = 0;
            g_saved = g_saved2 = 0;
            g_steps = 0;
            g_textMs = 0;
            g_head = head(self);
        }
        if (g_written)
            return;
        // real ms since the trace began, goes in the old pad field
        auto ms = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
        if (g_trace.size() < TRACE_CAP) {
            g_trace.push_back({ logic.step_frame, player->getPositionX(), player->getPositionY(), player->getRotation(),
                player->m_yVelocity, self->m_gameState.m_levelTime, Logic::button_held(player, static_cast<int>(PlayerButton::Jump)),
                self->m_playerDied, static_cast<std::uint16_t>(std::min<std::uint32_t>(logic.steps_this_tick, 65535)), ms });
            // batch field = what the icon is: 1 dual on, 2 dead, 4 upside down, 8 on ground
            if (auto* other = self->m_player2)
                g_trace2.push_back({ logic.step_frame, other->getPositionX(), other->getPositionY(), other->getRotation(),
                    other->m_yVelocity, self->m_gameState.m_levelTime, Logic::button_held(other, static_cast<int>(PlayerButton::Jump)),
                    self->m_playerDied, static_cast<std::uint16_t>((self->m_gameState.m_isDualMode ? 1 : 0) | (other->m_isDead ? 2 : 0) | (other->m_isUpsideDown ? 4 : 0) | (other->m_isOnGround ? 8 : 0)), ms });
        }
        // 1 line of pacing a (game) second, so a slow run says WHY in the same file
        // per second of LEVEL, at 7220 tps every 240 steps filled the log before the level was half done
        auto every = static_cast<std::uint64_t>(std::max(240.0, std::round(logic.sample_rate())));
        if (g_steps++ % every == 0 && g_pace.size() < 200000)
            g_pace += fmt::format("T frame={} ms={} {}\n", logic.step_frame, ms, echo::step::pace());
        {
            // where the song is against the level, once a game second. a render logs the same numbers (logs/render.txt)
            // so the 2 can be put side by side
            if (g_steps % every == 1 && g_pace.size() < 200000) {
                auto* engine = FMODAudioEngine::get();
                unsigned raw = 0;
                if (auto* channel = engine ? engine->getActiveMusicChannel(0) : nullptr)
                    channel->getPosition(&raw, FMOD_TIMEUNIT_MS);
                g_pace += fmt::format("U frame={} level_time={:.4f} song={}ms raw_song={}ms x={:.3f}\n", logic.step_frame, self->m_gameState.m_levelTime,
                    engine ? engine->getMusicTimeMS(0) : 0, raw, player->getPositionX());
            }
        }
        // a step that waited over 100ms of real time = a lag spike
        if (ms > g_lastMs + 100 && g_pace.size() < 200000)
            g_pace += fmt::format("L frame={} ms={} gap_ms={}\n", logic.step_frame, ms, ms - g_lastMs);
        g_lastMs = ms;
        auto* play = Logic::as_play(self);
        bool finished = play && (play->m_hasCompletedLevel || play->m_levelEndAnimationStarted) && !g_finished;
        if (finished) {
            g_finished = true;
            g_pace += fmt::format("E frame={} ms={} x={} y={} level complete\n", logic.step_frame, ms, player->getPositionX(), player->getPositionY());
        }
        // a recording keeps going after a death (practice mode), so 1 line a death & the trace stays open
        bool note_death = self->m_playerDied && !g_deadNoted;
        g_deadNoted = self->m_playerDied;
        if (note_death)
            g_pace += fmt::format("D frame={} ms={} x={} y={} yvel={} rot={} upside_down={} mode={} {}\n", logic.step_frame, ms, player->getPositionX(),
                player->getPositionY(), player->m_yVelocity, player->getRotation(), player->m_isUpsideDown,
                player->m_isShip ? "ship" : player->m_isBall ? "ball" : player->m_isBird ? "ufo" : player->m_isDart ? "wave" : player->m_isRobot ? "robot" : player->m_isSpider ? "spider" : player->m_isSwing ? "swing" : "cube",
                echo::step::pace());
        if (note_death)
            if (auto* other = self->m_player2)
                g_pace += fmt::format("D2 dual={} p1_dead={} p2_dead={} p2_x={} p2_y={} p2_yvel={} p2_rot={} p2_upside_down={} p2_mode={}\n", self->m_gameState.m_isDualMode,
                    player->m_isDead, other->m_isDead, other->getPositionX(), other->getPositionY(), other->m_yVelocity, other->getRotation(), other->m_isUpsideDown,
                    other->m_isShip ? "ship" : other->m_isBall ? "ball" : other->m_isBird ? "ufo" : other->m_isDart ? "wave" : other->m_isRobot ? "robot" : other->m_isSpider ? "spider" : other->m_isSwing ? "swing" : "cube");
        // 4000 new rows, or (past the cap, where no rows come anymore) the text every 2 real seconds
        bool full = g_trace.size() - g_saved >= 4000 || ms >= g_textMs + 2000;
        if (full)
            g_textMs = ms;
        if (!self->m_playerDied && !full && !finished)
            return;
        if (self->m_playerDied && !g_record)
            g_written = true;
#ifdef GEODE_IS_ANDROID64
        g_pace += fmt::format("A sort={} calls={}\n", echo::sort::status, echo::sort::calls);
#endif
        write_out();
    }
}
