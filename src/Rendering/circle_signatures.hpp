#pragma once
#include <cstddef>
#include <cstdint>
namespace echo::circles {
struct Signature { uintptr_t offset; size_t size; uint64_t hash; };
inline constexpr Signature armCircle{0xad83a0, 0x1b4, 0x50ddf1831a147d30};
inline constexpr Signature armFilled{0xad8714, 0x178, 0xa232138299aa65c8};
inline constexpr Signature winCircle{0x10a40, 0x230, 0x2d79db021810c7d7};
inline constexpr Signature winFilled{0x11230, 0x1be, 0xb10e03260604fd86};
inline uint64_t fingerprint(uint8_t const* bytes, size_t size) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
}
