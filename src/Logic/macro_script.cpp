#include "Logic/macro_script.hpp"
#include "GUI/gui.hpp"
#include "Hack/label_types.hpp"
#include "Hack/script_labels.hpp"
#include "Hack/trajectory.hpp"
#include "Logic/conversions/codecs.hpp"
#include "Logic/logic.hpp"
#include "Script/game_access.hpp"
#include "Script/host.hpp"
#include "Script/host_helpers.hpp"
#include "Script/lua_runtime.hpp"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace geode::prelude;
using Script::read_text;
using Script::set_bool;
using Script::set_number;
using Script::set_string;
using Script::take_function;

#define MACRO_MEMORY_LIMIT 268435456ull
#define RUN_INSTRUCTIONS 1000000000
#define CALLBACK_INSTRUCTIONS 5000000
#define MAX_NEW_FILES 1000
#define SNAPSHOT_MOST_INPUTS 20000000
#define TOUCH_RADIUS 60.f
#define MAX_OBJECT_RADIUS 3000.f
#define MAX_LISTED_OBJECTS 256
#define DEFAULT_SECTION_FACTOR 0.01f
#define MAX_PREDICTED_STEPS 2400
#define PREDICTED_STEPS_PER_CALL 20000
#define BUNDLED_MACRO_PREFIX "echo-macro-"
#define GUIDE_FILE "echo-macro-guide.md"
#define MOST_PRINT_LINES 40
#define MOST_SCRIPTS 32

namespace MacroScript {
    bool g_running = false;
    bool g_before_step = false;
}

enum class Callback {
    Step,
    BeforeStep,
    Attempt,
    Death,
    Complete,
    Count
};

static const char* const NEW_MACRO_SCRIPT = R"(print("hi from " .. script.name))

function attempt(number)
    print("attempt " .. number)
end
)";

static const char* const CALLBACK_NAMES[] = { "step", "before_step", "attempt", "death", "complete" };
static_assert(std::size(CALLBACK_NAMES) == (size_t)Callback::Count);

struct MacroSnapshot {
    bool taken = false;
    bool whole = false;
    std::vector<Frame> inputs;
    std::vector<FramePose> poses;
    std::vector<unsigned long long> ids;
};

struct NearObject {
    GameObject* object = nullptr;
    const char* type = nullptr;
    CCRect box;
    float distance = 0.f;
    bool touching = false;
};

struct RunningScript {
    int label_owner = 0;
    std::unique_ptr<Script::Runtime> runtime;
    int callbacks[(int)Callback::Count] = { LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF };
    fs::path file;
    std::string name;
    bool holds[2][RIGHT + 1] = {};
    bool stopping = false;
    std::string print_label;
    int print_lines = 1;
    std::deque<std::string> printed;
};

static std::vector<std::unique_ptr<RunningScript>> g_scripts;
static RunningScript* g_active = nullptr;
static std::string g_error;
static std::deque<std::string> g_output;
static PlayLayer* g_play = nullptr;
static bool g_can_predict = false;
static int g_predicted_steps = 0;
static MacroSnapshot g_snapshot;
static std::vector<NearObject> g_near;
static std::chrono::steady_clock::time_point g_started = std::chrono::steady_clock::now();

static fs::path scripts_folder() {
    return Logic::get().save_root() / "macro scripts";
}

static std::string chunk_name(const fs::path& file) {
    return Script::chunk_name(file, scripts_folder());
}

static int label_owner_of(const fs::path& file) {
    return (int)std::hash<std::string>{}(geode::utils::string::pathToString(file));
}

static void add_output(const std::string& text) {
    Script::add_output(g_output, text);
}

static void report(const std::string& message) {
    log::warn("[macro script] {}", message);
    g_error = message.substr(0, message.find('\n'));
    add_output(message);
}

static void print_from(RunningScript& script, const std::string& text) {
    if (script.print_label.empty()) {
        add_output(text);
        return;
    }
    script.printed.push_back(text.substr(0, MAX_OUTPUT_LINE_BYTES));
    while ((int)script.printed.size() > script.print_lines)
        script.printed.pop_front();
    std::string shown;
    for (const auto& line : script.printed)
        shown += (shown.empty() ? "" : "\n") + line;
    ScriptLabels::set(script.label_owner, script.print_label, shown, nullptr);
}

static RunningScript& active_script(lua_State* lua) {
    if (g_active && g_active->runtime && g_active->runtime->state() == lua) return *g_active;
    for (auto& script : g_scripts) {
        if (script->runtime && script->runtime->state() == lua) return *script;
    }
    luaL_error(lua, "this script is not running");
    return *g_scripts.front();
}

static void refresh_flags() {
    MacroScript::g_running = !g_scripts.empty();
    MacroScript::g_before_step = false;
    std::vector<std::string> names;
    for (const auto& script : g_scripts) {
        MacroScript::g_before_step = MacroScript::g_before_step || script->callbacks[(int)Callback::BeforeStep] != LUA_NOREF;
        names.push_back(script->name);
    }
    ScriptLabels::set_running_scripts(names);
}

static void queue_button(PlayLayer* play, int button, bool down, bool player2) {
    auto& logic = Logic::get();
    if (g_active) g_active->holds[player2 ? 1 : 0][button] = down;
    if (button == JUMP) logic.queue_tool_click(play, down, player2);
    else play->queueButton(button, down, player2, logic.click_stamp());
}

static void let_go_of_held_buttons(RunningScript& script) {
    auto& logic = Logic::get();
    PlayLayer* play = PlayLayer::get();
    bool only_playing = logic.is_playing() && !logic.is_recording();
    for (int side = 0; side < 2; ++side) {
        PlayerObject* player = play && !only_playing && !logic.plays_player(side == 1) ? (side == 1 ? play->m_player2 : play->m_player1) : nullptr;
        for (int button : { JUMP, LEFT, RIGHT }) {
            if (!script.holds[side][button]) continue;
            script.holds[side][button] = false;
            if (player && !play->m_playerDied && Logic::button_held(player, button)) queue_button(play, button, false, side == 1);
        }
    }
}

static void stop_script(RunningScript& script, bool keep_labels) {
    for (size_t i = 0; i < g_scripts.size(); ++i) {
        if (g_scripts[i].get() != &script) continue;
        std::unique_ptr<RunningScript> gone = std::move(g_scripts[i]);
        g_scripts.erase(g_scripts.begin() + (long)i);
        let_go_of_held_buttons(*gone);
        if (!keep_labels) ScriptLabels::clear(gone->label_owner);
        if (g_active == gone.get()) g_active = nullptr;
        break;
    }
    refresh_flags();
}

static void stop_file(const fs::path& file) {
    for (size_t i = g_scripts.size(); i-- > 0;) {
        if (g_scripts[i]->file == file) stop_script(*g_scripts[i], false);
    }
    ScriptLabels::clear(label_owner_of(file));
}

static void stop_everything() {
    while (!g_scripts.empty())
        stop_script(*g_scripts.back(), false);
    ScriptLabels::clear_all();
}

static std::string bundled_name(const fs::path& file) {
    return Script::bundled_name(file, BUNDLED_MACRO_PREFIX);
}

static fs::path installed_file(const fs::path& example) {
    return scripts_folder() / ReplayIO::utf8_path(bundled_name(example) + ".lua");
}

static void copy_guide(const fs::path& folder) {
    std::error_code error;
    fs::copy_file(Mod::get()->getResourcesDir() / GUIDE_FILE, folder / "Macro Scripting Guide.md", fs::copy_options::overwrite_existing, error);
}

static bool copy_script(const fs::path& from, const fs::path& made) {
    std::error_code error;
    fs::create_directories(made.parent_path(), error);
    if (error) return false;
    if (!fs::exists(made, error)) fs::copy_file(from, made, error);
    if (error) return false;
    copy_guide(made.parent_path());
    return true;
}

static PlayLayer* current_play() {
    return g_play ? g_play : PlayLayer::get();
}

static bool two_player(PlayLayer* play) {
    return play->m_levelSettings && play->m_levelSettings->m_twoPlayerMode;
}

static PlayerObject* player_number(PlayLayer* play, int number) {
    if (!play) return nullptr;
    if (number == 2) return play->m_gameState.m_isDualMode ? play->m_player2 : nullptr;
    return play->m_player1;
}

static int player_argument(lua_State* lua, int index) {
    lua_Integer number = luaL_optinteger(lua, index, 1);
    if (number != 1 && number != 2) luaL_argerror(lua, index, "the player is 1 or 2");
    return (int)number;
}

template <class Apply>
static void read_number_field(lua_State* lua, int table, const char* what, const char* key, Apply&& apply) {
    if (lua_getfield(lua, table, key) != LUA_TNIL) {
        if (!lua_isnumber(lua, -1)) luaL_error(lua, "%s %s is a number", what, key);
        apply(lua_tonumber(lua, -1));
    }
    lua_pop(lua, 1);
}

static void set_integer(lua_State* lua, const char* key, lua_Integer value) {
    lua_pushinteger(lua, value);
    lua_setfield(lua, -2, key);
}

static const char* button_name(int button) {
    if (button == LEFT) return "left";
    if (button == RIGHT) return "right";
    return "jump";
}

static int button_named(std::string_view name) {
    if (name == "jump") return JUMP;
    if (name == "left") return LEFT;
    if (name == "right") return RIGHT;
    return 0;
}

static const char* mode_name(PlayerObject* player) {
    if (player->m_isShip) return "ship";
    if (player->m_isBall) return "ball";
    if (player->m_isBird) return "ufo";
    if (player->m_isDart) return "wave";
    if (player->m_isRobot) return "robot";
    if (player->m_isSpider) return "spider";
    if (player->m_isSwing) return "swing";
    return "cube";
}

static const char* object_type_name(GameObject* object) {
    switch (object->m_objectType) {
    case GameObjectType::YellowJumpRing: return "yellow_orb";
    case GameObjectType::PinkJumpRing: return "pink_orb";
    case GameObjectType::RedJumpRing: return "red_orb";
    case GameObjectType::GravityRing: return "blue_orb";
    case GameObjectType::GreenRing: return "green_orb";
    case GameObjectType::DropRing: return "black_orb";
    case GameObjectType::DashRing: return "dash_orb";
    case GameObjectType::GravityDashRing: return "pink_dash_orb";
    case GameObjectType::SpiderOrb: return "spider_orb";
    case GameObjectType::TeleportOrb: return "teleport_orb";
    case GameObjectType::CustomRing: return "toggle_orb";
    case GameObjectType::YellowJumpPad: return "yellow_pad";
    case GameObjectType::PinkJumpPad: return "pink_pad";
    case GameObjectType::RedJumpPad: return "red_pad";
    case GameObjectType::GravityPad: return "blue_pad";
    case GameObjectType::SpiderPad: return "spider_pad";
    case GameObjectType::CubePortal: return "cube_portal";
    case GameObjectType::ShipPortal: return "ship_portal";
    case GameObjectType::BallPortal: return "ball_portal";
    case GameObjectType::UfoPortal: return "ufo_portal";
    case GameObjectType::WavePortal: return "wave_portal";
    case GameObjectType::RobotPortal: return "robot_portal";
    case GameObjectType::SpiderPortal: return "spider_portal";
    case GameObjectType::SwingPortal: return "swing_portal";
    case GameObjectType::NormalGravityPortal: return "blue_gravity_portal";
    case GameObjectType::InverseGravityPortal: return "yellow_gravity_portal";
    case GameObjectType::GravityTogglePortal: return "green_gravity_portal";
    case GameObjectType::MiniSizePortal: return "mini_portal";
    case GameObjectType::RegularSizePortal: return "big_portal";
    case GameObjectType::InverseMirrorPortal: return "mirror_portal";
    case GameObjectType::NormalMirrorPortal: return "unmirror_portal";
    case GameObjectType::DualPortal: return "dual_portal";
    case GameObjectType::SoloPortal: return "solo_portal";
    case GameObjectType::TeleportPortal: return "teleport_portal";
    case GameObjectType::Hazard: return "hazard";
    case GameObjectType::AnimatedHazard: return "hazard";
    case GameObjectType::Solid: return "block";
    case GameObjectType::Breakable: return "block";
    case GameObjectType::Slope: return "slope";
    case GameObjectType::SecretCoin: return "coin";
    case GameObjectType::UserCoin: return "coin";
    default: return nullptr;
    }
}

static bool type_matches(const char* type, std::string_view wanted) {
    std::string_view name = type;
    if (name == wanted) return true;
    if (wanted == "orb" || wanted == "pad" || wanted == "portal") return name.ends_with(std::string("_") + std::string(wanted));
    return false;
}

static void collect_objects(PlayLayer* play, PlayerObject* player, float radius, std::vector<NearObject>& found) {
    found.clear();
    CCPoint center = player->getPosition();
    CCRect body = Logic::object_hitbox(player);
    float body_reach = body.size.width + body.size.height;
    float x_factor = play->m_sectionXFactor > 0.f ? play->m_sectionXFactor : DEFAULT_SECTION_FACTOR;
    auto& sections = play->m_sections;
    auto& sizes = play->m_sectionSizes;
    int first_x = std::max((int)std::floor((center.x - radius) * x_factor) - 1, 0);
    int last_x = std::min((int)std::floor((center.x + radius) * x_factor) + 1, (int)std::min(sections.size(), sizes.size()) - 1);
    for (int x = first_x; x <= last_x; ++x) {
        auto* column = sections[(size_t)x];
        auto* counts = sizes[(size_t)x];
        if (!column || !counts) continue;
        int rows = (int)std::min(column->size(), counts->size());
        for (int y = 0; y < rows; ++y) {
            auto* bucket = (*column)[(size_t)y];
            if (!bucket) continue;
            int object_count = std::min((*counts)[(size_t)y], (int)bucket->size());
            for (int i = 0; i < object_count; ++i) {
                GameObject* object = (*bucket)[(size_t)i];
                if (!object || object->m_isDisabled || object->m_isGroupDisabled) continue;
                const char* type = object_type_name(object);
                if (!type) continue;
                float distance = ccpDistance(center, CCPoint((float)object->m_positionX, (float)object->m_positionY));
                float scale = std::max(std::fabs(object->m_scaleX), std::fabs(object->m_scaleY));
                float reach = (object->m_width + object->m_height + object->m_objectRadius) * scale + body_reach;
                if (distance > radius && distance > reach) continue;
                CCRect box = Logic::object_hitbox(object);
                bool touching = body.intersectsRect(box);
                if (distance <= radius || touching) found.push_back({ object, type, box, distance, touching });
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const NearObject& a, const NearObject& b) {
        return a.distance < b.distance;
    });
}

static bool orb_used(const NearObject& nearby, int number) {
    if (!std::string_view(nearby.type).ends_with("_orb")) return false;
    auto* ring = static_cast<EnhancedGameObject*>(nearby.object);
    return number == 2 ? ring->m_activatedByPlayer2 : ring->m_activatedByPlayer1;
}

static float level_percent(PlayLayer* play) {
    if (!play->m_player1 || !play->m_endPortal) return 0.f;
    float end_x = play->m_endPortal->getPositionX();
    return end_x == 0.f ? 0.f : std::min(100.f, play->m_player1->getPositionX() / end_x * 100.f);
}

struct ButtonChoice {
    int button = JUMP;
    bool player2 = false;
};

static ButtonChoice read_button(lua_State* lua) {
    ButtonChoice choice;
    for (int i = 1; i <= lua_gettop(lua); ++i) {
        int type = lua_type(lua, i);
        if (type == LUA_TNUMBER) {
            lua_Integer number = lua_tointeger(lua, i);
            if (number != 1 && number != 2) luaL_argerror(lua, i, "the player is 1 or 2");
            choice.player2 = number == 2;
        }
        else if (type == LUA_TSTRING) {
            choice.button = button_named(lua_tostring(lua, i));
            if (!choice.button) luaL_argerror(lua, i, "the button is \"jump\", \"left\" or \"right\"");
        }
        else if (type != LUA_TNIL) {
            luaL_argerror(lua, i, "expected a button name or a player number");
        }
    }
    return choice;
}

static bool can_press(lua_State* lua, bool player2) {
    if (!g_play) luaL_error(lua, "press, release and click only work inside step, before_step, attempt, death and complete");
    auto& logic = Logic::get();
    PlayLayer* play = g_play;
    if (play->m_isPaused || play->m_playerDied) return false;
    if (logic.is_playing() && !logic.is_recording()) return false;
    if (logic.plays_player(player2)) return false;
    if (player2 && !(two_player(play) && play->m_gameState.m_isDualMode)) return false;
    PlayerObject* player = player2 ? play->m_player2 : play->m_player1;
    return player && !player->m_isDead;
}

static int api_press(lua_State* lua) {
    ButtonChoice choice = read_button(lua);
    bool pressed = can_press(lua, choice.player2);
    if (pressed) queue_button(g_play, choice.button, true, choice.player2);
    lua_pushboolean(lua, pressed);
    return 1;
}

static int api_release(lua_State* lua) {
    ButtonChoice choice = read_button(lua);
    bool released = can_press(lua, choice.player2);
    if (released) queue_button(g_play, choice.button, false, choice.player2);
    lua_pushboolean(lua, released);
    return 1;
}

static int api_click(lua_State* lua) {
    ButtonChoice choice = read_button(lua);
    bool clicked = can_press(lua, choice.player2);
    if (clicked) {
        queue_button(g_play, choice.button, true, choice.player2);
        queue_button(g_play, choice.button, false, choice.player2);
    }
    lua_pushboolean(lua, clicked);
    return 1;
}

static int api_survives(lua_State* lua) {
    if (!g_can_predict) luaL_error(lua, "survives only works inside before_step");
    std::string_view action = luaL_checkstring(lua, 1);
    ClickType click = ClickType::Release;
    if (action == "hold") click = ClickType::Hold;
    else if (action == "click") click = ClickType::Swift;
    else if (action != "release") luaL_argerror(lua, 1, "the action is \"hold\", \"release\" or \"click\"");
    lua_Integer steps = luaL_checkinteger(lua, 2);
    if (steps < 1 || steps > MAX_PREDICTED_STEPS) luaL_argerror(lua, 2, lua_pushfstring(lua, "steps go from 1 to %d", MAX_PREDICTED_STEPS));
    lua_Integer number = luaL_optinteger(lua, 3, 1);
    if (number != 1 && number != 2) luaL_argerror(lua, 3, "the player is 1 or 2");
    if (g_predicted_steps + steps > PREDICTED_STEPS_PER_CALL)
        luaL_error(lua, "survives can look at most %d steps ahead in all during one before_step", PREDICTED_STEPS_PER_CALL);
    g_predicted_steps += (int)steps;
    PlayLayer* play = g_play;
    bool player2 = number == 2 && two_player(play);
    if (player2 && !(play->m_player2 && play->m_gameState.m_isDualMode)) return 0;
    int checked = 0;
    int alive = Trajectory::get().click_survival(play, !player2, click, checked, (int)steps);
    if (checked <= 0) return 0;
    lua_pushinteger(lua, std::min(alive, checked));
    lua_pushinteger(lua, checked);
    return 2;
}

static int api_player(lua_State* lua) {
    int number = player_argument(lua, 1);
    PlayerObject* player = player_number(current_play(), number);
    if (!player) {
        lua_pushnil(lua);
        return 1;
    }
    const int fields = 40;
    lua_createtable(lua, 0, fields);
    set_number(lua, "number", number);
    set_number(lua, "x", player->getPositionX());
    set_number(lua, "y", player->getPositionY());
    set_number(lua, "y_velocity", player->m_yVelocity);
    set_number(lua, "x_velocity", player->m_platformerXVelocity);
    set_number(lua, "speed", player->m_playerSpeed);
    set_number(lua, "speed_multiplier", player->m_speedMultiplier);
    set_number(lua, "rotation", player->getRotation());
    set_number(lua, "rotation_speed", player->m_rotationSpeed);
    set_number(lua, "size", player->m_vehicleSize);
    set_number(lua, "scale", player->getScale());
    set_number(lua, "gravity", player->m_gravity);
    set_number(lua, "fall_speed", player->m_fallSpeed);
    set_number(lua, "time", player->m_totalTime);
    set_number(lua, "last_jump_time", player->m_lastJumpTime);
    set_number(lua, "last_land_time", player->m_lastLandTime);
    set_number(lua, "last_flip_time", player->m_lastFlipTime);
    set_number(lua, "dash_angle", player->m_dashAngle);
    CCRect box = Logic::object_hitbox(player);
    set_number(lua, "w", box.size.width);
    set_number(lua, "h", box.size.height);
    set_string(lua, "mode", mode_name(player));
    set_bool(lua, "mini", player->m_vehicleSize < 1.f);
    set_bool(lua, "upside_down", player->m_isUpsideDown);
    set_bool(lua, "on_ground", player->m_isOnGround);
    set_bool(lua, "on_slope", player->m_isOnSlope);
    set_bool(lua, "on_ice", player->m_isOnIce);
    set_bool(lua, "sliding", player->m_isSliding);
    set_bool(lua, "moving", player->m_isMoving);
    set_bool(lua, "dead", player->m_isDead);
    set_bool(lua, "dashing", player->m_isDashing);
    set_bool(lua, "going_left", player->m_isGoingLeft);
    set_bool(lua, "sideways", player->m_isSideways);
    set_bool(lua, "locked", player->m_isLocked);
    set_bool(lua, "hidden", player->m_isHidden || !player->isVisible());
    set_bool(lua, "platformer", player->m_isPlatformer);
    set_bool(lua, "jumped", player->m_hasEverJumped);
    set_bool(lua, "jump_buffered", player->m_jumpBuffered);
    set_bool(lua, "holding", Logic::button_held(player, JUMP));
    set_bool(lua, "holding_left", Logic::button_held(player, LEFT));
    set_bool(lua, "holding_right", Logic::button_held(player, RIGHT));
    return 1;
}

static int api_set_player(lua_State* lua) {
    if (!g_play) luaL_error(lua, "set_player only works inside step, before_step, attempt, death and complete");
    int number = player_argument(lua, 1);
    luaL_checktype(lua, 2, LUA_TTABLE);
    PlayerObject* player = player_number(g_play, number);
    if (!player) {
        lua_pushboolean(lua, false);
        return 1;
    }
    auto number_field = [&](const char* key, auto&& apply) { read_number_field(lua, 2, "set_player:", key, apply); };
    auto flag_field = [&](const char* key, auto&& apply) {
        if (lua_getfield(lua, 2, key) != LUA_TNIL) apply(lua_toboolean(lua, -1) != 0);
        lua_pop(lua, 1);
    };
    CCPoint at = player->getPosition();
    bool moved = false;
    number_field("x", [&](double value) {
        at.x = (float)value;
        moved = true;
    });
    number_field("y", [&](double value) {
        at.y = (float)value;
        moved = true;
    });
    if (moved) player->setPosition(at);
    number_field("y_velocity", [&](double value) { player->m_yVelocity = value; });
    number_field("x_velocity", [&](double value) { player->m_platformerXVelocity = value; });
    number_field("rotation", [&](double value) { player->setRotation((float)value); });
    number_field("speed", [&](double value) { player->m_playerSpeed = (float)value; });
    number_field("gravity", [&](double value) { player->m_gravity = value; });
    number_field("scale", [&](double value) { player->setScale((float)value); });
    flag_field("locked", [&](bool on) { player->m_isLocked = on; });
    flag_field("hidden", [&](bool on) { player->setVisible(!on); });
    lua_pushboolean(lua, true);
    return 1;
}

static int api_touching(lua_State* lua) {
    std::string_view wanted = luaL_checkstring(lua, 1);
    int number = player_argument(lua, 2);
    PlayLayer* play = current_play();
    PlayerObject* player = player_number(play, number);
    if (!player) {
        lua_pushnil(lua);
        return 1;
    }
    collect_objects(play, player, TOUCH_RADIUS, g_near);
    for (const auto& nearby : g_near) {
        if (!nearby.touching || !type_matches(nearby.type, wanted)) continue;
        lua_pushstring(lua, nearby.type);
        return 1;
    }
    lua_pushnil(lua);
    return 1;
}

static int api_objects(lua_State* lua) {
    float radius = std::clamp((float)luaL_checknumber(lua, 1), 0.f, MAX_OBJECT_RADIUS);
    int number = player_argument(lua, 2);
    PlayLayer* play = current_play();
    PlayerObject* player = player_number(play, number);
    lua_newtable(lua);
    if (!player) return 1;
    collect_objects(play, player, radius, g_near);
    int listed = std::min((int)g_near.size(), MAX_LISTED_OBJECTS);
    for (int i = 0; i < listed; ++i) {
        const NearObject& nearby = g_near[(size_t)i];
        const int fields = 9;
        lua_createtable(lua, 0, fields);
        set_string(lua, "type", nearby.type);
        set_number(lua, "x", nearby.object->m_positionX);
        set_number(lua, "y", nearby.object->m_positionY);
        set_number(lua, "w", nearby.box.size.width);
        set_number(lua, "h", nearby.box.size.height);
        set_number(lua, "distance", nearby.distance);
        set_bool(lua, "touching", nearby.touching);
        set_bool(lua, "used", orb_used(nearby, number));
        lua_rawseti(lua, -2, i + 1);
    }
    return 1;
}

static int game_field(lua_State* lua) {
    std::string_view key = luaL_checkstring(lua, 2);
    auto& logic = Logic::get();
    PlayLayer* play = current_play();
    if (key == "in_level") lua_pushboolean(lua, play != nullptr);
    else if (key == "recording") lua_pushboolean(lua, logic.is_recording());
    else if (key == "playing") lua_pushboolean(lua, logic.is_playing());
    else if (key == "state") lua_pushstring(lua, logic.state_name(logic.state));
    else if (key == "tps") lua_pushnumber(lua, logic.sample_rate());
    else if (key == "speedhack") lua_pushnumber(lua, logic.speedhack);
    else if (key == "frame_advance") lua_pushboolean(lua, logic.frame_advance);
    else if (key == "inputs") lua_pushinteger(lua, (lua_Integer)logic.inputs.size());
    else if (!play) lua_pushnil(lua);
    else if (key == "frame") lua_pushinteger(lua, logic.step_frame);
    else if (key == "percent") lua_pushnumber(lua, level_percent(play));
    else if (key == "attempt") lua_pushinteger(lua, play->m_attempts);
    else if (key == "platformer") lua_pushboolean(lua, play->m_isPlatformer);
    else if (key == "two_player") lua_pushboolean(lua, two_player(play));
    else if (key == "dual") lua_pushboolean(lua, play->m_gameState.m_isDualMode);
    else if (key == "practice") lua_pushboolean(lua, play->m_isPracticeMode);
    else if (key == "test_mode") lua_pushboolean(lua, play->m_isTestMode);
    else if (key == "paused") lua_pushboolean(lua, play->m_isPaused);
    else if (key == "started") lua_pushboolean(lua, play->m_started);
    else if (key == "dead") lua_pushboolean(lua, play->m_playerDied);
    else if (key == "completed") lua_pushboolean(lua, play->m_hasCompletedLevel);
    else if (key == "time") lua_pushnumber(lua, play->m_gameState.m_levelTime);
    else if (key == "total_time") lua_pushnumber(lua, play->m_gameState.m_totalTime);
    else if (key == "attempt_time") lua_pushnumber(lua, play->m_attemptTime);
    else if (key == "time_played") lua_pushnumber(lua, play->m_timePlayed);
    else if (key == "timewarp") lua_pushnumber(lua, play->m_gameState.m_timeWarp);
    else if (key == "jumps") lua_pushinteger(lua, play->m_jumps);
    else if (key == "clicks") lua_pushinteger(lua, play->m_clicks);
    else if (key == "orbs") lua_pushinteger(lua, play->m_orbs);
    else if (key == "points") lua_pushinteger(lua, play->m_gameState.m_points);
    else if (key == "progress") lua_pushinteger(lua, play->m_gameState.m_currentProgress);
    else if (key == "last_percent") lua_pushinteger(lua, play->m_lastAttemptPercent);
    else if (key == "length") lua_pushnumber(lua, play->m_levelLength);
    else if (key == "end_x") lua_pushnumber(lua, play->m_endPortal ? play->m_endPortal->getPositionX() : 0.f);
    else if (key == "objects") lua_pushinteger(lua, play->m_objects ? play->m_objects->count() : 0);
    else if (key == "camera_zoom") lua_pushnumber(lua, play->m_gameState.m_cameraZoom);
    else if (key == "camera_angle") lua_pushnumber(lua, play->m_gameState.m_cameraAngle);
    else if (key == "flipped") lua_pushboolean(lua, play->m_gameState.m_isFlipped);
    else if (key == "level") lua_pushstring(lua, play->m_level ? std::string(play->m_level->m_levelName).c_str() : "");
    else if (key == "level_id") lua_pushinteger(lua, play->m_level ? play->m_level->m_levelID.value() : 0);
    else if (key == "creator") lua_pushstring(lua, play->m_level ? std::string(play->m_level->m_creatorName).c_str() : "");
    else if (key == "song_id") lua_pushinteger(lua, play->m_level ? play->m_level->m_songID : 0);
    else if (key == "stars") lua_pushinteger(lua, play->m_level ? play->m_level->m_stars.value() : 0);
    else if (key == "demon") lua_pushboolean(lua, play->m_level && play->m_level->m_demon.value() != 0);
    else if (key == "best") lua_pushinteger(lua, play->m_level ? play->m_level->m_normalPercent.value() : 0);
    else if (key == "practice_best") lua_pushinteger(lua, play->m_level ? play->m_level->m_practicePercent : 0);
    else if (key == "total_attempts") lua_pushinteger(lua, play->m_level ? play->m_level->m_attempts.value() : 0);
    else if (key == "total_jumps") lua_pushinteger(lua, play->m_level ? play->m_level->m_jumps.value() : 0);
    else if (key == "coins") lua_pushinteger(lua, play->m_level ? play->m_level->m_coins : 0);
    else lua_pushnil(lua);
    return 1;
}

struct EchoSetting {
    const char* name;
    bool Logic::* flag;
    float Logic::* number;
    int Logic::* whole;
};

static const EchoSetting ECHO_SETTINGS[] = {
    { "speedhack", nullptr, &Logic::speedhack, nullptr },
    { "tps", nullptr, &Logic::fps, nullptr },
    { "frame_advance", &Logic::frame_advance, nullptr, nullptr },
    { "noclip_p1", &Logic::noclip_p1, nullptr, nullptr },
    { "noclip_p2", &Logic::noclip_p2, nullptr, nullptr },
    { "show_hitboxes", &Logic::show_hitboxes, nullptr, nullptr },
    { "show_trajectory", &Logic::show_traj, nullptr, nullptr },
    { "trajectory_seconds", nullptr, &Logic::traj_seconds, nullptr },
    { "trajectory_autoclicker", &Logic::traj_autoclicker, nullptr, nullptr },
    { "prevent_death", &Logic::pause_before_kill, nullptr, nullptr },
    { "autoclicker", &Logic::autoclicker, nullptr, nullptr },
    { "clickbot", &Logic::clickbot, nullptr, nullptr },
    { "safe_mode", &Logic::safe_mode, nullptr, nullptr },
    { "maintain_gravity", &Logic::maintain_gravity, nullptr, nullptr },
    { "same_gravity", &Logic::same_gravity, nullptr, nullptr },
    { "click_both", &Logic::click_both, nullptr, nullptr },
    { "swap_players", &Logic::swap_players, nullptr, nullptr },
    { "inverse_p1", &Logic::inverse_p1, nullptr, nullptr },
    { "inverse_p2", &Logic::inverse_p2, nullptr, nullptr },
    { "hide_p1", &Logic::hide_p1, nullptr, nullptr },
    { "hide_p2", &Logic::hide_p2, nullptr, nullptr },
    { "no_mirror_portals", &Logic::no_mirror_portals, nullptr, nullptr },
    { "layout_mode", &Logic::layout_mode, nullptr, nullptr },
    { "backstep", &Logic::backstep, nullptr, nullptr },
    { "lock_delta", &Logic::lock_delta, nullptr, nullptr },
    { "real_time", &Logic::real_time_mode, nullptr, nullptr },
    { "click_dash_orbs", &Logic::click_dash_orbs, nullptr, nullptr },
    { "click_black_orbs", &Logic::click_black_orbs, nullptr, nullptr },
    { "practice_music", &Logic::practice_music_hack, nullptr, nullptr },
    { "velocity_unrounding", &Logic::velocity_unrounding, nullptr, nullptr },
    { "moving_objects", &Logic::traj_moving_objects, nullptr, nullptr },
    { "follow_mode", nullptr, nullptr, &Logic::follow_mode },
    { "extra_click_count", nullptr, nullptr, &Logic::extra_click_count },
    { "labels_auto", &Logic::labels_auto, nullptr, nullptr },
    { "label_corner", nullptr, nullptr, &Logic::label_corner },
    { "label_scale", nullptr, &Logic::label_scale, nullptr },
    { "label_decimals", nullptr, nullptr, &Logic::label_decimals },
    { "percent_decimals", nullptr, nullptr, &Logic::percent_accuracy },
};

static EchoSetting echo_setting(lua_State* lua, int index) {
    std::string_view wanted = luaL_checkstring(lua, index);
    for (const auto& setting : ECHO_SETTINGS) {
        if (wanted == setting.name) return setting;
    }
    if (wanted.starts_with("show_")) {
        if (auto* info = label_info(std::string(wanted.substr(5)))) return { info->key, info->shown, nullptr, nullptr };
    }
    luaL_argerror(lua, index, lua_pushfstring(lua, "there is no Echo setting named %s", std::string(wanted).c_str()));
    return {};
}

static int echo_get(lua_State* lua) {
    EchoSetting setting = echo_setting(lua, 1);
    auto& logic = Logic::get();
    if (setting.flag) lua_pushboolean(lua, logic.*setting.flag);
    else if (setting.number) lua_pushnumber(lua, logic.*setting.number);
    else lua_pushinteger(lua, logic.*setting.whole);
    return 1;
}

static int echo_set(lua_State* lua) {
    EchoSetting setting = echo_setting(lua, 1);
    auto& logic = Logic::get();
    std::string_view name = setting.name;
    if (setting.flag) {
        luaL_checkany(lua, 2);
        logic.*setting.flag = lua_toboolean(lua, 2);
        if (name == "no_mirror_portals") GUI::get().apply_no_mirror();
    }
    else if (name == "speedhack") {
        float speed = (float)luaL_checknumber(lua, 2);
        if (speed <= 0.f) luaL_argerror(lua, 2, "the speed is more than 0");
        GUI::get().set_speed(speed, true);
    }
    else if (name == "tps") {
        float rate = (float)luaL_checknumber(lua, 2);
        if (rate <= 0.f) luaL_argerror(lua, 2, "the TPS is more than 0");
        logic.set_tps(rate);
    }
    else if (setting.number) logic.*setting.number = (float)luaL_checknumber(lua, 2);
    else logic.*setting.whole = (int)luaL_checkinteger(lua, 2);
    logic.push_runtime_settings();
    logic.save_settings_file();
    return 0;
}

static int echo_settings(lua_State* lua) {
    lua_createtable(lua, (int)std::size(ECHO_SETTINGS), 0);
    int at = 1;
    for (const auto& setting : ECHO_SETTINGS) {
        lua_pushstring(lua, setting.name);
        lua_rawseti(lua, -2, at++);
    }
    for (const auto& info : label_infos()) {
        lua_pushstring(lua, (std::string("show_") + info.key).c_str());
        lua_rawseti(lua, -2, at++);
    }
    return 1;
}

static int echo_notify(lua_State* lua) {
    const char* text = luaL_checkstring(lua, 1);
    bool good = lua_isnoneornil(lua, 2) || lua_toboolean(lua, 2);
    Logic::get().notify(text, good);
    return 0;
}

static int echo_field(lua_State* lua) {
    std::string_view key = luaL_checkstring(lua, 2);
    auto& logic = Logic::get();
    if (key == "state") lua_pushstring(lua, logic.state_name(logic.state));
    else if (key == "recording") lua_pushboolean(lua, logic.is_recording());
    else if (key == "playing") lua_pushboolean(lua, logic.is_playing());
    else if (key == "macro") lua_pushstring(lua, logic.macro_name.c_str());
    else if (key == "version") lua_pushstring(lua, Mod::get()->getVersion().toVString().c_str());
    else if (key == "docked") lua_pushboolean(lua, GUI::get().docked());
    else lua_pushnil(lua);
    return 1;
}

static void read_look(lua_State* lua, int index, ScriptLabels::Look& look) {
    luaL_checktype(lua, index, LUA_TTABLE);
    auto number = [&](const char* key, auto&& apply) { read_number_field(lua, index, "label option", key, apply); };
    number("x", [&](double value) {
        look.place.x = (float)value;
        look.placed = true;
    });
    number("y", [&](double value) {
        look.place.y = (float)value;
        look.placed = true;
    });
    number("scale", [&](double value) { look.place.scale = std::clamp((float)value, .05f, 5.f); });
    number("opacity", [&](double value) { look.place.opacity = std::clamp((int)value, 0, 100); });
    if (lua_getfield(lua, index, "color") != LUA_TNIL) {
        if (!lua_istable(lua, -1)) luaL_error(lua, "label option color is a table like {255, 255, 255}");
        int table = lua_gettop(lua);
        unsigned char parts[3] = { 255, 255, 255 };
        for (int i = 0; i < 3; ++i) {
            lua_rawgeti(lua, table, i + 1);
            if (lua_isnumber(lua, -1)) parts[i] = (unsigned char)std::clamp((int)lua_tonumber(lua, -1), 0, 255);
            lua_pop(lua, 1);
        }
        look.color = { parts[0], parts[1], parts[2] };
    }
    lua_pop(lua, 1);
}

static void place_label(lua_State* lua, RunningScript& script, const std::string& name, const std::string& text, int look_index) {
    ScriptLabels::Look look;
    bool styled = !lua_isnoneornil(lua, look_index);
    if (styled) read_look(lua, look_index, look);
    if (!ScriptLabels::set(script.label_owner, name, text, styled ? &look : nullptr))
        luaL_error(lua, "there is no room for another label (%d is the most)", SCRIPT_LAST_TAG - SCRIPT_FIRST_TAG + 1);
}

static int label_set(lua_State* lua) {
    RunningScript& script = active_script(lua);
    std::string name = luaL_checkstring(lua, 1);
    if (name.empty()) luaL_argerror(lua, 1, "the label needs a name");
    if (label_info(name)) {
        if (lua_isnoneornil(lua, 2)) ScriptLabels::set_override(script.label_owner, name, nullptr);
        else {
            std::string text = luaL_checkstring(lua, 2);
            ScriptLabels::set_override(script.label_owner, name, &text);
        }
        lua_pushboolean(lua, true);
        return 1;
    }
    if (lua_isnoneornil(lua, 2)) {
        lua_pushboolean(lua, ScriptLabels::remove(script.label_owner, name));
        return 1;
    }
    place_label(lua, script, name, luaL_checkstring(lua, 2), 3);
    lua_pushboolean(lua, true);
    return 1;
}

static int label_remove(lua_State* lua) {
    lua_settop(lua, 1);
    return label_set(lua);
}

static int label_clear(lua_State* lua) {
    RunningScript& script = active_script(lua);
    ScriptLabels::clear(script.label_owner);
    return 0;
}

static int label_names(lua_State* lua) {
    auto infos = label_infos();
    lua_createtable(lua, (int)infos.size(), 0);
    int at = 1;
    for (const auto& info : infos) {
        lua_pushstring(lua, info.key);
        lua_rawseti(lua, -2, at++);
    }
    return 1;
}

static int api_print_to(lua_State* lua) {
    RunningScript& script = active_script(lua);
    if (lua_isnoneornil(lua, 1)) {
        script.print_label.clear();
        script.printed.clear();
        return 0;
    }
    std::string name = luaL_checkstring(lua, 1);
    if (name.empty()) luaL_argerror(lua, 1, "the label needs a name");
    lua_Integer lines = luaL_optinteger(lua, 2, 1);
    if (lines < 1 || lines > MOST_PRINT_LINES) luaL_argerror(lua, 2, lua_pushfstring(lua, "lines go from 1 to %d", MOST_PRINT_LINES));
    place_label(lua, script, name, "", 3);
    script.print_label = name;
    script.print_lines = (int)lines;
    script.printed.clear();
    return 0;
}

static int api_stop(lua_State* lua) {
    active_script(lua).stopping = true;
    return 0;
}

static int api_clock(lua_State* lua) {
    lua_pushnumber(lua, std::chrono::duration<double>(std::chrono::steady_clock::now() - g_started).count());
    return 1;
}

static int script_field(lua_State* lua) {
    std::string_view key = luaL_checkstring(lua, 2);
    RunningScript& script = active_script(lua);
    if (key == "name") lua_pushstring(lua, script.name.c_str());
    else if (key == "file") lua_pushstring(lua, chunk_name(script.file).c_str());
    else if (key == "autorun") lua_pushboolean(lua, MacroScript::host().autoruns(script.file));
    else if (key == "others") {
        lua_newtable(lua);
        int at = 1;
        for (const auto& other : g_scripts) {
            if (other.get() == &script) continue;
            lua_pushstring(lua, other->name.c_str());
            lua_rawseti(lua, -2, at++);
        }
    }
    else lua_pushnil(lua);
    return 1;
}

static void check_editable(lua_State* lua) {
    auto& logic = Logic::get();
    if (logic.is_recording() || logic.is_playing()) luaL_error(lua, "the macro can only be changed while Echo is not recording or playing");
}

static void before_macro_edit(lua_State* lua) {
    check_editable(lua);
    if (g_snapshot.taken) return;
    auto& logic = Logic::get();
    g_snapshot.taken = true;
    g_snapshot.whole = logic.inputs.size() <= SNAPSHOT_MOST_INPUTS;
    if (!g_snapshot.whole) return;
    g_snapshot.inputs = logic.inputs;
    g_snapshot.poses = logic.poses;
    g_snapshot.ids = logic.input_ids;
}

static void finish_macro_edits(bool fine) {
    if (!g_snapshot.taken) return;
    auto& logic = Logic::get();
    if (!fine && g_snapshot.whole) {
        logic.inputs = std::move(g_snapshot.inputs);
        logic.poses = std::move(g_snapshot.poses);
        logic.input_ids = std::move(g_snapshot.ids);
        add_output("The macro was put back the way it was before the error.");
    }
    else if (!fine) {
        add_output("The macro is too big to put back, so the changes made before the error were kept.");
    }
    g_snapshot = {};
    logic.sort_inputs();
    logic.autosave_replay();
}

static size_t input_index(lua_State* lua, int index) {
    lua_Integer number = luaL_checkinteger(lua, index);
    if (number < 1 || (unsigned long long)number > Logic::get().inputs.size()) luaL_argerror(lua, index, "there is no input with that number");
    return (size_t)(number - 1);
}

static void push_input(lua_State* lua, const Frame& input) {
    const int fields = 4;
    lua_createtable(lua, 0, fields);
    set_number(lua, "frame", input.number);
    set_string(lua, "button", button_name(input.button));
    set_bool(lua, "down", input.holding);
    set_number(lua, "player", input.player2 ? 2 : 1);
}

static Frame read_input(lua_State* lua, int index, Frame input) {
    luaL_checktype(lua, index, LUA_TTABLE);
    if (lua_getfield(lua, index, "frame") != LUA_TNIL) {
        int whole = 0;
        lua_Integer frame = lua_tointegerx(lua, -1, &whole);
        if (!whole || frame < 0 || frame > std::numeric_limits<int>::max()) luaL_error(lua, "an input's frame must be a whole number, 0 or more");
        input.number = (int)frame;
    }
    lua_pop(lua, 1);
    if (lua_getfield(lua, index, "button") != LUA_TNIL) {
        int button = lua_type(lua, -1) == LUA_TSTRING ? button_named(lua_tostring(lua, -1)) : 0;
        if (!button) luaL_error(lua, "an input's button is \"jump\", \"left\" or \"right\"");
        input.button = (unsigned char)button;
    }
    lua_pop(lua, 1);
    if (lua_getfield(lua, index, "down") != LUA_TNIL) input.holding = lua_toboolean(lua, -1);
    lua_pop(lua, 1);
    if (lua_getfield(lua, index, "player") != LUA_TNIL) {
        lua_Integer player = lua_tointeger(lua, -1);
        if (player != 1 && player != 2) luaL_error(lua, "an input's player is 1 or 2");
        input.player2 = player == 2;
    }
    lua_pop(lua, 1);
    return input;
}

static int macro_count(lua_State* lua) {
    lua_pushinteger(lua, (lua_Integer)Logic::get().inputs.size());
    return 1;
}

static int macro_get(lua_State* lua) {
    push_input(lua, Logic::get().inputs[input_index(lua, 1)]);
    return 1;
}

static int macro_set(lua_State* lua) {
    size_t at = input_index(lua, 1);
    auto& inputs = Logic::get().inputs;
    Frame changed = read_input(lua, 2, inputs[at]);
    before_macro_edit(lua);
    inputs[at] = changed;
    return 0;
}

static int macro_insert(lua_State* lua) {
    luaL_checktype(lua, 1, LUA_TTABLE);
    if (lua_getfield(lua, 1, "frame") == LUA_TNIL) luaL_error(lua, "macro.insert needs a frame");
    lua_pop(lua, 1);
    Frame created;
    created.holding = true;
    created = read_input(lua, 1, created);
    auto& logic = Logic::get();
    if (logic.inputs.size() >= ReplayIO::max_inputs) luaL_error(lua, "the macro already has as many inputs as it can hold");
    before_macro_edit(lua);
    size_t before = logic.inputs.size();
    logic.inputs.push_back(created);
    if (!logic.poses.empty() && logic.poses.size() == before) logic.poses.push_back(FramePose{});
    else logic.poses.clear();
    if (logic.input_ids.size() == before) logic.input_ids.push_back(logic.new_input_id());
    else logic.input_ids.clear();
    lua_pushinteger(lua, (lua_Integer)logic.inputs.size());
    return 1;
}

static int macro_remove(lua_State* lua) {
    size_t at = input_index(lua, 1);
    before_macro_edit(lua);
    auto& logic = Logic::get();
    size_t before = logic.inputs.size();
    logic.inputs.erase(logic.inputs.begin() + (long)at);
    if (logic.poses.size() == before) logic.poses.erase(logic.poses.begin() + (long)at);
    else logic.poses.clear();
    if (logic.input_ids.size() == before) logic.input_ids.erase(logic.input_ids.begin() + (long)at);
    else logic.input_ids.clear();
    return 0;
}

static int macro_clear(lua_State* lua) {
    before_macro_edit(lua);
    auto& logic = Logic::get();
    logic.inputs.clear();
    logic.clear_input_extras();
    return 0;
}

static int macro_filter(lua_State* lua) {
    luaL_checktype(lua, 1, LUA_TFUNCTION);
    check_editable(lua);
    auto& inputs = Logic::get().inputs;
    size_t count = inputs.size();
    std::vector<char> keep(count);
    for (size_t i = 0; i < count; ++i) {
        lua_pushvalue(lua, 1);
        push_input(lua, inputs[i]);
        lua_pushinteger(lua, (lua_Integer)i + 1);
        lua_call(lua, 2, 1);
        keep[i] = lua_toboolean(lua, -1);
        lua_pop(lua, 1);
        if (inputs.size() != count) luaL_error(lua, "the function given to macro.filter can't add or remove inputs");
    }
    before_macro_edit(lua);
    auto& logic = Logic::get();
    size_t at = 0;
    compact_inputs(logic.inputs, logic.poses, logic.input_ids, [&](const Frame&) {
        return keep[at++] != 0;
    });
    return 0;
}

static int macro_field(lua_State* lua) {
    std::string_view key = luaL_checkstring(lua, 2);
    auto& logic = Logic::get();
    if (key == "name") lua_pushstring(lua, logic.macro_name.c_str());
    else if (key == "fps") lua_pushnumber(lua, logic.macro_fps);
    else lua_pushnil(lua);
    return 1;
}

static void set_lazy_fields(lua_State* lua, lua_CFunction field) {
    lua_createtable(lua, 0, 1);
    lua_pushcfunction(lua, field);
    lua_setfield(lua, -2, "__index");
    lua_setmetatable(lua, -2);
}

static int register_api(lua_State* lua) {
    const luaL_Reg functions[] = {
        { "press", api_press },
        { "release", api_release },
        { "click", api_click },
        { "player", api_player },
        { "set_player", api_set_player },
        { "touching", api_touching },
        { "objects", api_objects },
        { "survives", api_survives },
        { "print_to", api_print_to },
        { "stop", api_stop },
        { "clock", api_clock },
    };
    for (const auto& function : functions)
        lua_register(lua, function.name, function.func);
    lua_newtable(lua);
    set_lazy_fields(lua, game_field);
    lua_setglobal(lua, "game");
    const luaL_Reg echo_functions[] = {
        { "get", echo_get },
        { "set", echo_set },
        { "settings", echo_settings },
        { "notify", echo_notify },
        { nullptr, nullptr },
    };
    lua_newtable(lua);
    luaL_setfuncs(lua, echo_functions, 0);
    set_lazy_fields(lua, echo_field);
    lua_setglobal(lua, "echo");
    const luaL_Reg label_functions[] = {
        { "set", label_set },
        { "remove", label_remove },
        { "clear", label_clear },
        { "names", label_names },
        { nullptr, nullptr },
    };
    lua_newtable(lua);
    luaL_setfuncs(lua, label_functions, 0);
    lua_setglobal(lua, "label");
    lua_newtable(lua);
    set_lazy_fields(lua, script_field);
    lua_setglobal(lua, "script");
    const luaL_Reg macro_functions[] = {
        { "count", macro_count },
        { "get", macro_get },
        { "set", macro_set },
        { "insert", macro_insert },
        { "remove", macro_remove },
        { "clear", macro_clear },
        { "filter", macro_filter },
        { nullptr, nullptr },
    };
    lua_newtable(lua);
    luaL_setfuncs(lua, macro_functions, 0);
    set_lazy_fields(lua, macro_field);
    lua_setglobal(lua, "macro");
    GameAccess::open(lua);
    return 0;
}

static std::string script_name(const fs::path& file) {
    return geode::utils::string::pathToString(file.stem());
}

static void run_script(const std::string& text, const fs::path& file) {
    stop_file(file);
    if (g_scripts.size() >= MOST_SCRIPTS) {
        report(fmt::format("{} scripts are already running, which is the most", MOST_SCRIPTS));
        return;
    }
    g_error.clear();
    auto script = std::make_unique<RunningScript>();
    script->label_owner = label_owner_of(file);
    script->file = file;
    script->name = script_name(file);
    RunningScript* raw = script.get();
    script->runtime = std::make_unique<Script::Runtime>(MACRO_MEMORY_LIMIT);
    script->runtime->on_print = [raw](const std::string& line) {
        print_from(*raw, line);
    };
    script->runtime->limit_instructions(RUN_INSTRUCTIONS);
    g_scripts.push_back(std::move(script));
    RunningScript* outer = g_active;
    g_active = raw;
    lua_State* lua = raw->runtime->state();
    bool fine = lua != nullptr;
    if (fine) {
        lua_pushcfunction(lua, register_api);
        fine = raw->runtime->call(0, 0);
    }
    if (fine) fine = raw->runtime->run_text(text, chunk_name(file));
    GameAccess::expire();
    finish_macro_edits(fine);
    g_active = outer;
    if (!fine) {
        report(lua ? raw->runtime->error() : "Lua could not start");
        stop_script(*raw, false);
        return;
    }
    bool listens = false;
    for (int i = 0; i < (int)Callback::Count; ++i) {
        raw->callbacks[i] = take_function(lua, CALLBACK_NAMES[i]);
        listens = listens || raw->callbacks[i] != LUA_NOREF;
    }
    if (listens && !raw->stopping) {
        refresh_flags();
        return;
    }
    stop_script(*raw, true);
    add_output("Finished " + chunk_name(file));
}

static void run_callback(Callback which, PlayLayer* play, std::initializer_list<lua_Integer> args) {
    size_t i = 0;
    while (i < g_scripts.size()) {
        RunningScript* script = g_scripts[i].get();
        if (script->callbacks[(int)which] == LUA_NOREF) {
            ++i;
            continue;
        }
        lua_State* lua = script->runtime->state();
        RunningScript* outer = g_active;
        g_active = script;
        g_play = play;
        script->runtime->limit_instructions(CALLBACK_INSTRUCTIONS);
        lua_rawgeti(lua, LUA_REGISTRYINDEX, script->callbacks[(int)which]);
        for (lua_Integer arg : args)
            lua_pushinteger(lua, arg);
        bool fine = script->runtime->call((int)args.size(), 0);
        GameAccess::expire();
        g_play = nullptr;
        g_active = outer;
        finish_macro_edits(fine);
        if (fine && !script->stopping) {
            ++i;
            continue;
        }
        if (!fine) report(script->runtime->error());
        else add_output("Finished " + chunk_name(script->file));
        stop_script(*script, fine);
    }
}

namespace MacroScript {
    void after_step(PlayLayer* play) {
        auto& logic = Logic::get();
        if (play->m_isPaused || play->m_playerDied || logic.predicting || logic.backstepping) return;
        run_callback(Callback::Step, play, { logic.step_frame });
    }

    void before_step(PlayLayer* play) {
        auto& logic = Logic::get();
        if (!play || play->m_isPaused || play->m_playerDied || logic.predicting || logic.backstepping) return;
        g_can_predict = true;
        g_predicted_steps = 0;
        run_callback(Callback::BeforeStep, play, { logic.step_frame + 1 });
        g_can_predict = false;
    }

    void attempt_started(PlayLayer* play) {
        if (Logic::get().predicting || Logic::get().backstepping) return;
        run_callback(Callback::Attempt, play, { play->m_attempts });
    }

    void player_died(PlayLayer* play, PlayerObject* player) {
        if (Logic::get().predicting) return;
        run_callback(Callback::Death, play, { Logic::get().step_frame, player == play->m_player2 ? 2 : 1 });
    }

    void level_finished(PlayLayer* play) {
        if (Logic::get().predicting) return;
        run_callback(Callback::Complete, play, { Logic::get().step_frame });
    }

    void autorun() {
        for (const auto& name : Logic::get().autorun_scripts) {
            fs::path file = scripts_folder() / ReplayIO::utf8_path(name);
            std::error_code error;
            if (!fs::is_regular_file(file, error)) continue;
            host().run_file(file);
        }
    }
}

$on_mod(Loaded) {
    Loader::get()->queueInMainThread([] {
        MacroScript::autorun();
    });
}

class MacroHost : public Script::Host {
public:
    const char* name() const override {
        return "Macros";
    }

    fs::path folder() const override {
        return scripts_folder();
    }

    std::vector<fs::path> files() const override {
        std::vector<fs::path> found;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder(), error)) {
            if (entry.is_regular_file(error) && GUI::get().lower_copy(geode::utils::string::pathToString(entry.path().extension())) == ".lua")
                found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end(), [this](const fs::path& a, const fs::path& b) {
            return GUI::get().lower_copy(label(a)) < GUI::get().lower_copy(label(b));
        });
        return found;
    }

    std::vector<fs::path> examples() const override {
        std::vector<fs::path> found;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(Mod::get()->getResourcesDir(), error)) {
            std::string name = geode::utils::string::pathToString(entry.path().filename());
            if (name.starts_with(BUNDLED_MACRO_PREFIX) && name.ends_with(".lua") && !fs::exists(installed_file(entry.path()), error))
                found.push_back(entry.path());
        }
        std::sort(found.begin(), found.end());
        return found;
    }

    std::string label(const fs::path& file) const override {
        if (Script::is_bundled(file)) return bundled_name(file);
        return geode::utils::string::pathToString(file.stem());
    }

    fs::path current() const override {
        return g_scripts.empty() ? fs::path() : g_scripts.front()->file;
    }

    std::vector<fs::path> running_files() const override {
        std::vector<fs::path> files;
        for (const auto& script : g_scripts)
            files.push_back(script->file);
        return files;
    }

    void stop_file(const fs::path& file) override {
        ::stop_file(file);
    }

    bool can_autorun() const override {
        return true;
    }

    bool autoruns(const fs::path& file) const override {
        const auto& names = Logic::get().autorun_scripts;
        return std::find(names.begin(), names.end(), chunk_name(file)) != names.end();
    }

    void set_autorun(const fs::path& file, bool on) override {
        auto& names = Logic::get().autorun_scripts;
        std::string name = chunk_name(file);
        auto at = std::find(names.begin(), names.end(), name);
        if (on && at == names.end()) names.push_back(name);
        if (!on && at != names.end()) names.erase(at);
        Logic::get().save_settings_file();
    }

    const std::vector<std::string>& names() const override {
        static const std::vector<std::string> api = [] {
            std::vector<std::string> found(std::begin(CALLBACK_NAMES), std::end(CALLBACK_NAMES));
            for (const char* name : { "press", "release", "click", "player", "set_player", "touching", "objects", "survives", "print_to", "stop", "clock", "game", "macro", "echo", "label", "script", "gd" })
                found.push_back(name);
            return found;
        }();
        return api;
    }

    bool make_new(fs::path& made) override {
        std::error_code error;
        for (int number = 1; number <= MAX_NEW_FILES; ++number) {
            made = folder() / ReplayIO::utf8_path(number == 1 ? std::string("My Script.lua") : fmt::format("My Script {}.lua", number));
            if (!fs::exists(made, error)) break;
        }
        fs::create_directories(folder(), error);
        if (error || !Script::write_text(made, NEW_MACRO_SCRIPT)) return false;
        copy_guide(folder());
        return true;
    }

    bool install_example(const fs::path& example, fs::path& made) override {
        made = installed_file(example);
        return copy_script(example, made);
    }

    bool remove_file(const fs::path& file) override {
        ::stop_file(file);
        if (autoruns(file)) set_autorun(file, false);
        std::error_code error;
        return fs::remove(file, error);
    }

    void run_text(const std::string& text, const fs::path& file) override {
        run_script(text, file);
    }

    void run_file(const fs::path& file) override {
        std::string text;
        if (read_text(file, text)) run_script(text, file);
        else report("Could not open " + geode::utils::string::pathToString(file.filename()));
    }

    void stop() override {
        stop_everything();
    }

    bool running() const override {
        return MacroScript::g_running;
    }

    bool reruns() const override {
        return false;
    }

    const std::string& error() const override {
        return g_error;
    }

    const std::deque<std::string>& output() const override {
        return g_output;
    }

    void clear_output() override {
        g_output.clear();
    }
};

namespace MacroScript {
    Script::Host& host() {
        static MacroHost macro_host;
        return macro_host;
    }
}
