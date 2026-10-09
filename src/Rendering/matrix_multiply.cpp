#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include "circle_signatures.hpp"
#include "matrix_multiply.hpp"
#include <array>

using namespace geode::prelude;
namespace {
using Multiply = float* (*)(float*, float const*, float const*);
Multiply originalMultiply = nullptr;
float* multiply(float* out, float const* a, float const* b) {
    if (!echo::matrix::supported(a,b)) return originalMultiply(out,a,b);
    return echo::matrix::multiply(out,a,b);
}
}
$execute {
    // This exact routine is self-contained: no calls or RIP-relative operands.
    // Retain its native instruction body for NaN/Inf and unmasked FP exceptions.
    constexpr echo::circles::Signature signature{0xb81e0,0x555,0xf789707004861682};
    auto cocos=reinterpret_cast<std::uintptr_t>(GetModuleHandleA("libcocos2d.dll"));
    auto* body=reinterpret_cast<std::uint8_t*>(cocos+signature.offset);
    if (!cocos || echo::circles::fingerprint(body,signature.size)!=signature.hash) {
        log::warn("Echo matrix multiply instructions differ; SIMD optimization skipped");
        return;
    }
    auto* fallback=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if (!fallback) return;
    std::memcpy(fallback,body,signature.size);
    DWORD protection;
    if (!VirtualProtect(fallback,4096,PAGE_EXECUTE_READ,&protection)) {
        VirtualFree(fallback,0,MEM_RELEASE);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(),fallback,signature.size);
    originalMultiply=reinterpret_cast<Multiply>(fallback);
    std::array<std::uint8_t,12> jump{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
    auto destination=reinterpret_cast<std::uintptr_t>(&multiply);
    std::memcpy(jump.data()+2,&destination,sizeof(destination));
    auto patch=Mod::get()->patch(body,std::vector<std::uint8_t>(jump.begin(),jump.end()));
    if (!patch) {
        originalMultiply=nullptr;
        VirtualFree(fallback,0,MEM_RELEASE);
        log::warn("Echo matrix multiply patch failed: {}",patch.unwrapErr());
        return;
    }
    // The native fallback's executable page remains valid until process exit.
    log::info("Echo native-order SIMD matrix multiply enabled (native exceptional-input fallback)");
}
#endif
