#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/loader/Loader.hpp>
#include <Geode/modify/CCNode.hpp>
#include <Geode/modify/CCActionManager.hpp>
#include <algorithm>
#include <vector>
#include "child_sort.hpp"
#include "circle_signatures.hpp"
#include "wave_merge.hpp"
#include "dash_bursts.hpp"

using namespace geode::prelude;

// Spammed orbs leave hundreds of thousands of rings and sprites in the object layer, and every
// frame adds more. Cocos re-sorts the whole child array with an insertion sort each time, which
// was most of the frame. Large arrays take the same order from a stable sort instead.
namespace {
constexpr unsigned kLargeChildren = 1024;

bool supported() {
    static bool const value = [] {
        auto cocos = geode::base::getCocos();
        // sortAllChildren past the prologue Geode rewrites, and sortAllChildrenNoIndex whole.
        if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x5160), 0x2f) != 0x0daabc9b183489e9ull ||
            echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x5190), 0xfd) != 0xb5a2a048166c7a9eull) {
            log::warn("Echo large child sort skipped: native sort differs");
            return false;
        }
        // Another mod's hook on the same function keeps the native chain.
        auto address = cocos + 0x5150;
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get()) continue;
            for (auto* hook : mod->getHooks()) {
                if (hook->isEnabled() && hook->getAddress() == address) {
                    log::info("Echo large child sort skipped: {} hooks it", mod->getID());
                    return false;
                }
            }
        }
        log::info("Echo large child sort enabled (native order, stable merge)");
        return true;
    }();
    return value;
}
}

class $modify(EchoChildSort, CCNode) {
    void sortAllChildren() {
        auto* bytes = reinterpret_cast<uint8_t*>(this);
        // Native: only when reorder is dirty (0x120); the indexed variant (0x138) is left alone.
        if (!bytes[0x120] || bytes[0x138] || !m_pChildren || !m_pChildren->data ||
            m_pChildren->data->num < kLargeChildren || !supported()) {
            CCNode::sortAllChildren();
            return;
        }
        auto* data = m_pChildren->data;
        echo::nodes::stableSortChildren(reinterpret_cast<void**>(data->arr), data->num,
            [](void* node) { return static_cast<CCNode*>(node)->getZOrder(); },
            [](void* node) { return static_cast<CCNode*>(node)->getOrderOfArrival(); });
        bytes[0x120] = 0;
    }
};

// Spent rings and dash fire sprites remove themselves from the object layer from their last
// action. Each removal searched and then shifted every later child, megabytes per ring with
// hundreds of thousands of children. Inside the action update nothing walks or draws those
// children, so the array removal waits until the update ends and runs as one ordered pass.
// Everything else detachChild does (exit, cleanup, parent) still happens at the call.
namespace {
constexpr unsigned kDeferredRemoval = 4096;
bool deferring = false;
struct Pending { CCNode* parent; CCNode* child; };
std::vector<Pending> pending;

bool removalSupported() {
    static bool const value = [] {
        auto cocos = geode::base::getCocos();
        if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x3230), 0x8a) != 0x3830838f23ce6ff6ull ||
            echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x9de50), 0x90) != 0x408d66a5aea37beaull) {
            log::warn("Echo batched child removal skipped: native removal differs");
            return false;
        }
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get()) continue;
            for (auto* hook : mod->getHooks()) {
                auto address = hook->getAddress();
                if (hook->isEnabled() && (address == cocos + 0x46d0 || address == cocos + 0x3220 || address == cocos + 0x38b90 ||
                                          address == geode::base::get() + 0x3b48f0)) {
                    log::info("Echo batched child removal skipped: {} hooks removal", mod->getID());
                    return false;
                }
            }
        }
        log::info("Echo batched child removal enabled");
        return true;
    }();
    return value;
}

// PlayLayer also keeps every live ring in m_circleWaveArray and takes each one out with
// CCArray::removeObject from circleWaveWillBeRemoved, the same search and shift.
struct PendingObject { CCArray* array; CCObject* object; };
std::vector<PendingObject> pendingObjects;
using WaveRemoved = void (*)(void*, CCCircleWave*);
WaveRemoved nativeWaveRemoved = nullptr;

void flushArrays() {
    if (pendingObjects.empty()) return;
    std::vector<CCArray*> arrays;
    arrays.reserve(pendingObjects.size());
    for (auto const& item : pendingObjects) arrays.push_back(item.array);
    std::sort(pendingObjects.begin(), pendingObjects.end(), [](PendingObject const& a, PendingObject const& b) {
        return a.array != b.array ? a.array < b.array : a.object < b.object;
    });
    // Each queued removeObject takes out one occurrence; a repeat finds nothing more natively
    // only when the object was there once, so count the requests per object.
    std::vector<CCObject*> released;
    for (size_t first = 0; first < pendingObjects.size();) {
        size_t last = first;
        while (last < pendingObjects.size() && pendingObjects[last].array == pendingObjects[first].array) ++last;
        auto* data = pendingObjects[first].array->data;
        if (data) {
            auto begin = pendingObjects.begin() + static_cast<std::ptrdiff_t>(first);
            auto end = pendingObjects.begin() + static_cast<std::ptrdiff_t>(last);
            auto low = begin->object, high = (end - 1)->object;
            unsigned write = 0;
            for (unsigned read = 0; read < data->num; ++read) {
                auto* object = data->arr[read];
                bool take = false;
                if (object >= low && object <= high) {
                    auto found = std::lower_bound(begin, end, object, [](PendingObject const& p, CCObject* o) { return p.object < o; });
                    // The first unused request for this object takes this occurrence.
                    while (found != end && found->object == object && !found->array) ++found;
                    if (found != end && found->object == object) {
                        found->array = nullptr;
                        take = true;
                    }
                }
                if (take) released.push_back(object);
                else data->arr[write++] = object;
            }
            data->num = write;
        }
        first = last;
    }
    pendingObjects.clear();
    for (auto* object : released) object->release();
    for (auto* array : arrays) array->release();
}

void waveRemoved(void* delegate, CCCircleWave* wave) {
    auto* array = *reinterpret_cast<CCArray**>(static_cast<uint8_t*>(delegate) + 0x118);
    if (!deferring || !array || !array->data || array->data->num < kDeferredRemoval || !removalSupported()) {
        nativeWaveRemoved(delegate, wave);
        return;
    }
    array->retain();
    pendingObjects.push_back({array, wave});
}

void flushRemovals() {
    flushArrays();
    if (pending.empty()) return;
    std::vector<CCNode*> parents;
    parents.reserve(pending.size());
    for (auto const& item : pending) parents.push_back(item.parent);
    // Group by parent; within a parent, the set of children to take out.
    std::sort(pending.begin(), pending.end(), [](Pending const& a, Pending const& b) {
        return a.parent != b.parent ? a.parent < b.parent : a.child < b.child;
    });
    pending.erase(std::unique(pending.begin(), pending.end(), [](Pending const& a, Pending const& b) {
        return a.parent == b.parent && a.child == b.child;
    }), pending.end());
    std::vector<CCNode*> released;
    for (size_t first = 0; first < pending.size();) {
        size_t last = first;
        while (last < pending.size() && pending[last].parent == pending[first].parent) ++last;
        auto* parent = pending[first].parent;
        auto* children = parent->getChildren();
        if (children && children->data) {
            auto* data = children->data;
            auto begin = pending.begin() + static_cast<std::ptrdiff_t>(first);
            auto end = pending.begin() + static_cast<std::ptrdiff_t>(last);
            auto low = begin->child, high = (end - 1)->child;
            auto take = [&](CCObject* object) {
                auto* node = static_cast<CCNode*>(object);
                if (node < low || node > high) return false;
                auto found = std::lower_bound(begin, end, node, [](Pending const& p, CCNode* n) { return p.child < n; });
                if (found == end || found->child != node || !found->parent) return false;
                found->parent = nullptr; // Native removes the first occurrence only.
                return true;
            };
            unsigned write = 0;
            for (unsigned read = 0; read < data->num; ++read) {
                auto* object = data->arr[read];
                if (take(object)) released.push_back(static_cast<CCNode*>(object));
                else data->arr[write++] = object;
            }
            data->num = write;
        }
        first = last;
    }
    pending.clear();
    // ccArrayRemoveObject releases the child it takes out.
    for (auto* node : released) node->release();
    for (auto* parent : parents) parent->release();
}
}

class $modify(EchoDeferredRemoval, CCNode) {
    void removeChild(CCNode* child, bool cleanup) {
        echo::bursts::noteRemoved(child);
        auto* bytes = reinterpret_cast<uint8_t*>(this);
        if (!deferring || !child || !m_pChildren || !m_pChildren->data || bytes[0x138] ||
            m_pChildren->data->num < kDeferredRemoval || !removalSupported()) {
            CCNode::removeChild(child, cleanup);
            return;
        }
        // detachChild, up to its array removal.
        if (m_bRunning) {
            child->onExitTransitionDidStart();
            child->onExit();
        }
        if (cleanup) child->cleanup();
        child->setParent(nullptr);
        // An action can end the level and free this layer before the flush.
        retain();
        pending.push_back({this, child});
    }
};

class $modify(EchoRemovalBatch, CCActionManager) {
    void update(float dt) {
        bool outer = !deferring;
        deferring = true;
        // Fresh rings are first stepped by the director's manager; merge right before it, so
        // every ring made since its last update joins (GD also runs a per-step manager).
        if (outer && this == CCDirector::sharedDirector()->getActionManager()) {
            echo::waves::mergeFresh();
            echo::bursts::mergeFresh();
        }
        CCActionManager::update(dt);
        if (outer) {
            deferring = false;
            flushRemovals();
        }
    }
};

$execute {
    auto address = geode::base::get() + 0x3b48f0;
    // The whole routine: load m_circleWaveArray, removeObject(wave, true).
    if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(address), 0x10) != 0x47cecc787b34ac87ull) {
        log::warn("Echo batched ring removal skipped: native routine differs");
        return;
    }
    nativeWaveRemoved = reinterpret_cast<WaveRemoved>(address);
    if (!Mod::get()->hook(reinterpret_cast<void*>(address), &waveRemoved, "PlayLayer::circleWaveWillBeRemoved"))
        log::warn("Echo batched ring removal hook failed");
}
#endif
