#pragma once
#include <cstddef>
#include <cstdint>

namespace echo::circles {
	// i hash gds code & compare it to the hash from the real 2.2081
	// no match = different version or another mod changed it so i dont touch it
	// { where in the exe, how many bytes, hash of those bytes }
	struct Signature {
		uintptr_t offset;
		size_t size;
		uint64_t hash;
	};

	// { where, size, hash } of gd code
	inline constexpr Signature arm_circle{ 0xad83a0, 0x1b4, 0x50ddf1831a147d30 };
	inline constexpr Signature arm_filled{ 0xad8714, 0x178, 0xa232138299aa65c8 };
	inline constexpr Signature win_circle{ 0x10a40, 0x230, 0x2d79db021810c7d7 };
	inline constexpr Signature win_filled{ 0x11230, 0x1be, 0xb10e03260604fd86 };

	inline uint64_t fingerprint(uint8_t const* bytes, size_t size) {
		uint64_t hash = 14695981039346656037ull; // fnv-1a start value
		for (size_t i = 0; i < size; ++i)
			hash = (hash ^ bytes[i]) * 1099511628211ull; // fnv-1a prime
		return hash;
	}
}
