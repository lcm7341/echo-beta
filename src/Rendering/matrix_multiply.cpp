// replaces cocos's 4x4 matrix multiply w/ an sse one. windows
// cocos runs it for every node transform & its plain scalar code. w/ thousands of sprites on screen at high tps it adds up
// how: the original functions body is copied to a new page first (it has no calls & nothing rip relative, so the copy just runs),
//      then the original is overwritten w/ a jmp to multiply()
// multiply() uses the sse version unless a matrix has a nan / inf in it or a float exception is unmasked. those go to the copy
//      of the original so they come out exactly like vanilla
// only done if the bytes hash to what 2.2081's libcocos2d has
#include <Geode/Geode.hpp>
#ifdef GEODE_IS_WINDOWS
#include "circle_signatures.hpp"
#include "matrix_multiply.hpp"
#include "Hooks/opt.hpp"
#include <array>

using namespace geode::prelude;

namespace {
    using Multiply = float* (*)(float*, float const*, float const*);
    Multiply original_multiply = nullptr;

    // anything weird (nan, inf, ...) goes to the copy of the original
    float* multiply(float* out, float const* a, float const* b) {
        if (!echo::opt::matrix || !echo::matrix::supported(a, b))
            return original_multiply(out, a, b);
        return echo::matrix::multiply(out, a, b);
    }
}

$execute {
    // cocos's 4x4 multiply. no calls & nothing rip relative so the whole body can be copied & still run
    // the copy is the fallback so nan/inf act like native
    constexpr echo::circles::Signature signature{ 0xb81e0, 0x555, 0xf789707004861682 };
    auto cocos = reinterpret_cast<std::uintptr_t>(GetModuleHandleA("libcocos2d.dll"));
    auto* body = reinterpret_cast<std::uint8_t*>(cocos + signature.offset);
    if (!cocos || echo::circles::fingerprint(body, signature.size) != signature.hash) {
        return;
    }
    auto* fallback = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!fallback)
        return;
    std::memcpy(fallback, body, signature.size);
    DWORD protection;
    if (!VirtualProtect(fallback, 4096, PAGE_EXECUTE_READ, &protection)) {
        VirtualFree(fallback, 0, MEM_RELEASE);
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), fallback, signature.size);
    original_multiply = reinterpret_cast<Multiply>(fallback);
    // mov rax, <fn>; jmp rax
    std::array<std::uint8_t, 12> jump{ 0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0 };
    auto dst = reinterpret_cast<std::uintptr_t>(&multiply);
    std::memcpy(jump.data() + 2, &dst, sizeof(dst));
    auto patch = Mod::get()->patch(body, std::vector<std::uint8_t>(jump.begin(), jump.end()));
    if (!patch) {
        original_multiply = nullptr;
        VirtualFree(fallback, 0, MEM_RELEASE);
        return;
    }
    // the fallback page is never freed, on purpose
}
#endif
