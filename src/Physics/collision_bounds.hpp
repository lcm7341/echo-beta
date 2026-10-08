#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace echo::collision_bounds {
	void prepare();
	inline constexpr std::uintptr_t site = 0x214b09;
	inline constexpr std::uintptr_t miss = 0x215a18;
	inline constexpr std::size_t size = 0x53;
	inline constexpr std::uint64_t fingerprint = 0x8bffb4e3bfa25599ull; // gd code hash
	// cocos 2.2081 rect getters without the ret. keep the sse order (width + x, then height + y)
	// swapping them changes how nans come out
	inline constexpr std::array<std::uint8_t, 9> maxX{ 0xf3, 0x0f, 0x10, 0x41, 0x08, 0xf3, 0x0f, 0x58, 0x01 };
	inline constexpr std::array<std::uint8_t, 4> minX{ 0xf3, 0x0f, 0x10, 0x01 };
	inline constexpr std::array<std::uint8_t, 10> maxY{ 0xf3, 0x0f, 0x10, 0x41, 0x0c, 0xf3, 0x0f, 0x58, 0x41, 0x04 };
	inline constexpr std::array<std::uint8_t, 5> minY{ 0xf3, 0x0f, 0x10, 0x41, 0x04 };

	// only the getter calls get inlined, robs 4 comiss/ja compares stay
	// rcx & xmm0 end up the same as native
	inline std::array<std::uint8_t, size> code(std::uintptr_t address, std::uintptr_t rejected) {
		std::array<std::uint8_t, size> result;
		result.fill(0x90);
		std::size_t used = 0;
		auto emit = [&](auto const& bytes) {
			for (auto byte : bytes)
				result[used++] = byte;
		};
		auto branch = [&] {
			emit(std::array<std::uint8_t, 2>{ 0x0f, 0x87 }); // ja rejected
			auto distance = static_cast<std::int32_t>(rejected - (address + used + 4));
			std::memcpy(result.data() + used, &distance, 4);
			used += 4;
		};
		emit(std::array<std::uint8_t, 5>{ 0x48, 0x8d, 0x4c, 0x24, 0x40 }); // lea rcx,[rsp+40h]
		// same order the game calls them in: getMinX, getMaxX, getMinY, getMaxY
		emit(minX);
		emit(std::array<std::uint8_t, 3>{ 0x0f, 0x2f, 0xc7 });
		branch();
		emit(maxX);
		emit(std::array<std::uint8_t, 4>{ 0x44, 0x0f, 0x2f, 0xc0 });
		branch();
		emit(minY);
		emit(std::array<std::uint8_t, 4>{ 0x41, 0x0f, 0x2f, 0xc1 });
		branch();
		emit(maxY);
		emit(std::array<std::uint8_t, 4>{ 0x44, 0x0f, 0x2f, 0xd0 });
		branch();
		return result;
	}
}
