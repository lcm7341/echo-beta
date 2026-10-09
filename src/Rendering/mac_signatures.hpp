#pragma once
#include "circle_signatures.hpp"

namespace echo::rendering::mac {
#if defined(__aarch64__)
	inline constexpr uintptr_t draw_count_address = 0x8bc400;
	// { where, size, hash } of gd code
	inline constexpr echo::circles::Signature circle{ 0x1b4c1c, 0x360, 0x710a0ce686f29620 };
	inline constexpr echo::circles::Signature filled{ 0x1b52e8, 0x30c, 0xb39e4255df9f28ef };
	inline constexpr echo::circles::Signature wave_init{ 0x13e734, 0x1a4, 0x652f7c6cbd730c67 };
	inline constexpr echo::circles::Signature tween{ 0x6ac654, 0x54, 0x3bc1e9db01b5ea98 };
	inline constexpr echo::circles::Signature ease{ 0x46b684, 0x38, 0x3082b61693a25305 };
	inline constexpr uintptr_t shader_address = 0x8b65a8, uniform_address = 0x8b3980;
	inline constexpr uintptr_t color_address = 0x8b3990;
#else
	inline constexpr uintptr_t draw_count_address = 0x9a4fb0;
	// { where, size, hash } of gd code
	inline constexpr echo::circles::Signature circle{ 0x208970, 0x450, 0xb040ef4fb5c0543c };
	inline constexpr echo::circles::Signature filled{ 0x209230, 0x3d0, 0xcb3d3083c545910e };
	inline constexpr echo::circles::Signature wave_init{ 0x178580, 0x1d1, 0xab5f331d6f9e7fc9 };
	inline constexpr echo::circles::Signature tween{ 0x7a5640, 0x62, 0x2f1c338e4550ed2e };
	inline constexpr echo::circles::Signature ease{ 0x514d30, 0x2e, 0x6e5b0cd33c2da52 };
	inline constexpr uintptr_t shader_address = 0x99f2f0, uniform_address = 0x99c690;
	inline constexpr uintptr_t color_address = 0x99c6a0;
#endif
}
