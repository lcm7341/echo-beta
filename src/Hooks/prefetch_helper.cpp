#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include "Logic/logic.hpp"
#include <Geode/modify/CCActionManager.hpp>
#include <windows.h>
#include <atomic>
#include <cstring>
#include <thread>
#include <xmmintrin.h>

using namespace geode::prelude;

#define MANAGER_TARGETS_OFFSET 0x38
#define MANAGER_CURRENT_TARGET_OFFSET 0x40
#define ELEMENT_ACTIONS_OFFSET 0x0
#define ELEMENT_TARGET_OFFSET 0x8
#define ELEMENT_NEXT_OFFSET 0x38
#define CCARRAY_DATA_OFFSET 0x38
#define ARRAY_COUNT_OFFSET 0x0
#define ARRAY_ITEMS_OFFSET 0x10
#define NODE_CHILDREN_OFFSET 0xd0
#define MAX_PREFETCH_ITEMS 8

enum PrefetchMode : int { PREFETCH_IDLE, PREFETCH_ACTIONS };

static std::atomic<int> g_mode = PREFETCH_IDLE;
static std::atomic<bool> g_running = false;
static void* g_subject = nullptr;
static bool g_thread_started = false;

static bool plausible(const void* pointer) {
    const unsigned long long lowest_user_address = 65536;
    const unsigned long long user_space_limit = 140737488355327ull;
    const unsigned long long pointer_align_mask = 7;
    auto bits = reinterpret_cast<unsigned long long>(pointer);
    return bits >= lowest_user_address && bits < user_space_limit && (bits & pointer_align_mask) == 0;
}

template <class T>
static T read(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const char*>(object) + offset, sizeof(T));
    return value;
}

static void touch(const void* object, size_t offset) {
    _mm_prefetch(static_cast<const char*>(object) + offset, _MM_HINT_T1);
}

static void touch_sprite(const void* sprite) {
    for (size_t offset : { (size_t)0x0, (size_t)0x40, (size_t)0x80, (size_t)0xc0, (size_t)0x100, (size_t)0x140, (size_t)0x180, (size_t)0x1c0, (size_t)0x200 })
        touch(sprite, offset);
}

static void touch_children(const void* node) {
    auto* children = read<const void*>(node, NODE_CHILDREN_OFFSET);
    if (!plausible(children)) return;
    auto* data = read<const void*>(children, CCARRAY_DATA_OFFSET);
    if (!plausible(data)) return;
    unsigned count = read<unsigned>(data, ARRAY_COUNT_OFFSET);
    auto* items = read<const void* const*>(data, ARRAY_ITEMS_OFFSET);
    if (!plausible(items) || count == 0 || count > MAX_PREFETCH_ITEMS) return;
    for (unsigned at = 0; at < count; ++at) {
        auto* child = items[at];
        if (plausible(child)) touch_sprite(child);
    }
}

static void touch_element(const void* element) {
    touch(element, 0);
    auto* actions = read<const void*>(element, ELEMENT_ACTIONS_OFFSET);
    auto* target = read<const void*>(element, ELEMENT_TARGET_OFFSET);
    if (plausible(target)) {
        touch_sprite(target);
        touch_children(target);
    }
    if (!plausible(actions)) return;
    unsigned count = read<unsigned>(actions, ARRAY_COUNT_OFFSET);
    auto* items = read<const void* const*>(actions, ARRAY_ITEMS_OFFSET);
    if (!plausible(items) || count == 0 || count > MAX_PREFETCH_ITEMS) return;
    for (unsigned at = 0; at < count; ++at) {
        auto* action = items[at];
        if (!plausible(action)) continue;
        touch(action, 0);
        touch(action, 0x40);
        touch(action, 0x80);
    }
}

static void walk_actions(const void* manager) {
    const int action_lookahead = 1536;
    const int mode_check_every = 64;
    const int idle_spins = 256;
    __try {
        while (g_mode.load(std::memory_order_acquire) == PREFETCH_ACTIONS) {
            auto* element = read<const void*>(manager, MANAGER_CURRENT_TARGET_OFFSET);
            if (!plausible(element)) element = read<const void*>(manager, MANAGER_TARGETS_OFFSET);
            for (int looked_ahead = 0; looked_ahead < action_lookahead && plausible(element); ++looked_ahead) {
                if ((looked_ahead % mode_check_every) == 0 && g_mode.load(std::memory_order_acquire) != PREFETCH_ACTIONS)
                    return;
                touch_element(element);
                element = read<const void*>(element, ELEMENT_NEXT_OFFSET);
            }
            for (int spin = 0; spin < idle_spins; ++spin)
                _mm_pause();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void helper_loop() {
    for (;;) {
        g_mode.wait(PREFETCH_IDLE, std::memory_order_acquire);
        int mode = g_mode.load(std::memory_order_acquire);
        if (mode == PREFETCH_IDLE) continue;
        g_running.store(true, std::memory_order_release);
        if (mode == PREFETCH_ACTIONS) walk_actions(g_subject);
        g_running.store(false, std::memory_order_release);
        while (g_mode.load(std::memory_order_acquire) == mode)
            _mm_pause();
    }
}

static void start_prefetch(int mode, void* subject) {
    if (!g_thread_started) {
        g_thread_started = true;
        std::thread(helper_loop).detach();
    }
    g_subject = subject;
    g_mode.store(mode, std::memory_order_release);
    g_mode.notify_one();
}

static void stop_prefetch() {
    g_mode.store(PREFETCH_IDLE, std::memory_order_release);
    while (g_running.load(std::memory_order_acquire))
        _mm_pause();
}

static CCNode* busy_object_layer() {
    const unsigned min_prefetch_children = 8192;
    auto& logic = Logic::get();
    if (!logic.prefetch_helper) return nullptr;
    auto* play = PlayLayer::get();
    if (!play || !play->m_objectLayer || play->m_objectLayer->getChildrenCount() < min_prefetch_children)
        return nullptr;
    return play->m_objectLayer;
}

class $modify(EchoPrefetchActions, CCActionManager) {
    void update(float dt) {
        if (g_mode.load(std::memory_order_relaxed) != PREFETCH_IDLE || this != CCDirector::sharedDirector()->getActionManager() || !busy_object_layer()) {
            CCActionManager::update(dt);
            return;
        }
        start_prefetch(PREFETCH_ACTIONS, this);
        CCActionManager::update(dt);
        stop_prefetch();
    }
};

#endif
