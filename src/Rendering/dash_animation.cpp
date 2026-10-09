#include <Geode/Geode.hpp>
#include "circle_signatures.hpp"
#include "dash_timeline.hpp"
#include "dash_batch.hpp"
#include "dash_end.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace geode::prelude;
#ifdef GEODE_IS_WINDOWS
namespace {
using Removal = void (*)(CCSprite*);
Removal nativeRemoval = nullptr;

class DashAnimation final : public CCActionInterval {
    echo::dash::Timeline timeline;
    float endX{}, endY{};
    CCSprite* removalTarget = nullptr;
    bool registeredDraw = false;
    struct Target {
        CCNode* node;
        CCSprite* removal;
        mutable CCPoint positionValue;
        float positionX() const { positionValue = node->getPosition(); return positionValue.x; }
        float positionY() const { return positionValue.y; }
        float scaleX() const { return node->getScaleX(); }
        float scaleY() const { return node->getScaleY(); }
        unsigned char opacity() const { return static_cast<CCSprite*>(node)->getOpacity(); }
        void scale(float x, float y) { node->setScaleX(x); node->setScaleY(y); }
        void position(float x, float y) { node->setPosition({x, y}); }
        void opacity(unsigned char value) { static_cast<CCSprite*>(node)->setOpacity(value); }
        void remove() { nativeRemoval(removal); }
    };
public:
    ~DashAnimation() override {
        if (removalTarget) {
            if (registeredDraw) echo::dash::untrackDraw(removalTarget);
            removalTarget->release();
        }
    }
    bool init(float x, float y, CCSprite* removal) {
        if (!CCActionInterval::initWithDuration(0.2f)) return false;
        removal->retain();
        if (removalTarget) {
            if (registeredDraw) echo::dash::untrackDraw(removalTarget);
            removalTarget->release();
        }
        registeredDraw = false;
        removalTarget = removal; endX = x; endY = y;
        return true;
    }
    void registerDraw(CCSprite* outline) {
        echo::dash::trackDraw(removalTarget,outline);
        registeredDraw = true;
    }
    void startWithTarget(CCNode* node) override {
        CCActionInterval::startWithTarget(node);
        Target target{node, removalTarget};
        timeline.start(target, endX, endY);
    }
    void update(float time) override {
        Target target{m_pTarget, removalTarget};
        timeline.update(time, target);
    }
    CCObject* copyWithZone(CCZone* zone) override {
        auto* copy = zone && zone->m_pCopyObject ? static_cast<DashAnimation*>(zone->m_pCopyObject) : new DashAnimation;
        if (!copy->init(endX, endY, removalTarget)) {
            if (!zone || !zone->m_pCopyObject) delete copy;
            return nullptr;
        }
        return copy;
    }
    CCActionInterval* reverse() override {
        // Native CCScaleTo does not support reversing. Keep its native API
        // behavior rather than inventing a reverse animation for this effect.
        return CCScaleTo::create(getDuration(), endX, endY)->reverse();
    }
};

void installAnimation(CCSprite* root, CCSprite* outline) {
    float y = root->getScaleY() * 0.2f;
    float x = root->getScaleX() * 0.2f;
    auto* animation = new DashAnimation;
    if (animation->init(x, y, root)) {
        // Only batching reads the draw registry; skip its bookkeeping while it is off.
        if (echoDashBatchingEnabled) animation->registerDraw(outline);
        animation->autorelease();
        root->runAction(animation);
    } else {
        delete animation;
        // Allocation/init fallback preserves the original three root actions.
        auto* removal = CCCallFunc::create(root, callfunc_selector(CCNode::removeMeAndCleanup));
        root->runAction(CCSequence::create(CCScaleTo::create(0.2f, x, y), removal, nullptr));
        root->runAction(CCMoveBy::create(0.2f, {-20.f, 0.f}));
        root->runAction(CCFadeTo::create(0.2f, 0));
    }
    // Keep the outline's independent target, fade clock and action API.
    outline->runAction(CCFadeTo::create(0.2f, 0));
    if (echoDashCompare) echo::dash::pairNative(root, outline);
}

// stopDashing builds its end effect in 0x39678e..0x396a4e, entered only when
// [player+0x7e4] is set and [player+0x7e1] is clear. That block creates two sprites,
// adds them to the object layer and starts their actions. It writes nothing to the
// player, and no register it sets is read after 0x396a4e, so skipping it is the
// same path the game takes when [player+0x7e1] is set.
constexpr std::uintptr_t gateSite = 0x396780;
constexpr std::array<std::uint8_t, 8> gateOriginal{0x41,0x80,0xbe,0xe1,0x07,0x00,0x00,0x00}; // cmp byte [r14+0x7e1], 0
constexpr std::size_t gateMode = 0x40, gateRecord = 0x48;
volatile std::uint8_t* modeByte = nullptr;
Patch* effectGatePatch = nullptr;
Patch* effectActionsPatch = nullptr;

// One page within rel32 reach of the game, so the call site and the flag read stay rip-relative.
std::uint8_t* allocateNear(std::uintptr_t target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    std::uintptr_t const step = info.dwAllocationGranularity;
    std::uintptr_t const reach = 0x7ff00000;
    auto tryAt = [](std::uintptr_t address) {
        return static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    };
    std::uintptr_t floor = target > reach ? target - reach : step;
    std::uintptr_t address = (target & ~(step - 1)) - step;
    while (address >= floor) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) break;
        if (region.State == MEM_FREE) if (auto* page = tryAt(address)) return page;
        std::uintptr_t next = region.State == MEM_FREE ? address : reinterpret_cast<std::uintptr_t>(region.AllocationBase);
        if (next < floor + step) break;
        address = (next - 1) & ~(step - 1);
    }
    address = (target + step) & ~(step - 1);
    while (address + 0x1000 < target + reach) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) break;
        if (region.State == MEM_FREE) if (auto* page = tryAt(address)) return page;
        auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        address = std::max(address + step, (end + step - 1) & ~(step - 1));
    }
    return nullptr;
}

bool installGate(std::uintptr_t module) {
    auto site = module + gateSite;
    if (std::memcmp(reinterpret_cast<void const*>(site), gateOriginal.data(), gateOriginal.size()) != 0) return false;
    auto* cave = allocateNear(site);
    if (!cave) return false;
    auto distance = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(cave)) - static_cast<std::int64_t>(site + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) { VirtualFree(cave, 0, MEM_RELEASE); return false; }
    // The original compare decides first. Only when the game would build the effect does the
    // mode byte apply: bit 1 calls Echo's recorder here, where the block would read the player,
    // and bit 0 skips the block. jne reads only ZF. The call keeps the native frame's 16-byte
    // alignment and shadow space; rax, rcx and the other volatiles are dead at both targets.
    constexpr std::array<std::uint8_t, 61> stub{
        0x41,0x80,0xbe,0xe1,0x07,0x00,0x00,0x00, //  0: cmp byte [r14+0x7e1], 0
        0x75,0x32,                               //  8: jne 60 (the game skips; ZF=0)
        0x0f,0xb6,0x05,0x2f,0x00,0x00,0x00,      // 10: movzx eax, byte [mode]
        0x84,0xc0,                               // 17: test al, al
        0x74,0x27,                               // 19: je 60 (native; ZF=1)
        0xa8,0x02,                               // 21: test al, 2
        0x74,0x1f,                               // 23: je 56 (skip without recording)
        0x48,0x83,0xec,0x28,                     // 25: sub rsp, 0x28
        0x4c,0x89,0xf1,                          // 29: mov rcx, r14
        0xff,0x15,0x22,0x00,0x00,0x00,           // 32: call [record]
        0x48,0x83,0xc4,0x28,                     // 38: add rsp, 0x28
        0x0f,0xb6,0x05,0x0f,0x00,0x00,0x00,      // 42: movzx eax, byte [mode]
        0xa8,0x01,                               // 49: test al, 1
        0x75,0x03,                               // 51: jne 56
        0x31,0xc0,                               // 53: xor eax, eax (build; ZF=1)
        0xc3,                                    // 55: ret
        0x48,0x85,0xe4,                          // 56: test rsp, rsp (skip; ZF=0)
        0xc3,                                    // 59: ret
        0xc3,                                    // 60: ret
    };
    static_assert(17 + 0x2f == gateMode && 49 + 0x0f == gateMode && 38 + 0x22 == gateRecord);
    std::memcpy(cave, stub.data(), stub.size());
    cave[gateMode] = 0;
    auto record = reinterpret_cast<std::uintptr_t>(&echoRecordDashEnd);
    std::memcpy(cave + gateRecord, &record, sizeof(record));
    FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
    std::vector<std::uint8_t> call{0xe8, 0, 0, 0, 0, 0x0f, 0x1f, 0x00}; // call cave; nop
    auto rel = static_cast<std::int32_t>(distance);
    std::memcpy(call.data() + 1, &rel, sizeof(rel));
    auto result = Mod::get()->patch(reinterpret_cast<void*>(site), call);
    if (!result) {
        log::warn("Echo dash-end effect gate patch failed: {}", result.unwrapErr());
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }
    modeByte = cave + gateMode;
    effectGatePatch = result.unwrap();
    return true;
}
}

namespace echo::dash {
void prepare() {
    if (!effectGatePatch && !effectActionsPatch) return;
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks()) {
            if (!hook->isEnabled() || hook->getAddress() != geode::base::get() + 0x396650) continue;
            for (auto** patch : {&effectGatePatch, &effectActionsPatch}) {
                if (!*patch) continue;
                if (auto result = (*patch)->disable(); result) *patch = nullptr;
                else log::error("Echo could not restore native dash patch: {}", result.unwrapErr());
            }
            modeByte = nullptr;
            log::info("Echo dash-end patches stand aside for {}", mod->getID());
            return;
        }
    }
}
bool endEffectSkippable(PlayerObject* player) {
    if (!modeByte || !player) return false;
    auto* bytes = reinterpret_cast<std::uint8_t const*>(player);
    return bytes[0x7e4] != 0 && bytes[0x7e1] == 0;
}
void setEndEffect(EndEffect mode) {
    resetPendingPair();
    if (modeByte) *modeByte = static_cast<std::uint8_t>(mode);
}
}

$execute {
    using echo::circles::Signature;
    // Body excludes the entry that other mods may hook. Every effect parameter,
    // native callback, remaining physics instruction, and register use is guarded.
    constexpr Signature stopBody{0x396670, 0x691, 0xc33c2694ed4a2084};
    constexpr Signature actions{0x396987, 0xc7, 0xc266d6572a611a17};
    auto module = geode::base::get();
    auto matches = [=](Signature signature) {
        return echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(module + signature.offset), signature.size) == signature.hash;
    };
    // Both patches below change bytes inside stopBody; check it before either is installed.
    if (!matches(stopBody)) {
        log::warn("Echo dash-end effect instructions differ; effect gate and compact animation skipped");
        return;
    }
    if (installGate(module)) log::info("Echo dash-end effect gate enabled");
    else log::warn("Echo dash-end effect gate unavailable; every end effect is built");
    if (!matches(actions)) {
        log::warn("Echo dash-end effect instructions differ; compact animation skipped");
        return;
    }
    constexpr std::array<Signature, 4> nativeMath{{
        {0x36f20, 0x70, 0x299bd24e337466e1},
        {0x370d0, 0xd0, 0x4e018c4e723c0e05},
        {0x373e0, 0x4f, 0x1dbc993295bc70b8},
        {0x37430, 0x114, 0x94c7bb6ce9a8c49e},
    }};
    auto cocos = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("libcocos2d.dll"));
    for (auto signature : nativeMath) {
        if (!cocos || echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(cocos + signature.offset), signature.size) != signature.hash) {
            log::warn("Echo dash-end native action math differs; compact animation skipped");
            return;
        }
    }
    nativeRemoval = reinterpret_cast<Removal>(module + 0x3be70);
    // Native stopDashing has aligned stack/shadow space here. R15 is the fully
    // configured effect, RSI its outline. Windows preserves its live nonvolatile
    // registers; the original physics continuation is left intact.
    std::vector<std::uint8_t> patch(actions.size, 0x90);
    constexpr std::array<std::uint8_t, 8> prefix{0x49,0x8b,0xcf,0x48,0x8b,0xd6,0x48,0xb8};
    std::copy(prefix.begin(), prefix.end(), patch.begin());
    auto destination = reinterpret_cast<std::uintptr_t>(&installAnimation);
    std::memcpy(patch.data() + 8, &destination, sizeof(destination));
    patch[16] = 0xff; patch[17] = 0xd0; // call rax
    patch[18] = 0xe9;
    std::int32_t displacement = static_cast<std::int32_t>(actions.size - 23);
    std::memcpy(patch.data() + 19, &displacement, sizeof(displacement));
    auto result = Mod::get()->patch(reinterpret_cast<void*>(module + actions.offset), patch);
    if (!result) { log::warn("Echo compact dash-end animation patch failed: {}", result.unwrapErr()); return; }
    effectActionsPatch = result.unwrap();
    log::info("Echo compact dash-end animations enabled (six actions to two; sprites preserved)");
}
#endif
