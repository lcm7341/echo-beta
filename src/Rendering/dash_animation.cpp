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
    Removal native_removal = nullptr;

    class DashAnimation final : public CCActionInterval {
        echo::dash::Timeline timeline;
        float end_x{}, end_y{};
        CCSprite* removal_target = nullptr;
        bool registered_draw = false;

        struct Target {
            CCNode* node;
            CCSprite* removal;
            mutable CCPoint pos_value;

            float pos_x() const {
                pos_value = node->getPosition();
                return pos_value.x;
            }

            float pos_y() const {
                return pos_value.y;
            }

            float scaleX() const {
                return node->getScaleX();
            }

            float scaleY() const {
                return node->getScaleY();
            }

            unsigned char opacity() const {
                return static_cast<CCSprite*>(node)->getOpacity();
            }

            void scale(float x, float y) {
                node->setScaleX(x);
                node->setScaleY(y);
            }

            void position(float x, float y) {
                node->setPosition({ x, y });
            }

            void opacity(unsigned char value) {
                static_cast<CCSprite*>(node)->setOpacity(value);
            }

            void remove() {
                native_removal(removal);
            }
        };

    public:
        ~DashAnimation() override {
            if (removal_target) {
                if (registered_draw)
                    echo::dash::untrack_draw(removal_target);
                removal_target->release();
            }
        }

        bool init(float x, float y, CCSprite* removal) {
            if (!CCActionInterval::initWithDuration(0.2f))
                return false;
            removal->retain();
            if (removal_target) {
                if (registered_draw)
                    echo::dash::untrack_draw(removal_target);
                removal_target->release();
            }
            registered_draw = false;
            removal_target = removal;
            end_x = x;
            end_y = y;
            return true;
        }

        void register_draw(CCSprite* outline) {
            echo::dash::track_draw(removal_target, outline);
            registered_draw = true;
        }

        void startWithTarget(CCNode* node) override {
            CCActionInterval::startWithTarget(node);
            Target target{ node, removal_target };
            timeline.start(target, end_x, end_y);
        }

        void update(float time) override {
            Target target{ m_pTarget, removal_target };
            timeline.update(time, target);
        }

        CCObject* copyWithZone(CCZone* zone) override {
            auto* copy = zone && zone->m_pCopyObject ? static_cast<DashAnimation*>(zone->m_pCopyObject) : new DashAnimation;
            if (!copy->init(end_x, end_y, removal_target)) {
                if (!zone || !zone->m_pCopyObject)
                    delete copy;
                return nullptr;
            }
            return copy;
        }

        CCActionInterval* reverse() override {
            return CCScaleTo::create(getDuration(), end_x, end_y)->reverse();
        }
    };

    void install_animation(CCSprite* root, CCSprite* outline) {
        float y = root->getScaleY() * 0.2f, x = root->getScaleX() * 0.2f;
        auto* animation = new DashAnimation;
        if (animation->init(x, y, root)) {
            if (echo_dash_batching_enabled)
                animation->register_draw(outline);
            animation->autorelease();
            root->runAction(animation);
        }
        else {
            delete animation;
            auto* removal = CCCallFunc::create(root, callfunc_selector(CCNode::removeMeAndCleanup));
            root->runAction(CCSequence::create(CCScaleTo::create(0.2f, x, y), removal, nullptr));
            root->runAction(CCMoveBy::create(0.2f, { -20.f, 0.f }));
            root->runAction(CCFadeTo::create(0.2f, 0));
        }
        outline->runAction(CCFadeTo::create(0.2f, 0));
        if (echo_dash_compare)
            echo::dash::pair_native(root, outline);
    }

    constexpr std::uintptr_t gate_site = 0x396780;
    // stopDashing builds its end effect in 0x39678e..0x396a4e, only when [player+0x7e4] is set & [player+0x7e1] is clear
    // its 2 sprites + their actions, writes nothing to the player
    // so skipping it = what gd does itself when [player+0x7e1] is set
    constexpr std::array<std::uint8_t, 8> gate_original{ 0x41, 0x80, 0xbe, 0xe1, 0x07, 0x00, 0x00, 0x00 }; // cmp byte [r14+0x7e1], 0
    constexpr std::size_t gate_mode = 0x40, gate_record = 0x48;
    volatile std::uint8_t* mode_byte = nullptr;
    Patch* effect_gate_patch = nullptr;
    Patch* effect_actions_patch = nullptr;

    // 1 page close to the game so the call & the mode byte stay rip relative
    std::uint8_t* allocate_near(std::uintptr_t target) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        std::uintptr_t const step = info.dwAllocationGranularity;
        std::uintptr_t const reach = 0x7ff00000; // a jmp can only go ~2gb
        auto try_at = [](std::uintptr_t address) {
            return static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        };
        std::uintptr_t floor = target > reach ? target - reach : step;
        std::uintptr_t address = (target & ~(step - 1)) - step;
        while (address >= floor) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
                break;
            if (region.State == MEM_FREE)
                if (auto* page = try_at(address))
                    return page;
            std::uintptr_t next = region.State == MEM_FREE ? address : reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            if (next < floor + step)
                break;
            address = (next - 1) & ~(step - 1);
        }
        address = (target + step) & ~(step - 1);
        while (address + 0x1000 < target + reach) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
                break;
            if (region.State == MEM_FREE)
                if (auto* page = try_at(address))
                    return page;
            auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
            address = std::max(address + step, (end + step - 1) & ~(step - 1));
        }
        return nullptr;
    }

    bool install_gate(std::uintptr_t module) {
        auto site = module + gate_site;
        if (std::memcmp(reinterpret_cast<void const*>(site), gate_original.data(), gate_original.size()) != 0)
            return false;
        auto* cave = allocate_near(site);
        if (!cave)
            return false;
        auto dist = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(cave)) - static_cast<std::int64_t>(site + 5);
        if (dist < INT32_MIN || dist > INT32_MAX) {
            VirtualFree(cave, 0, MEM_RELEASE);
            return false;
        }
        // robs compare goes first. the mode byte only matters if gd WOULD build the effect
        // bit 1 calls my recorder, bit 0 skips the block. the jne after only reads zf
        // stack stays aligned & the volatiles are dead at both targets
        constexpr std::array<std::uint8_t, 61> stub{
            0x41,
            0x80,
            0xbe,
            0xe1,
            0x07,
            0x00,
            0x00,
            0x00, //  0: cmp byte [r14+0x7e1], 0
            0x75,
            0x32, //  8: jne 60 (the game skips; ZF=0)
            0x0f,
            0xb6,
            0x05,
            0x2f,
            0x00,
            0x00,
            0x00, // 10: movzx eax, byte [mode]
            0x84,
            0xc0, // 17: test al, al
            0x74,
            0x27, // 19: je 60 (native; ZF=1)
            0xa8,
            0x02, // 21: test al, 2
            0x74,
            0x1f, // 23: je 56 (skip without recording)
            0x48,
            0x83,
            0xec,
            0x28, // 25: sub rsp, 0x28
            0x4c,
            0x89,
            0xf1, // 29: mov rcx, r14
            0xff,
            0x15,
            0x22,
            0x00,
            0x00,
            0x00, // 32: call [record]
            0x48,
            0x83,
            0xc4,
            0x28, // 38: add rsp, 0x28
            0x0f,
            0xb6,
            0x05,
            0x0f,
            0x00,
            0x00,
            0x00, // 42: movzx eax, byte [mode]
            0xa8,
            0x01, // 49: test al, 1
            0x75,
            0x03, // 51: jne 56
            0x31,
            0xc0, // 53: xor eax, eax (build; ZF=1)
            0xc3, // 55: ret
            0x48,
            0x85,
            0xe4, // 56: test rsp, rsp (skip; ZF=0)
            0xc3, // 59: ret
            0xc3, // 60: ret
        };
        static_assert(17 + 0x2f == gate_mode && 49 + 0x0f == gate_mode && 38 + 0x22 == gate_record);
        std::memcpy(cave, stub.data(), stub.size());
        cave[gate_mode] = 0;
        auto record = reinterpret_cast<std::uintptr_t>(&echo_record_dash_end);
        std::memcpy(cave + gate_record, &record, sizeof(record));
        FlushInstructionCache(GetCurrentProcess(), cave, 0x1000);
        std::vector<std::uint8_t> call{ 0xe8, 0, 0, 0, 0, 0x0f, 0x1f, 0x00 }; // call cave; nop
        auto rel = static_cast<std::int32_t>(dist);
        std::memcpy(call.data() + 1, &rel, sizeof(rel));
        auto res = Mod::get()->patch(reinterpret_cast<void*>(site), call);
        if (!res) {
            VirtualFree(cave, 0, MEM_RELEASE);
            return false;
        }
        mode_byte = cave + gate_mode;
        effect_gate_patch = res.unwrap();
        return true;
    }
}

namespace echo::dash {
    // another mod hooked stopDashing after me, put the bytes back & stay out of the way
    void prepare() {
        if (!effect_gate_patch && !effect_actions_patch)
            return;
        for (auto* mod : Loader::get()->getAllMods()) {
            if (mod == Mod::get())
                continue;
            for (auto* hook : mod->getHooks()) {
                if (!hook->isEnabled() || hook->getAddress() != geode::base::get() + 0x396650)
                    continue; // stopDashing
                for (auto** patch : { &effect_gate_patch, &effect_actions_patch }) {
                    if (!*patch)
                        continue;
                    if (auto res = (*patch)->disable(); res)
                        *patch = nullptr;
                }
                mode_byte = nullptr;
                return;
            }
        }
    }

    bool end_effect_skippable(PlayerObject* player) {
        if (!mode_byte || !player)
            return false;
        auto* bytes = reinterpret_cast<std::uint8_t const*>(player);
        return bytes[0x7e4] != 0 && bytes[0x7e1] == 0; // same 2 bytes stopDashing checks
    }

    void set_end_effect(EndEffect mode) {
        reset_pending_pair();
        if (mode_byte)
            *mode_byte = static_cast<std::uint8_t>(mode);
    }
}

$execute {
    using echo::circles::Signature;
    // hash skips the entry (thats where other mods hook), everything else is in it
    constexpr Signature stop_body{ 0x396670, 0x691, 0xc33c2694ed4a2084 };
    constexpr Signature actions{ 0x396987, 0xc7, 0xc266d6572a611a17 };
    auto module = geode::base::get();
    auto matches = [=](Signature signature) {
        return echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(module + signature.offset), signature.size) == signature.hash;
    };
    // both patches write inside stop_body so check it first
    if (!matches(stop_body)) {
        return;
    }
    (void)install_gate(module);
    if (!matches(actions)) {
        return;
    }
    constexpr std::array<Signature, 4> native_math{ {
        // { where, size, hash } of gd code
        { 0x36f20, 0x70, 0x299bd24e337466e1 }, // CCFadeTo::update
        { 0x370d0, 0xd0, 0x4e018c4e723c0e05 },
        { 0x373e0, 0x4f, 0x1dbc993295bc70b8 },
        { 0x37430, 0x114, 0x94c7bb6ce9a8c49e },
    } };
    auto cocos = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("libcocos2d.dll"));
    for (auto signature : native_math) {
        if (!cocos || echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(cocos + signature.offset), signature.size) != signature.hash) {
            return;
        }
    }
    native_removal = reinterpret_cast<Removal>(module + 0x3be70); // tf
    // the 6 actions gd starts on the 2 sprites become 1 call to install_animation
    // r15 = the effect, rsi = its outline. stack is aligned here
    // mov rcx, r15; mov rdx, rsi; mov rax, <fn>; call rax; jmp to the end of the block
    std::vector<std::uint8_t> patch(actions.size, 0x90);
    constexpr std::array<std::uint8_t, 8> prefix{ 0x49, 0x8b, 0xcf, 0x48, 0x8b, 0xd6, 0x48, 0xb8 };
    std::copy(prefix.begin(), prefix.end(), patch.begin());
    auto dst = reinterpret_cast<std::uintptr_t>(&install_animation);
    std::memcpy(patch.data() + 8, &dst, sizeof(dst));
    patch[16] = 0xff;
    patch[17] = 0xd0; // call rax
    patch[18] = 0xe9;
    std::int32_t displacement = static_cast<std::int32_t>(actions.size - 23);
    std::memcpy(patch.data() + 19, &displacement, sizeof(displacement));
    auto res = Mod::get()->patch(reinterpret_cast<void*>(module + actions.offset), patch);
    if (!res) {
        return;
    }
    effect_actions_patch = res.unwrap();
}
#endif
