#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace CollisionBounds {
	void prepare();
	inline const std::uintptr_t BOUNDS_CHECK_SITE = 0x214b09;
	inline const std::uintptr_t NOT_TOUCHING = 0x215a18;
	inline const std::size_t PATCH_SIZE = 0x53;
	// cocos 2.2081 rect getters without the ret. keep the sse order (width + x, then height + y)
	// swapping them changes how nans come out
	inline const std::array<unsigned char, 9> max_x_code{ 0xf3, 0x0f, 0x10, 0x41, 0x08, 0xf3, 0x0f, 0x58, 0x01 };
	inline const std::array<unsigned char, 4> min_x_code{ 0xf3, 0x0f, 0x10, 0x01 };
	inline const std::array<unsigned char, 10> max_y_code{ 0xf3, 0x0f, 0x10, 0x41, 0x0c, 0xf3, 0x0f, 0x58, 0x41, 0x04 };
	inline const std::array<unsigned char, 5> min_y_code{ 0xf3, 0x0f, 0x10, 0x41, 0x04 };

	inline std::array<unsigned char, PATCH_SIZE> build_patch(std::uintptr_t address, std::uintptr_t not_touching) {
		std::array<unsigned char, PATCH_SIZE> patch;
		patch.fill(0x90);
		std::size_t length = 0;
		auto append = [&](const auto& bytes) {
			for (auto byte : bytes)
				patch[length++] = byte;
		};
		auto jump_if_outside = [&] {
			append(std::array<unsigned char, 2>{ 0x0f, 0x87 }); // ja not_touching
			auto distance = (int)(not_touching - (address + length + 4));
			std::memcpy(patch.data() + length, &distance, 4);
			length += 4;
		};
		append(std::array<unsigned char, 5>{ 0x48, 0x8d, 0x4c, 0x24, 0x40 }); // lea rcx,[rsp+40h]
		append(min_x_code);
		append(std::array<unsigned char, 3>{ 0x0f, 0x2f, 0xc7 });
		jump_if_outside();
		append(max_x_code);
		append(std::array<unsigned char, 4>{ 0x44, 0x0f, 0x2f, 0xc0 });
		jump_if_outside();
		append(min_y_code);
		append(std::array<unsigned char, 4>{ 0x41, 0x0f, 0x2f, 0xc1 });
		jump_if_outside();
		append(max_y_code);
		append(std::array<unsigned char, 4>{ 0x44, 0x0f, 0x2f, 0xd0 });
		jump_if_outside();
		return patch;
	}
}
