#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include "Rendering/circle_signatures.hpp"
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace geode::prelude;

// checkCollisions walks two object arrays it builds every step (layer+0x31a0 backwards, then
// layer+0x31c0 forwards) and first reads each object's 0x28e and 0x368 flags. The objects are
// scattered over the heap, so most of that loop waits on memory. Each loop head now also asks
// for those two cache lines of the object eight entries ahead. A prefetch changes no register
// the loop reads, no flag it tests and no memory, so the loop computes exactly what it did.
namespace {
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

struct Code {
    std::vector<std::uint8_t> bytes;
    void put(std::initializer_list<std::uint8_t> list) { bytes.insert(bytes.end(), list); }
    void rel32(std::uintptr_t at, std::uintptr_t target) {
        auto value = static_cast<std::int32_t>(static_cast<std::int64_t>(target) - static_cast<std::int64_t>(at + bytes.size() + 4));
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
};

// The lines checkCollisions' loops read first: the vtable (a dirty rect calls through it),
// [r11+0x28e] and [r11+0x368]. 20 bytes.
void prefetchR11(Code& code) {
    code.put({0x41, 0x0f, 0x18, 0x0b});                         // prefetcht0 [r11]
    code.put({0x41, 0x0f, 0x18, 0x8b, 0x8e, 0x02, 0x00, 0x00}); // prefetcht0 [r11+0x28e]
    code.put({0x41, 0x0f, 0x18, 0x8b, 0x68, 0x03, 0x00, 0x00}); // prefetcht0 [r11+0x368]
}

bool redirect(std::uintptr_t site, std::size_t length, std::uint8_t* cave) {
    auto distance = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(cave)) - static_cast<std::int64_t>(site + 5);
    if (distance < INT32_MIN || distance > INT32_MAX) return false;
    std::vector<std::uint8_t> patch(length, 0x90);
    patch[0] = 0xe9;
    auto rel = static_cast<std::int32_t>(distance);
    std::memcpy(patch.data() + 1, &rel, 4);
    return static_cast<bool>(Mod::get()->patch(reinterpret_cast<void*>(site), patch));
}
}

$execute {
    auto module = geode::base::get();
    auto first = module + 0x2144f0, second = module + 0x2147c0;
    // Both loop heads with the flag tests that follow them.
    if (echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(first), 0x34) != 0xbf76d5ce2f997f56ull ||
        echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(second), 0x2d) != 0x7f5905ad4ffbcb89ull) {
        log::info("Echo collision prefetch skipped: native code differs");
        return;
    }
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        for (auto* hook : mod->getHooks())
            if (hook->isEnabled() && hook->getAddress() == module + 0x2137f0) {
                log::info("Echo collision prefetch skipped: {} hooks checkCollisions", mod->getID());
                return;
            }
    }
    auto* cave = allocateNear(first);
    if (!cave) return;
    auto base = reinterpret_cast<std::uintptr_t>(cave);

    // Loop 1 (backwards). Original: sub ecx,r12d; movsxd rcx,ecx; mov rax,[r14+31a0h];
    // mov rbx,[rax+rcx*8-8]. Entry rcx-9 is eight ahead in walking order.
    Code one;
    one.put({0x41, 0x2b, 0xcc});                          // sub ecx, r12d
    one.put({0x48, 0x63, 0xc9});                          // movsxd rcx, ecx
    one.put({0x49, 0x8b, 0x86, 0xa0, 0x31, 0x00, 0x00});  // mov rax, [r14+31a0h]
    one.put({0x48, 0x8b, 0x5c, 0xc8, 0xf8});              // mov rbx, [rax+rcx*8-8]
    one.put({0x48, 0x83, 0xf9, 0x09});                    // cmp rcx, 9
    one.put({0x7c, 0x19});                                // jl done (+25)
    one.put({0x4c, 0x8b, 0x5c, 0xc8, 0xb8});              // mov r11, [rax+rcx*8-48h]
    prefetchR11(one);
    one.put({0xe9}); one.rel32(base, first + 0x12);       // done: jmp 1402144f02h+
    // Loop 2 (forwards). Original: mov rax,[r14+31c0h]; mov rbx,[r15+rax]. r12 is the index
    // and [r14+31b8h] the count; r15 is the byte offset.
    std::uintptr_t secondBase = base + 0x80;
    Code two;
    two.put({0x49, 0x8b, 0x86, 0xc0, 0x31, 0x00, 0x00});  // mov rax, [r14+31c0h]
    two.put({0x49, 0x8b, 0x1c, 0x07});                    // mov rbx, [r15+rax]
    two.put({0x45, 0x8d, 0x5c, 0x24, 0x08});              // lea r11d, [r12+8]
    two.put({0x45, 0x3b, 0x9e, 0xb8, 0x31, 0x00, 0x00});  // cmp r11d, [r14+31b8h]
    two.put({0x7d, 0x19});                                // jge done (+25)
    two.put({0x4d, 0x8b, 0x5c, 0x07, 0x40});              // mov r11, [r15+rax+40h]
    prefetchR11(two);
    two.put({0xe9}); two.rel32(secondBase, second + 0x0b);
    // collisionCheckObjects (0x214a20), forwards over the candidates vector: r12 is the index,
    // [rsp+70h] the count, rdi the vector (reloaded before every pass). It reads 0x3a0 (type),
    // 0x28e and 0x3d2 first. Original: mov rax,[rdi]; mov rdi,[rax+r12*8].
    auto third = module + 0x214a20;
    bool candidates = echo::circles::fingerprint(reinterpret_cast<std::uint8_t const*>(third), 0x40) == 0x59030fd621f26c71ull;
    std::uintptr_t thirdBase = base + 0x100;
    Code three;
    three.put({0x48, 0x8b, 0x07});                          // mov rax, [rdi]
    three.put({0x4d, 0x8d, 0x5c, 0x24, 0x08});              // lea r11, [r12+8]
    three.put({0x4c, 0x3b, 0x5c, 0x24, 0x70});              // cmp r11, [rsp+70h]
    three.put({0x7d, 0x1c});                                // jge done (+28)
    three.put({0x4e, 0x8b, 0x1c, 0xd8});                    // mov r11, [rax+r11*8]
    three.put({0x41, 0x0f, 0x18, 0x8b, 0xa0, 0x03, 0x00, 0x00}); // prefetcht0 [r11+0x3a0]
    three.put({0x41, 0x0f, 0x18, 0x8b, 0x8e, 0x02, 0x00, 0x00}); // prefetcht0 [r11+0x28e]
    three.put({0x41, 0x0f, 0x18, 0x8b, 0xd2, 0x03, 0x00, 0x00}); // prefetcht0 [r11+0x3d2]
    three.put({0x4a, 0x8b, 0x3c, 0xe0});                    // done: mov rdi, [rax+r12*8]
    three.put({0xe9}); three.rel32(thirdBase, third + 7);
    if (one.bytes.size() > 0x80 || two.bytes.size() > 0x80 || three.bytes.size() > 0x80) return;
    std::memcpy(cave, one.bytes.data(), one.bytes.size());
    std::memcpy(cave + 0x80, two.bytes.data(), two.bytes.size());
    std::memcpy(cave + 0x100, three.bytes.data(), three.bytes.size());
    FlushInstructionCache(GetCurrentProcess(), cave, 0x180);
    if (!redirect(first, 0x12, cave)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        log::warn("Echo collision prefetch patch failed");
        return;
    }
    if (!redirect(second, 0x0b, cave + 0x80)) {
        log::warn("Echo collision prefetch: second loop patch failed; first loop prefetches only");
        return;
    }
    if (candidates && !redirect(third, 7, cave + 0x100)) log::warn("Echo collision prefetch: candidate loop patch failed");
    log::info("Echo collision prefetch installed{}", candidates ? "" : " (candidate loop skipped: native code differs)");
}
#endif
