// faster sorting & removing of children on big nodes. windows
//
// cocos re-sorts ALL the children of a node w/ an insertion sort whenever something was added. the object layer has tens of
//   thousands of them, so w/ orb spam at high tps that sort was most of the frame
// sortAllChildren: nodes w/ LARGE_CHILDREN or more get a stable sort by (z order, order of arrival). same order out, way less work
// removal: finished effects are collected & taken out in 1 pass, not 1 array shift each
// only on if cocos's own code hashes right & no other mod hooked sortAllChildren
#include <Geode/Geode.hpp>
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

namespace {
    constexpr unsigned LARGE_CHILDREN = 1024;

    bool supported() {
        static bool const value = [] {
            auto cocos = geode::base::getCocos();
            // gd code hash
            if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x5160), 0x2f) != 0x0daabc9b183489e9ull ||
                echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x5190), 0xfd) != 0xb5a2a048166c7a9eull) {
                return false;
            }
            auto address = cocos + 0x5150; // sortAllChildren
            for (auto* mod : Loader::get()->getAllMods()) {
                if (mod == Mod::get())
                    continue;
                for (auto* hook : mod->getHooks()) {
                    if (hook->isEnabled() && hook->getAddress() == address) {
                        return false;
                    }
                }
            }
            return true;
        }();
        return value;
    }
}

class $modify(EchoChildSort, CCNode) {
    void sortAllChildren() {
        auto* bytes = reinterpret_cast<uint8_t*>(this);
        if (!bytes[0x120] || bytes[0x138] || !m_pChildren || !m_pChildren->data || // 0x120 = needs sorting. 0x138 = the indexed kind
            m_pChildren->data->num < LARGE_CHILDREN || !supported()) {
            CCNode::sortAllChildren();
            return;
        }
        auto* data = m_pChildren->data;
        echo::nodes::stable_sort_children(reinterpret_cast<void**>(data->arr), data->num, [](void* node) {
            return static_cast<CCNode*>(node)->getZOrder();
        },
            [](void* node) {
                return static_cast<CCNode*>(node)->getOrderOfArrival();
            });
        bytes[0x120] = 0;
    }
};

namespace {
    constexpr unsigned DEFERRED_REMOVAL = 4096;
    bool deferring = false;

    struct Pending {
        CCNode* parent;
        CCNode* child;
    };

    std::vector<Pending> pending;

    bool removal_supported() {
        static bool const value = [] {
            auto cocos = geode::base::getCocos();
            // gd code hash
            if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x3230), 0x8a) != 0x3830838f23ce6ff6ull ||  // tf
                echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(cocos + 0x9de50), 0x90) != 0x408d66a5aea37beaull) { // tf
                return false;
            }
            for (auto* mod : Loader::get()->getAllMods()) {
                if (mod == Mod::get())
                    continue;
                for (auto* hook : mod->getHooks()) {
                    auto address = hook->getAddress();
                    if (hook->isEnabled() && (address == cocos + 0x46d0 || address == cocos + 0x3220 || address == cocos + 0x38b90 ||
                                                 address == geode::base::get() + 0x3b48f0)) {
                        return false;
                    }
                }
            }
            return true;
        }();
        return value;
    }

    struct PendingObject {
        CCArray* array;
        CCObject* object;
    };

    std::vector<PendingObject> pending_objects;
    using WaveRemoved = void (*)(void*, CCCircleWave*);
    WaveRemoved native_wave_removed = nullptr;

    void flush_arrays() {
        if (pending_objects.empty())
            return;
        std::vector<CCArray*> arrays;
        arrays.reserve(pending_objects.size());
        for (auto const& item : pending_objects)
            arrays.push_back(item.array);
        std::sort(pending_objects.begin(), pending_objects.end(), [](PendingObject const& a, PendingObject const& b) {
            return a.array != b.array ? a.array < b.array : a.object < b.object;
        });
        std::vector<CCObject*> released;
        for (size_t first = 0; first < pending_objects.size();) {
            size_t last = first;
            while (last < pending_objects.size() && pending_objects[last].array == pending_objects[first].array)
                ++last;
            auto* data = pending_objects[first].array->data;
            if (data) {
                auto begin = pending_objects.begin() + static_cast<std::ptrdiff_t>(first);
                auto end = pending_objects.begin() + static_cast<std::ptrdiff_t>(last);
                auto low = begin->object, high = (end - 1)->object;
                unsigned write = 0;
                for (unsigned read = 0; read < data->num; ++read) {
                    auto* obj = data->arr[read];
                    bool take = false;
                    if (obj >= low && obj <= high) {
                        auto it = std::lower_bound(begin, end, obj, [](PendingObject const& p, CCObject* o) {
                            return p.object < o;
                        });
                        while (it != end && it->object == obj && !it->array)
                            ++it;
                        if (it != end && it->object == obj) {
                            it->array = nullptr;
                            take = true;
                        }
                    }
                    if (take)
                        released.push_back(obj);
                    else
                        data->arr[write++] = obj;
                }
                data->num = write;
            }
            first = last;
        }
        pending_objects.clear();
        for (auto* obj : released)
            obj->release();
        for (auto* array : arrays)
            array->release();
    }

    void wave_removed(void* delegate, CCCircleWave* wave) {
        auto* array = *reinterpret_cast<CCArray**>(static_cast<uint8_t*>(delegate) + 0x118); // m_circleWaveArray
        if (!deferring || !array || !array->data || array->data->num < DEFERRED_REMOVAL || !removal_supported()) {
            native_wave_removed(delegate, wave);
            return;
        }
        array->retain();
        pending_objects.push_back({ array, wave });
    }

    void flush_removals() {
        flush_arrays();
        if (pending.empty())
            return;
        std::vector<CCNode*> parents;
        parents.reserve(pending.size());
        for (auto const& item : pending)
            parents.push_back(item.parent);
        std::sort(pending.begin(), pending.end(), [](Pending const& a, Pending const& b) {
            return a.parent != b.parent ? a.parent < b.parent : a.child < b.child;
        });
        pending.erase(std::unique(pending.begin(), pending.end(), [](Pending const& a, Pending const& b) {
            return a.parent == b.parent && a.child == b.child;
        }),
            pending.end());
        std::vector<CCNode*> released;
        for (size_t first = 0; first < pending.size();) {
            size_t last = first;
            while (last < pending.size() && pending[last].parent == pending[first].parent)
                ++last;
            auto* parent = pending[first].parent;
            auto* children = parent->getChildren();
            if (children && children->data) {
                auto* data = children->data;
                auto begin = pending.begin() + static_cast<std::ptrdiff_t>(first);
                auto end = pending.begin() + static_cast<std::ptrdiff_t>(last);
                auto low = begin->child, high = (end - 1)->child;
                auto take = [&](CCObject* object) {
                    auto* node = static_cast<CCNode*>(object);
                    if (node < low || node > high)
                        return false;
                    auto it = std::lower_bound(begin, end, node, [](Pending const& p, CCNode* n) {
                        return p.child < n;
                    });
                    if (it == end || it->child != node || !it->parent)
                        return false;
                    it->parent = nullptr; // only the first one like rob does
                    return true;
                };
                unsigned write = 0;
                for (unsigned read = 0; read < data->num; ++read) {
                    auto* obj = data->arr[read];
                    if (take(obj))
                        released.push_back(static_cast<CCNode*>(obj));
                    else
                        data->arr[write++] = obj;
                }
                data->num = write;
            }
            first = last;
        }
        pending.clear();
        for (auto* node : released)
            node->release();
        for (auto* parent : parents)
            parent->release();
    }
}

class $modify(EchoDeferredRemoval, CCNode) {
    void removeChild(CCNode* child, bool cleanup) {
        echo::bursts::note_removed(child);
        auto* bytes = reinterpret_cast<uint8_t*>(this);
        if (!deferring || !child || !m_pChildren || !m_pChildren->data || bytes[0x138] ||
            m_pChildren->data->num < DEFERRED_REMOVAL || !removal_supported()) {
            CCNode::removeChild(child, cleanup);
            return;
        }
        if (m_bRunning) {
            child->onExitTransitionDidStart();
            child->onExit();
        }
        if (cleanup)
            child->cleanup();
        child->setParent(nullptr);
        retain();
        pending.push_back({ this, child });
    }
};

class $modify(EchoRemovalBatch, CCActionManager) {
    void update(float dt) {
        bool outer = !deferring;
        deferring = true;
        if (outer && this == CCDirector::sharedDirector()->getActionManager()) {
            echo::waves::merge_fresh();
            echo::bursts::merge_fresh();
        }
        CCActionManager::update(dt);
        if (outer) {
            deferring = false;
            flush_removals();
        }
    }
};

$execute {
    auto address = geode::base::get() + 0x3b48f0; // circleWaveWillBeRemoved
    // gd code hash
    if (echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(address), 0x10) != 0x47cecc787b34ac87ull) {
        return;
    }
    native_wave_removed = reinterpret_cast<WaveRemoved>(address);
    (void)Mod::get()->hook(reinterpret_cast<void*>(address), &wave_removed, "PlayLayer::circleWaveWillBeRemoved");
}
