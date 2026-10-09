#include "Physics/obb_math.hpp"
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <vector>
#include <sys/mman.h>
#include <immintrin.h>

using Native = void (__attribute__((ms_abi)) *)(void*, echo::obb::Point*, float, float, float);
float __attribute__((ms_abi)) stubSin(float x) { return std::sin(x); }
float __attribute__((ms_abi)) stubCos(float x) { return std::cos(x); }
void __attribute__((ms_abi)) stubHelper(void*) {}
// Opaque to the optimizer, as GD's imported sinf/cosf are: no inlining, no merging into sincosf.
__attribute__((noinline)) float hostSin(float x) { return std::sin(x); }
__attribute__((noinline)) float hostCos(float x) { return std::cos(x); }

int main(int argc, char** argv) {
    assert(argc == 2);
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> blob{std::istreambuf_iterator<char>(input), {}};
    assert(blob.size() == 0x1000);
    auto* memory = static_cast<std::uint8_t*>(mmap(reinterpret_cast<void*>(SANDBOX), 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
    assert(memory == reinterpret_cast<std::uint8_t*>(SANDBOX));
    std::memcpy(memory, blob.data(), blob.size());
    auto slot = [&](std::size_t offset, auto function) { auto value = reinterpret_cast<std::uintptr_t>(function); std::memcpy(memory + offset, &value, 8); };
    slot(SIN_SLOT, &stubSin); slot(COS_SLOT, &stubCos); slot(HELPER_SLOT, &stubHelper);
    auto native = reinterpret_cast<Native>(memory);

    std::mt19937 rng(2081);
    std::uniform_real_distribution<float> position(-100000.f, 100000.f), size(0.f, 400.f), angle(-1000.f, 1000.f);
    std::array<float, 10> special{0.f, -0.f, 1e-38f, 1e30f, INFINITY, -INFINITY, NAN, 0.5f, 90.f, 3.14159265f};
    auto saved = _mm_getcsr();
    unsigned checks = 0;
    for (unsigned mode : {0x1f80u, 0x9fc0u, 0x3f80u, 0x5f80u, 0x7f80u}) {
        for (int n = 0; n < 40000; ++n) {
            echo::obb::Point center{position(rng), position(rng)};
            float width = size(rng), height = size(rng), theta = angle(rng);
            if (n % 17 == 0) width = special[n % special.size()];
            if (n % 19 == 0) height = special[(n / 19) % special.size()];
            if (n % 23 == 0) theta = special[(n / 23) % special.size()];
            if (n % 29 == 0) center.x = special[(n / 29) % special.size()];
            alignas(16) std::array<std::uint8_t, 0x200> expected{}, actual{};
            expected.fill(0xa5); actual.fill(0xa5);
            echo::obb::Point copy = center;
            _mm_setcsr(mode); native(expected.data(), &copy, width, height, theta); auto nativeCsr = _mm_getcsr();
            _mm_setcsr(mode); echo::obb::Trig volatile cosine = &hostCos, sine = &hostSin;
            echo::obb::calculate(actual.data(), center, width, height, theta, cosine, sine, 0.5f, 1.0f);
            auto oursCsr = _mm_getcsr();
            assert(nativeCsr == oursCsr);
            assert(std::memcmp(expected.data(), actual.data(), expected.size()) == 0);
            ++checks;
        }
    }
    _mm_setcsr(saved);
    std::cout << checks << " native OBB2D::calculateWithCenter comparisons passed (every byte and MXCSR flag, five modes)\n";
}
