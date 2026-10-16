#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace CollisionBounds {
	void prepare();
	inline const std::uintptr_t site = 0x214b09;
	inline const std::uintptr_t miss = 0x215a18;
	inline const std::size_t size = 0x53;
	// cocos 2.2081 rect getters without the ret. keep the sse order (width + x, then height + y)
	// swapping them changes how nans come out
	inline const std::array<unsigned char, 9> maxX{ 0xf3, 0x0f, 0x10, 0x41, 0x08, 0xf3, 0x0f, 0x58, 0x01 };
	inline const std::array<unsigned char, 4> minX{ 0xf3, 0x0f, 0x10, 0x01 };
	inline const std::array<unsigned char, 10> maxY{ 0xf3, 0x0f, 0x10, 0x41, 0x0c, 0xf3, 0x0f, 0x58, 0x41, 0x04 };
	inline const std::array<unsigned char, 5> minY{ 0xf3, 0x0f, 0x10, 0x41, 0x04 };

	inline std::array<unsigned char, size> code(std::uintptr_t address, std::uintptr_t rejected) {
		std::array<unsigned char, size> result;
		result.fill(0x90);
		std::size_t used = 0;
		auto emit = [&](const auto& bytes) {
			for (auto byte : bytes)
				result[used++] = byte;
		};
		auto branch = [&] {
			emit(std::array<unsigned char, 2>{ 0x0f, 0x87 }); // ja rejected
			auto distance = (int)(rejected - (address + used + 4));
			std::memcpy(result.data() + used, &distance, 4);
			used += 4;
		};
		emit(std::array<unsigned char, 5>{ 0x48, 0x8d, 0x4c, 0x24, 0x40 }); // lea rcx,[rsp+40h]
		emit(minX);
		emit(std::array<unsigned char, 3>{ 0x0f, 0x2f, 0xc7 });
		branch();
		emit(maxX);
		emit(std::array<unsigned char, 4>{ 0x44, 0x0f, 0x2f, 0xc0 });
		branch();
		emit(minY);
		emit(std::array<unsigned char, 4>{ 0x41, 0x0f, 0x2f, 0xc1 });
		branch();
		emit(maxY);
		emit(std::array<unsigned char, 4>{ 0x44, 0x0f, 0x2f, 0xd0 });
		branch();
		return result;
	}
}
