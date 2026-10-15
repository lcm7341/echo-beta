#include <Geode/Geode.hpp>
#ifdef GEODE_IS_MACOS
#include "Logic/logic.hpp"
#include "mac_random.hpp"
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <libkern/OSCacheControl.h>
#include <algorithm>
#include <cstring>
#include <limits>

using namespace geode::prelude;

namespace {
    int shake_random() {
        auto& state = Logic::get().shake_random;
        state = ((214013ull * state + 2531011ull) >> 16) & 0x7fff; // msvc rand()
        float unit = static_cast<float>(state) / 32767.f;
        auto scaled = static_cast<uint64_t>(static_cast<double>(unit) * 2147483648.0);
        return static_cast<int>(std::min(scaled, uint64_t{ 0x7fffffff }));
    }

    mach_vm_address_t near_veneer(uintptr_t base) {
        for (uintptr_t delta = 0x1000000; delta < 0x7f00000; delta += 0x10000) {
            mach_vm_address_t address = (base + delta) & ~uintptr_t{ 0xfff };
            if (mach_vm_allocate(mach_task_self(), &address, 4096, VM_FLAGS_FIXED) != KERN_SUCCESS)
                continue;
#ifdef GEODE_IS_ARM_MAC
            std::array<uint8_t, 16> bytes{ 0x50, 0, 0, 0x58, 0, 0x02, 0x1f, 0xd6 }; // ldr x16,#8; br x16
            constexpr size_t pointer_offset = 8;
#else
            std::array<uint8_t, 13> bytes{ 0x49, 0xbb, 0, 0, 0, 0, 0, 0, 0, 0, 0x41, 0xff, 0xe3 }; // movabs r11; jmp r11
            constexpr size_t pointer_offset = 2;
#endif
            auto dst = reinterpret_cast<uintptr_t>(&shake_random);
            std::memcpy(bytes.data() + pointer_offset, &dst, sizeof(dst));
            std::memcpy(reinterpret_cast<void*>(address), bytes.data(), bytes.size());
            if (mach_vm_protect(mach_task_self(), address, 4096, false, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) {
                (void)mach_vm_deallocate(mach_task_self(), address, 4096);
                return 0;
            }
            sys_icache_invalidate(reinterpret_cast<void*>(address), bytes.size());
            return address;
        }
        return 0;
    }
}

$execute {
    namespace native = echo::patches::mac;
    auto base = geode::base::get();
    bool matches = std::all_of(native::shake_sites.begin(), native::shake_sites.end(), [base](auto const& site) {
        return std::memcmp(reinterpret_cast<void*>(base + site.offset), site.original.data(), site.original.size()) == 0;
    });
    if (!matches)
        return;
    if (auto veneer = near_veneer(base)) {
        std::vector<Patch*> installed;
        bool success = true;
        for (auto const& site : native::shake_sites) {
#ifdef GEODE_IS_ARM_MAC
            auto displacement = static_cast<int64_t>(veneer) - static_cast<int64_t>(base + site.offset);
            uint32_t call = 0x94000000 | (static_cast<uint32_t>(displacement / 4) & 0x03ffffff); // arm64 bl
            std::vector<uint8_t> bytes(4);
            std::memcpy(bytes.data(), &call, sizeof(call));
#else
            // call veneer
            int32_t displacement = static_cast<int32_t>(veneer - (base + site.offset + 5));
            std::vector<uint8_t> bytes{ 0xe8, 0, 0, 0, 0 };
            std::memcpy(bytes.data() + 1, &displacement, sizeof(displacement));
#endif
            auto patch = Mod::get()->patch(reinterpret_cast<void*>(base + site.offset), bytes);
            if (!patch) {
                success = false;
                break;
            }
            installed.push_back(patch.unwrap());
        }
        if (!success) {
            for (auto* patch : installed)
                (void)patch->disable();
        }
    }
}
#endif
