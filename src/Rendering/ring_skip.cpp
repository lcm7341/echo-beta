#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "circle_signatures.hpp"
#include "wave_merge.hpp"
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace geode::prelude;

// Every orb press builds a ring in ringJump (0x399b62..0x399ca3): CCCircleWave::create, color,
// position, follow the orb, PlayLayer's delegate and ring array, addChild. Presses on the same orb
// in one frame build identical rings, which the ring merge then retires. A patch at the block's
// first instruction asks Echo first; a duplicate jumps past the block (it only builds that ring;
// GD itself jumps to the same place when it builds none) and counts toward the existing ring.
namespace {
float startRadius = 0.f, startRadiusAlt = 0.f, endRadius = 0.f, duration = 0.f;

std::uint8_t* allocateNear(std::uintptr_t target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    std::uintptr_t const step = info.dwAllocationGranularity;
    std::uintptr_t const reach = 0x7ff00000;
    std::uintptr_t floor = target > reach ? target - reach : step;
    std::uintptr_t address = (target & ~(step - 1)) - step;
    while (address >= floor) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region))) break;
        if (region.State == MEM_FREE) {
            if (auto* page = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), 0x1000,
                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))) return page;
        }
        std::uintptr_t next = region.State == MEM_FREE ? address : reinterpret_cast<std::uintptr_t>(region.AllocationBase);
        if (next < floor + step) break;
        address = (next - 1) & ~(step - 1);
    }
    return nullptr;
}
}

// player: rdi, orb: rsi, color: bx (r, g) and r14b (b), packed r | g << 8 | b << 16.
extern "C" bool echoRingDuplicate(PlayerObject* player, GameObject* orb, std::uint32_t color) noexcept {
    try {
        if (!player || !orb) return false;
        auto* manager = GameManager::get();
        auto* play = manager ? *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(manager) + 0x208) : nullptr;
        if (!play) return false;
        auto* parent = *reinterpret_cast<CCNode**>(reinterpret_cast<std::uint8_t*>(player) + 0xc28);
        if (!parent) return false;
        // The block's own choice: the orb type virtual at 0x660 picks the start radius.
        using TypeOf = int (*)(void*);
        auto type = (*reinterpret_cast<TypeOf**>(orb))[0x660 / 8](orb);
        float from = type == 0x23 ? startRadiusAlt : startRadius;
        ccColor3B rgb{static_cast<GLubyte>(color), static_cast<GLubyte>(color >> 8), static_cast<GLubyte>(color >> 16)};
        return echo::waves::countDuplicate(parent, orb->getPosition(), rgb, from, endRadius, duration, true, true, play + 0x37a0);
    } catch (...) {
    }
    return false;
}

$execute {
    auto module = geode::base::get();
    auto site = module + 0x399b62;
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(site), 0x141) != 0x853bc52d99ea83c8ull) {
        log::info("Echo ring skip not installed: native code differs");
        return;
    }
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && hook->getAddress() == module + 0x398c00) {
                log::info("Echo ring skip not installed: {} hooks ringJump", mod->getID());
                return;
            }
    }
    startRadius = *reinterpret_cast<float const*>(module + 0x623198);
    startRadiusAlt = *reinterpret_cast<float const*>(module + 0x6231b8);
    endRadius = *reinterpret_cast<float const*>(module + 0x623010);
    duration = *reinterpret_cast<float const*>(module + 0x622abc);
    auto* cave = allocateNear(site);
    if (!cave) return;
    auto distance = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(cave)) - static_cast<std::int64_t>(site + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) { VirtualFree(cave, 0, MEM_RELEASE); return; }
    // Reached by jmp at an instruction boundary inside the frame, so alignment and shadow space
    // hold for the call. Volatile registers are dead here (the block reloads rax and rcx and
    // passes only xmm0-2, r9 and the stack to create); rdi, rsi, bx and r14 are nonvolatile.
    std::vector<std::uint8_t> code;
    auto put = [&](std::initializer_list<std::uint8_t> list) { code.insert(code.end(), list); };
    auto imm64 = [&](std::uint64_t value) { for (int i = 0; i < 8; ++i) code.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
    put({0x48, 0x89, 0xf9});                  // mov rcx, rdi
    put({0x48, 0x89, 0xf2});                  // mov rdx, rsi
    put({0x44, 0x0f, 0xb7, 0xc3});            // movzx r8d, bx
    put({0x41, 0x0f, 0xb6, 0xc6});            // movzx eax, r14b
    put({0xc1, 0xe0, 0x10});                  // shl eax, 16
    put({0x41, 0x09, 0xc0});                  // or r8d, eax
    put({0x48, 0xb8}); imm64(reinterpret_cast<std::uintptr_t>(&echoRingDuplicate)); // mov rax, fn
    put({0xff, 0xd0});                        // call rax
    put({0x84, 0xc0});                        // test al, al
    put({0x75, 0x1b});                        // jne skip (+27)
    put({0x48, 0xb8}); imm64(module + 0x623198);    // mov rax, &35.0f
    put({0xf3, 0x0f, 0x10, 0x30});            // movss xmm6, [rax]   (the replaced instruction)
    put({0x49, 0xbb}); imm64(site + 8);       // mov r11, next instruction
    put({0x41, 0xff, 0xe3});                  // jmp r11
    put({0x49, 0xbb}); imm64(module + 0x399ca3); // skip: mov r11, after the block
    put({0x41, 0xff, 0xe3});                  // jmp r11
    std::memcpy(cave, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), cave, code.size());
    std::vector<std::uint8_t> patch{0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    auto rel = static_cast<std::int32_t>(distance);
    std::memcpy(patch.data() + 1, &rel, sizeof(rel));
    if (auto result = Mod::get()->patch(reinterpret_cast<void*>(site), patch); !result) {
        log::warn("Echo ring skip patch failed: {}", result.unwrapErr());
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    log::info("Echo ring skip installed");
}
#endif
