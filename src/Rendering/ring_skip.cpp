#include <Geode/Geode.hpp>
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

// every orb press builds a ring in ringJump (0x399b62..0x399ca3)
// presses on the same orb in 1 frame build the same ring & the merge deletes it right after
// so the patch asks me first & a duplicate jumps past the block
namespace {
    float startRadius = 0.f, start_radius_alt = 0.f, endRadius = 0.f, duration = 0.f;

    // a free page within rel32 range of the patch, for the cave
    std::uint8_t* allocate_near(std::uintptr_t target) {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        std::uintptr_t const step = info.dwAllocationGranularity;
        std::uintptr_t const reach = 0x7ff00000; // a jmp can only go ~2gb
        std::uintptr_t floor = target > reach ? target - reach : step;
        std::uintptr_t address = (target & ~(step - 1)) - step;
        while (address >= floor) {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)))
                break;
            if (region.State == MEM_FREE) {
                if (auto* page = static_cast<std::uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(address), 0x1000,
                        MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)))
                    return page;
            }
            std::uintptr_t next = region.State == MEM_FREE ? address : reinterpret_cast<std::uintptr_t>(region.AllocationBase);
            if (next < floor + step)
                break;
            address = (next - 1) & ~(step - 1);
        }
        return nullptr;
    }
}

// called from the cave. player is rdi, orb is rsi, color is bx (r, g) + r14b (b), packed r | g << 8 | b << 16
extern "C" bool echo_ring_duplicate(PlayerObject* player, GameObject* orb, std::uint32_t color) noexcept {
    try {
        if (!player || !orb)
            return false;
        auto* manager = GameManager::get();
        auto* play = manager ? *reinterpret_cast<std::uint8_t**>(reinterpret_cast<std::uint8_t*>(manager) + 0x208) : nullptr; // GameManager -> play layer
        if (!play)
            return false;
        auto* parent = *reinterpret_cast<CCNode**>(reinterpret_cast<std::uint8_t*>(player) + 0xc28); // layer the rings go in
        if (!parent)
            return false;
        // orb type (virtual at 0x660) picks the start radius like the block does
        using TypeOf = int (*)(void*);
        auto type = (*reinterpret_cast<TypeOf**>(orb))[0x660 / 8](orb);
        float from = type == 0x23 ? start_radius_alt : startRadius;
        ccColor3B rgb{ static_cast<GLubyte>(color), static_cast<GLubyte>(color >> 8), static_cast<GLubyte>(color >> 16) };
        return echo::waves::count_duplicate(parent, orb->getPosition(), rgb, from, endRadius, duration, true, true, play + 0x37a0);
    }
    catch (...) {
    }
    return false;
}

$execute {
    auto module = geode::base::get();
    auto site = module + 0x399b62; // in ringJump
    // gd code hash
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(site), 0x141) != 0x853bc52d99ea83c8ull) {
        return;
    }
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get())
            continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && hook->getAddress() == module + 0x398c00) { // ringJump
                return;
            }
    }
    startRadius = *reinterpret_cast<float const*>(module + 0x623198); // 35.0f
    start_radius_alt = *reinterpret_cast<float const*>(module + 0x6231b8);
    endRadius = *reinterpret_cast<float const*>(module + 0x623010);
    duration = *reinterpret_cast<float const*>(module + 0x622abc);
    auto* cave = allocate_near(site);
    if (!cave)
        return;
    auto dist = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(cave)) - static_cast<std::int64_t>(site + 5);
    if (dist < INT32_MIN || dist > INT32_MAX) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    // got here by a jmp inside the frame so alignment & shadow space are fine for the call
    // volatile registers are dead here. rdi, rsi, bx & r14 are nonvolatile
    std::vector<std::uint8_t> code;
    auto put = [&](std::initializer_list<std::uint8_t> list) {
        code.insert(code.end(), list);
    };
    auto imm64 = [&](std::uint64_t value) {
        for (int i = 0; i < 8; ++i)
            code.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    put({ 0x48, 0x89, 0xf9 });       // mov rcx, rdi
    put({ 0x48, 0x89, 0xf2 });       // mov rdx, rsi
    put({ 0x44, 0x0f, 0xb7, 0xc3 }); // movzx r8d, bx
    put({ 0x41, 0x0f, 0xb6, 0xc6 }); // movzx eax, r14b
    put({ 0xc1, 0xe0, 0x10 });       // shl eax, 16
    put({ 0x41, 0x09, 0xc0 });       // or r8d, eax
    put({ 0x48, 0xb8 });
    imm64(reinterpret_cast<std::uintptr_t>(&echo_ring_duplicate)); // mov rax, fn
    put({ 0xff, 0xd0 });                                           // call rax
    put({ 0x84, 0xc0 });                                           // test al, al
    put({ 0x75, 0x1b });                                           // jne skip (+27)
    put({ 0x48, 0xb8 });
    imm64(module + 0x623198);        // mov rax, &35.0f
    put({ 0xf3, 0x0f, 0x10, 0x30 }); // movss xmm6, [rax]   (the replaced instruction)
    put({ 0x49, 0xbb });
    imm64(site + 8);           // mov r11, next instruction
    put({ 0x41, 0xff, 0xe3 }); // jmp r11
    put({ 0x49, 0xbb });
    imm64(module + 0x399ca3);  // skip: mov r11, after the block
    put({ 0x41, 0xff, 0xe3 }); // jmp r11
    std::memcpy(cave, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), cave, code.size());
    // jmp cave; 3 nops
    std::vector<std::uint8_t> patch{ 0xe9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    auto rel = static_cast<std::int32_t>(dist);
    std::memcpy(patch.data() + 1, &rel, sizeof(rel));
    if (auto res = Mod::get()->patch(reinterpret_cast<void*>(site), patch); !res) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
}
