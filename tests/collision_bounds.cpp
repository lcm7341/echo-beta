#include "Physics/collision_bounds.hpp"
#include "Rendering/circle_signatures.hpp"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <immintrin.h>
using namespace echo::collision_bounds;
using Bounds = std::uint64_t (*)(void const*, void const*);
std::vector<std::uint8_t> file(char const* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}
Bounds executable(std::vector<std::uint8_t> const& native, std::vector<std::uint8_t> const& getters, bool patched) {
    auto* memory = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    assert(memory != MAP_FAILED);
    std::memset(memory, 0x90, 4096);
    // SysV test entry supplies GD's exact stack rectangle and four live player bounds.
    std::uint8_t prefix[]{
        0x48,0x83,0xec,0x68, // sub rsp,68h (native getter shadow space + rectangle)
        0x0f,0x10,0x07, 0x0f,0x11,0x44,0x24,0x40, // movups xmm0,[rdi]; [rsp+40h]=xmm0
        0xf3,0x0f,0x10,0x3e, // xmm7=[rsi]
        0xf3,0x44,0x0f,0x10,0x46,0x04,
        0xf3,0x44,0x0f,0x10,0x4e,0x08,
        0xf3,0x44,0x0f,0x10,0x56,0x0c
    };
    std::memcpy(memory, prefix, sizeof(prefix));
    auto relative = [&](std::size_t operand, std::size_t next, std::size_t destination) {
        auto value = static_cast<std::int32_t>(destination) - static_cast<std::int32_t>(next);
        std::memcpy(memory + operand, &value, 4);
    };
    constexpr std::size_t block = 0x60, failed = 0x300, finish = 0x340;
    if (patched) {
        auto bytes = code(reinterpret_cast<std::uintptr_t>(memory + block),
                          reinterpret_cast<std::uintptr_t>(memory + failed));
        std::memcpy(memory + block, bytes.data(), bytes.size());
    } else {
        std::memcpy(memory + block, native.data(), native.size());
        unsigned calls[]{5,25,46,67}, branches[]{14,35,56,77}, lengths[]{5,10,6,11};
        std::size_t source = 0;
        for (unsigned i = 0; i < 4; ++i) {
            auto destination = memory + 0x500 + 32 * i;
            std::memcpy(destination, getters.data() + source, lengths[i]);
            source += lengths[i];
            std::memcpy(memory + 0x400 + 8 * i, &destination, 8);
            relative(block + calls[i] + 2, block + calls[i] + 6, 0x400 + 8 * i);
            relative(block + branches[i] + 2, block + branches[i] + 6, failed);
        }
    }
    std::uint8_t success[]{0xb8,1,0,0,0,0xe9,0,0,0,0};
    std::memcpy(memory + block + size, success, sizeof(success));
    relative(block + size + 6, block + size + 10, finish);
    std::uint8_t failure[]{0x31,0xc0,0xe9,0,0,0,0};
    std::memcpy(memory + failed, failure, sizeof(failure));
    relative(failed + 3, failed + 7, finish);
    // Return both decision and the exact final XMM0 bits (including NaN payload).
    std::uint8_t epilogue[]{0x66,0x0f,0x7e,0xc2,0x48,0xc1,0xe2,0x20,0x48,0x09,0xd0,
        0x48,0x83,0xc4,0x68,0xc3};
    std::memcpy(memory + finish, epilogue, sizeof(epilogue));
    assert(mprotect(memory, 4096, PROT_READ | PROT_EXEC) == 0);
    return reinterpret_cast<Bounds>(memory);
}
int main(int argc, char** argv) {
    assert(argc == 3);
    auto native = file(argv[1]), getters = file(argv[2]);
    assert(native.size() == size && getters.size() == 32);
    assert(echo::circles::fingerprint(native.data(), native.size()) == fingerprint);
    std::size_t offset = 0;
    auto checkGetter = [&](auto const& body) {
        assert(std::memcmp(getters.data() + offset, body.data(), body.size()) == 0);
        assert(getters[offset + body.size()] == 0xc3);
        offset += body.size() + 1;
    };
    checkGetter(minX); checkGetter(maxX); checkGetter(minY); checkGetter(maxY);
    auto reference = executable(native, getters, false), optimized = executable(native, getters, true);
    std::mt19937 rng(2081);
    auto saved = _mm_getcsr();
    unsigned checks = 0;
    std::uint32_t special[]{0,0x80000000,1,0x80000001,0x007fffff,0x00800000,
        0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0x7f812345,0xff812345};
    for (unsigned mode : {0x1f80u,0x9fc0u,0x3f80u,0x5f80u,0x7f80u}) {
        auto compare = [&](auto const& rect, auto const& bounds) {
            _mm_setcsr(mode); auto expected = reference(rect.data(), bounds.data()); auto nativeCsr = _mm_getcsr();
            _mm_setcsr(mode); auto actual = optimized(rect.data(), bounds.data()); auto patchedCsr = _mm_getcsr();
            assert(expected == actual && nativeCsr == patchedCsr); ++checks;
        };
        for (unsigned n = 0; n < 50000; ++n) {
            // Arbitrary IEEE bit patterns exercise signed zeros, NaNs and overflow.
            std::array<std::uint32_t, 4> rect, bounds;
            for (auto& value : rect) value = rng();
            for (auto& value : bounds) value = rng();
            if (n < 4096) for (unsigned i = 0; i < 4; ++i) {
                rect[i] = special[(n + i) % std::size(special)];
                bounds[i] = special[(n / 13 + i) % std::size(special)];
            }
            compare(rect, bounds);
            // Finite contacts, zero/negative extents, overlap and separated pairs.
            std::array<float, 4> finite{float(int(rng()%2000)-1000), float(int(rng()%2000)-1000),
                float(int(rng()%100)-20), float(int(rng()%100)-20)};
            std::array<float, 4> player{finite[0] + finite[2], finite[0],
                finite[1] + finite[3], finite[1]};
            if (n % 2) player[n % 4] += .00006103515625f;
            compare(finite, player);
        }
    }
    _mm_setcsr(saved);
    std::cout << checks << " native collision bounds comparisons passed (decision, XMM0 bits, MXCSR)\n";
    std::array<float,4> rect{0,0,100,100}, bounds{0,100,0,100};
    constexpr unsigned runs = 4000000;
    std::uint64_t sink = 0;
    auto measure = [&](Bounds function) {
        auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < runs; ++i) sink += function(rect.data(), bounds.data());
        return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    };
    auto baseline = measure(reference), replacement = measure(optimized);
    std::cout << "Isolated overlapping bounds: native " << baseline << "s, inline " << replacement
              << "s, ratio " << baseline/replacement << " (not a replay speedup); sink " << sink << '\n';
}
