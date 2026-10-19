#include "Script/game_access.hpp"
#include "Logic/logic.hpp"

#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>

#include <iterator>
#include <string>
#include <string_view>

#define OBJECT_META "echo.game.object"

using GameAccess::field;
using GameAccess::method;
using GameAccess::seed_field;

struct HeldObject {
    void* pointer;
    int id;
    int generation;
};

static const GameAccess::Field GAME_LAYER_FIELDS[] = {
    { "m_player1", field<GJBaseGameLayer, &GJBaseGameLayer::m_player1> },
    { "m_player2", field<GJBaseGameLayer, &GJBaseGameLayer::m_player2> },
    { "m_level", field<GJBaseGameLayer, &GJBaseGameLayer::m_level> },
    { "m_attempts", field<GJBaseGameLayer, &GJBaseGameLayer::m_attempts> },
    { "m_clicks", field<GJBaseGameLayer, &GJBaseGameLayer::m_clicks> },
    { "m_currentStep", field<GJBaseGameLayer, &GJBaseGameLayer::m_currentStep> },
    { "m_timePlayed", field<GJBaseGameLayer, &GJBaseGameLayer::m_timePlayed> },
    { "m_levelLength", field<GJBaseGameLayer, &GJBaseGameLayer::m_levelLength> },
    { "m_started", field<GJBaseGameLayer, &GJBaseGameLayer::m_started> },
    { "m_playerDied", field<GJBaseGameLayer, &GJBaseGameLayer::m_playerDied> },
    { "m_isPracticeMode", field<GJBaseGameLayer, &GJBaseGameLayer::m_isPracticeMode> },
    { "m_isTestMode", field<GJBaseGameLayer, &GJBaseGameLayer::m_isTestMode> },
    { "m_isPlatformer", field<GJBaseGameLayer, &GJBaseGameLayer::m_isPlatformer> },
    { "m_lowDetailMode", field<GJBaseGameLayer, &GJBaseGameLayer::m_lowDetailMode> },
    { "m_points", field<GJBaseGameLayer, &GJBaseGameLayer::m_points> },
};

static const GameAccess::Method GAME_LAYER_METHODS[] = {
    { "shakeCamera", method<&GJBaseGameLayer::shakeCamera>, 3 },
    { "toggleGroup", method<&GJBaseGameLayer::toggleGroup>, 2 },
    { "getItemValue", method<&GJBaseGameLayer::getItemValue>, 2 },
    { "updateCounters", method<&GJBaseGameLayer::updateCounters>, 2 },
};

static const GameAccess::Field PLAY_LAYER_FIELDS[] = {
    { "m_jumps", field<PlayLayer, &PlayLayer::m_jumps> },
    { "m_attemptTime", field<PlayLayer, &PlayLayer::m_attemptTime> },
    { "m_lastAttemptPercent", field<PlayLayer, &PlayLayer::m_lastAttemptPercent> },
    { "m_hasCompletedLevel", field<PlayLayer, &PlayLayer::m_hasCompletedLevel> },
    { "m_isPaused", field<PlayLayer, &PlayLayer::m_isPaused> },
    { "m_endXPosition", field<PlayLayer, &PlayLayer::m_endXPosition> },
    { "m_orbs", field<PlayLayer, &PlayLayer::m_orbs> },
};

static const GameAccess::Method PLAY_LAYER_METHODS[] = {
    { "getCurrentPercent", method<&PlayLayer::getCurrentPercent>, 0 },
    { "resetLevel", method<&PlayLayer::resetLevel>, 0 },
    { "resetLevelFromStart", method<&PlayLayer::resetLevelFromStart>, 0 },
    { "pauseGame", method<&PlayLayer::pauseGame>, 1 },
    { "resume", method<&PlayLayer::resume>, 0 },
    { "togglePracticeMode", method<&PlayLayer::togglePracticeMode>, 1 },
    { "removeCheckpoint", method<&PlayLayer::removeCheckpoint>, 1 },
    { "removeAllCheckpoints", method<&PlayLayer::removeAllCheckpoints>, 0 },
    { "posForTime", method<&PlayLayer::posForTime>, 1 },
    { "toggleProgressbar", method<&PlayLayer::toggleProgressbar>, 0 },
};

static const GameAccess::Field PLAYER_FIELDS[] = {
    { "m_position", field<PlayerObject, &PlayerObject::m_position> },
    { "m_yVelocity", field<PlayerObject, &PlayerObject::m_yVelocity> },
    { "m_platformerXVelocity", field<PlayerObject, &PlayerObject::m_platformerXVelocity> },
    { "m_gravity", field<PlayerObject, &PlayerObject::m_gravity> },
    { "m_fallSpeed", field<PlayerObject, &PlayerObject::m_fallSpeed> },
    { "m_playerSpeed", field<PlayerObject, &PlayerObject::m_playerSpeed> },
    { "m_speedMultiplier", field<PlayerObject, &PlayerObject::m_speedMultiplier> },
    { "m_vehicleSize", field<PlayerObject, &PlayerObject::m_vehicleSize> },
    { "m_rotationSpeed", field<PlayerObject, &PlayerObject::m_rotationSpeed> },
    { "m_dashAngle", field<PlayerObject, &PlayerObject::m_dashAngle> },
    { "m_isShip", field<PlayerObject, &PlayerObject::m_isShip> },
    { "m_isBird", field<PlayerObject, &PlayerObject::m_isBird> },
    { "m_isBall", field<PlayerObject, &PlayerObject::m_isBall> },
    { "m_isDart", field<PlayerObject, &PlayerObject::m_isDart> },
    { "m_isRobot", field<PlayerObject, &PlayerObject::m_isRobot> },
    { "m_isSpider", field<PlayerObject, &PlayerObject::m_isSpider> },
    { "m_isSwing", field<PlayerObject, &PlayerObject::m_isSwing> },
    { "m_isUpsideDown", field<PlayerObject, &PlayerObject::m_isUpsideDown> },
    { "m_isSideways", field<PlayerObject, &PlayerObject::m_isSideways> },
    { "m_isGoingLeft", field<PlayerObject, &PlayerObject::m_isGoingLeft> },
    { "m_isOnGround", field<PlayerObject, &PlayerObject::m_isOnGround> },
    { "m_isOnSlope", field<PlayerObject, &PlayerObject::m_isOnSlope> },
    { "m_isOnIce", field<PlayerObject, &PlayerObject::m_isOnIce> },
    { "m_isSliding", field<PlayerObject, &PlayerObject::m_isSliding> },
    { "m_isDashing", field<PlayerObject, &PlayerObject::m_isDashing> },
    { "m_isDead", field<PlayerObject, &PlayerObject::m_isDead> },
    { "m_isLocked", field<PlayerObject, &PlayerObject::m_isLocked> },
    { "m_isHidden", field<PlayerObject, &PlayerObject::m_isHidden> },
    { "m_isPlatformer", field<PlayerObject, &PlayerObject::m_isPlatformer> },
    { "m_isSecondPlayer", field<PlayerObject, &PlayerObject::m_isSecondPlayer> },
    { "m_isOutOfBounds", field<PlayerObject, &PlayerObject::m_isOutOfBounds> },
    { "m_holdingLeft", field<PlayerObject, &PlayerObject::m_holdingLeft> },
    { "m_holdingRight", field<PlayerObject, &PlayerObject::m_holdingRight> },
    { "m_jumpBuffered", field<PlayerObject, &PlayerObject::m_jumpBuffered> },
    { "m_hasEverJumped", field<PlayerObject, &PlayerObject::m_hasEverJumped> },
    { "m_touchedRing", field<PlayerObject, &PlayerObject::m_touchedRing> },
    { "m_touchedPad", field<PlayerObject, &PlayerObject::m_touchedPad> },
    { "m_touchedGravityPortal", field<PlayerObject, &PlayerObject::m_touchedGravityPortal> },
    { "m_ignoreDamage", field<PlayerObject, &PlayerObject::m_ignoreDamage> },
    { "m_lastJumpTime", field<PlayerObject, &PlayerObject::m_lastJumpTime> },
    { "m_lastLandTime", field<PlayerObject, &PlayerObject::m_lastLandTime> },
    { "m_lastFlipTime", field<PlayerObject, &PlayerObject::m_lastFlipTime> },
    { "m_totalTime", field<PlayerObject, &PlayerObject::m_totalTime> },
    { "m_lastPortalPos", field<PlayerObject, &PlayerObject::m_lastPortalPos> },
    { "m_lastGroundedPos", field<PlayerObject, &PlayerObject::m_lastGroundedPos> },
    { "m_playerColor1", field<PlayerObject, &PlayerObject::m_playerColor1> },
    { "m_playerColor2", field<PlayerObject, &PlayerObject::m_playerColor2> },
    { "m_gameLayer", field<PlayerObject, &PlayerObject::m_gameLayer> },
};

static const GameAccess::Method PLAYER_METHODS[] = {
    { "getRealPosition", method<&PlayerObject::getRealPosition>, 0 },
    { "setPosition", method<&PlayerObject::setPosition>, 1 },
    { "setRotation", method<&PlayerObject::setRotation>, 1 },
    { "setVisible", method<&PlayerObject::setVisible>, 1 },
    { "setColor", method<&PlayerObject::setColor>, 1 },
    { "setSecondColor", method<&PlayerObject::setSecondColor>, 1 },
    { "setYVelocity", method<&PlayerObject::setYVelocity>, 2 },
    { "boostPlayer", method<&PlayerObject::boostPlayer>, 1 },
    { "flipGravity", method<&PlayerObject::flipGravity>, 2 },
    { "togglePlayerScale", method<&PlayerObject::togglePlayerScale>, 2 },
    { "toggleFlyMode", method<&PlayerObject::toggleFlyMode>, 2 },
    { "toggleRollMode", method<&PlayerObject::toggleRollMode>, 2 },
    { "toggleBirdMode", method<&PlayerObject::toggleBirdMode>, 2 },
    { "toggleDartMode", method<&PlayerObject::toggleDartMode>, 2 },
    { "toggleRobotMode", method<&PlayerObject::toggleRobotMode>, 2 },
    { "toggleSpiderMode", method<&PlayerObject::toggleSpiderMode>, 2 },
    { "toggleSwingMode", method<&PlayerObject::toggleSwingMode>, 2 },
    { "stopDashing", method<&PlayerObject::stopDashing>, 0 },
    { "lockPlayer", method<&PlayerObject::lockPlayer>, 0 },
    { "enablePlayerControls", method<&PlayerObject::enablePlayerControls>, 0 },
    { "disablePlayerControls", method<&PlayerObject::disablePlayerControls>, 0 },
};

static const GameAccess::Field LEVEL_FIELDS[] = {
    { "m_levelID", seed_field<GJGameLevel, &GJGameLevel::m_levelID> },
    { "m_levelName", field<GJGameLevel, &GJGameLevel::m_levelName> },
    { "m_creatorName", field<GJGameLevel, &GJGameLevel::m_creatorName> },
    { "m_songID", field<GJGameLevel, &GJGameLevel::m_songID> },
    { "m_audioTrack", field<GJGameLevel, &GJGameLevel::m_audioTrack> },
    { "m_levelLength", field<GJGameLevel, &GJGameLevel::m_levelLength> },
    { "m_difficulty", field<GJGameLevel, &GJGameLevel::m_difficulty> },
    { "m_demonDifficulty", field<GJGameLevel, &GJGameLevel::m_demonDifficulty> },
    { "m_stars", seed_field<GJGameLevel, &GJGameLevel::m_stars> },
    { "m_coins", field<GJGameLevel, &GJGameLevel::m_coins> },
    { "m_twoPlayerMode", field<GJGameLevel, &GJGameLevel::m_twoPlayerMode> },
    { "m_attempts", seed_field<GJGameLevel, &GJGameLevel::m_attempts> },
    { "m_jumps", seed_field<GJGameLevel, &GJGameLevel::m_jumps> },
    { "m_normalPercent", seed_field<GJGameLevel, &GJGameLevel::m_normalPercent> },
    { "m_practicePercent", field<GJGameLevel, &GJGameLevel::m_practicePercent> },
    { "m_bestTime", field<GJGameLevel, &GJGameLevel::m_bestTime> },
    { "m_bestPoints", field<GJGameLevel, &GJGameLevel::m_bestPoints> },
    { "m_likes", field<GJGameLevel, &GJGameLevel::m_likes> },
    { "m_downloads", field<GJGameLevel, &GJGameLevel::m_downloads> },
    { "m_featured", field<GJGameLevel, &GJGameLevel::m_featured> },
    { "m_isEpic", field<GJGameLevel, &GJGameLevel::m_isEpic> },
    { "m_gameVersion", field<GJGameLevel, &GJGameLevel::m_gameVersion> },
};

static const GameAccess::Method LEVEL_METHODS[] = {
    { "getSongName", method<&GJGameLevel::getSongName>, 0 },
    { "getAudioFileName", method<&GJGameLevel::getAudioFileName>, 0 },
    { "getAverageDifficulty", method<&GJGameLevel::getAverageDifficulty>, 0 },
    { "areCoinsVerified", method<&GJGameLevel::areCoinsVerified>, 0 },
};

static const GameAccess::ClassInfo CLASSES[] = {
    { "GJBaseGameLayer", GAME_LAYER_FIELDS, (int)std::size(GAME_LAYER_FIELDS), GAME_LAYER_METHODS, (int)std::size(GAME_LAYER_METHODS), -1, nullptr },
    { "PlayLayer", PLAY_LAYER_FIELDS, (int)std::size(PLAY_LAYER_FIELDS), PLAY_LAYER_METHODS, (int)std::size(PLAY_LAYER_METHODS), 0, [](void* self) -> void* { return static_cast<GJBaseGameLayer*>(static_cast<PlayLayer*>(self)); } },
    { "PlayerObject", PLAYER_FIELDS, (int)std::size(PLAYER_FIELDS), PLAYER_METHODS, (int)std::size(PLAYER_METHODS), -1, nullptr },
    { "GJGameLevel", LEVEL_FIELDS, (int)std::size(LEVEL_FIELDS), LEVEL_METHODS, (int)std::size(LEVEL_METHODS), -1, nullptr },
};

static int g_generation = 0;

static void check_access(lua_State* lua) {
    if (!Logic::get().script_full_access) luaL_error(lua, "Full Game Access is off, turn it on on the Scripting page");
}

static void* upcast_to(void* pointer, int from, int to) {
    while (from != to) {
        const auto& info = GameAccess::class_at(from);
        if (info.base < 0) return nullptr;
        pointer = info.to_base(pointer);
        from = info.base;
    }
    return pointer;
}

static HeldObject& check_object(lua_State* lua, int index) {
    auto* object = static_cast<HeldObject*>(luaL_checkudata(lua, index, OBJECT_META));
    if (object->generation != g_generation)
        luaL_error(lua, "this %s is from an earlier call, get it again", GameAccess::class_at(object->id).name);
    return *object;
}

static const GameAccess::Method* find_method(int id, const char* name) {
    for (; id >= 0; id = GameAccess::class_at(id).base) {
        const auto& info = GameAccess::class_at(id);
        for (int i = 0; i < info.method_count; ++i) {
            if (std::string_view(name) == info.methods[i].name) return &info.methods[i];
        }
    }
    return nullptr;
}

static const GameAccess::Field* find_field(int id, const char* name, int& owner) {
    for (; id >= 0; id = GameAccess::class_at(id).base) {
        const auto& info = GameAccess::class_at(id);
        for (int i = 0; i < info.field_count; ++i) {
            if (std::string_view(name) == info.fields[i].name) {
                owner = id;
                return &info.fields[i];
            }
        }
    }
    return nullptr;
}

static int call_method(lua_State* lua) {
    const auto* method = static_cast<const GameAccess::Method*>(lua_touserdata(lua, lua_upvalueindex(1)));
    check_access(lua);
    int given = lua_gettop(lua) - 1;
    if (given != method->args) return luaL_error(lua, "%s takes %d arguments, not %d", method->name, method->args, given);
    return method->call(lua);
}

static int object_index(lua_State* lua) {
    check_access(lua);
    auto& object = check_object(lua, 1);
    const char* key = luaL_checkstring(lua, 2);
    if (const auto* method = find_method(object.id, key)) {
        lua_pushlightuserdata(lua, const_cast<GameAccess::Method*>(method));
        lua_pushcclosure(lua, call_method, 1);
        return 1;
    }
    int owner = -1;
    if (const auto* field = find_field(object.id, key, owner)) return field->access(lua, upcast_to(object.pointer, object.id, owner), 0);
    return luaL_error(lua, "%s has no %s", GameAccess::class_at(object.id).name, key);
}

static int object_newindex(lua_State* lua) {
    check_access(lua);
    auto& object = check_object(lua, 1);
    const char* key = luaL_checkstring(lua, 2);
    int owner = -1;
    const auto* field = find_field(object.id, key, owner);
    if (!field) return luaL_error(lua, "%s has no field %s", GameAccess::class_at(object.id).name, key);
    GameAccess::changed_game();
    return field->access(lua, upcast_to(object.pointer, object.id, owner), 3);
}

static int object_tostring(lua_State* lua) {
    auto* object = static_cast<HeldObject*>(luaL_checkudata(lua, 1, OBJECT_META));
    lua_pushstring(lua, GameAccess::class_at(object->id).name);
    return 1;
}

static int object_eq(lua_State* lua) {
    auto* first = static_cast<HeldObject*>(luaL_checkudata(lua, 1, OBJECT_META));
    auto* second = static_cast<HeldObject*>(luaL_checkudata(lua, 2, OBJECT_META));
    lua_pushboolean(lua, first->pointer == second->pointer);
    return 1;
}

static int gd_play_layer(lua_State* lua) {
    check_access(lua);
    GameAccess::push(lua, PlayLayer::get());
    return 1;
}

static int gd_player(lua_State* lua) {
    check_access(lua);
    PlayLayer* play = PlayLayer::get();
    int number = (int)luaL_optinteger(lua, 1, 1);
    if (!play || (number != 1 && number != 2)) lua_pushnil(lua);
    else GameAccess::push(lua, number == 1 ? play->m_player1 : play->m_player2);
    return 1;
}

static int gd_level(lua_State* lua) {
    check_access(lua);
    PlayLayer* play = PlayLayer::get();
    if (!play) lua_pushnil(lua);
    else GameAccess::push(lua, play->m_level);
    return 1;
}

namespace GameAccess {
    const ClassInfo& class_at(int id) {
        return CLASSES[id];
    }

    void open(lua_State* lua) {
        const luaL_Reg object_meta[] = {
            { "__index", object_index },
            { "__newindex", object_newindex },
            { "__tostring", object_tostring },
            { "__eq", object_eq },
            { nullptr, nullptr },
        };
        luaL_newmetatable(lua, OBJECT_META);
        luaL_setfuncs(lua, object_meta, 0);
        lua_pop(lua, 1);
        const luaL_Reg functions[] = {
            { "play_layer", gd_play_layer },
            { "player", gd_player },
            { "level", gd_level },
            { nullptr, nullptr },
        };
        lua_newtable(lua);
        luaL_setfuncs(lua, functions, 0);
        lua_setglobal(lua, "gd");
    }

    void expire() {
        ++g_generation;
    }

    void changed_game() {
        Logic::get().script_changed_game = true;
    }

    void push_object(lua_State* lua, void* pointer, int id) {
        if (!pointer) {
            lua_pushnil(lua);
            return;
        }
        *static_cast<HeldObject*>(lua_newuserdatauv(lua, sizeof(HeldObject), 0)) = { pointer, id, g_generation };
        luaL_setmetatable(lua, OBJECT_META);
    }

    void* to_object(lua_State* lua, int index, int id) {
        if (lua_isnoneornil(lua, index)) return nullptr;
        auto& object = check_object(lua, index);
        void* found = upcast_to(object.pointer, object.id, id);
        if (!found) luaL_error(lua, "expected %s, got %s", class_at(id).name, class_at(object.id).name);
        return found;
    }

    static float number_field(lua_State* lua, int index, const char* key) {
        luaL_checktype(lua, index, LUA_TTABLE);
        lua_getfield(lua, index, key);
        float value = (float)luaL_optnumber(lua, -1, 0.0);
        lua_pop(lua, 1);
        return value;
    }

    static void set_field(lua_State* lua, const char* key, lua_Number value) {
        lua_pushnumber(lua, value);
        lua_setfield(lua, -2, key);
    }

    cocos2d::CCPoint read_point(lua_State* lua, int index) {
        return { number_field(lua, index, "x"), number_field(lua, index, "y") };
    }

    cocos2d::ccColor3B read_color(lua_State* lua, int index) {
        return { (GLubyte)number_field(lua, index, "r"), (GLubyte)number_field(lua, index, "g"), (GLubyte)number_field(lua, index, "b") };
    }

    void push_point(lua_State* lua, const cocos2d::CCPoint& point) {
        lua_createtable(lua, 0, 2);
        set_field(lua, "x", point.x);
        set_field(lua, "y", point.y);
    }

    void push_color(lua_State* lua, const cocos2d::ccColor3B& color) {
        lua_createtable(lua, 0, 3);
        set_field(lua, "r", color.r);
        set_field(lua, "g", color.g);
        set_field(lua, "b", color.b);
    }
}
