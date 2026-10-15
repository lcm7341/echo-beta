#pragma once
#include <array>
#include <cstdint>
#include <cstddef>

//    HOW 2 FIND SHIT
//
//    ./build/_deps/bindings-src/bindings/2.2081/GeometryDash.bro
//
//    win: exe in steamapps
//    mac: Geometry Dash.app/Contents/MacOS/Geometry Dash. arm & intel are in 1 file so lipo -thin arm64 / x86_64 first
//    ios: GeometryJump from launcher: xcrun devicectl device copy from
//    android64: adb pull libcocos2dcpp.so
//
// !! disassemble ONLY the function. llvm-objdump -d --start-address=STARTADDR --stop-address=ENDADDR 
//    apple: otool -tv
//    apple offsets = address - 0x100000000
//
// !! find the instructions to change in there
//    offset = address of the first one
//    keep the original asm in a comment over the patch
//    it's good practice.
//
//    other platforms: same function in the .bro, same math
//    arm step count: fcvt / fdiv / fmul / frinta
//
//    hashes: fnv-1a over { where, size } of ORIGINAL bytes (echo::circles::fingerprint)
//    clean binary only, NOT from memory w/ mods loaded
//
//    inlined (mac processCommands, getModifiedDelta): no call, look at the calls around it

namespace echo::patches {
	// 2.2081 ONLY. checked on the windows exe & the arm64 lib off my phone
	// android: rob calls a whole other function just to copy a CCPoint
	inline constexpr uintptr_t point_address = 0xace648;
	inline constexpr std::array<uint32_t, 11> point_original = {
		0xa9be7bfd, 0x910003fd, 0xbd400020, 0xf9000bf3, 0xbd400421,
		0xaa0003f3, 0x97fffff7, 0xaa1303e0, 0xf9400bf3, 0xa8c27bfd, 0xd65f03c0
	};
	inline constexpr std::array<uint32_t, 5> point_replacement = {
		0xbd400020, 0xbd400421, 0xbd000000, 0xbd000401, 0xd65f03c0
	};
	inline constexpr uintptr_t rings_address = 0x623e2c;
	inline constexpr std::array<uint32_t, 13> rings_original = {
		0xf9451680, 0xb40000a0, 0xf9400013, 0x941aa02e, 0xaa1303e0,
		0xb5ffffb3, 0xf94006a2, 0x52800001, 0xf9450e80, 0xd37df042,
		0x97fad3df, 0xf9000ebf, 0xf9000abf
	};
	// longer cbz so an empty set skips the memset too
	inline constexpr std::array<uint32_t, 1> rings_replacement = { 0xb4000140 };
	inline constexpr uintptr_t queue_address = 0x231daa;
	inline constexpr std::array<uint8_t, 16> queue_original = {
		0xe8, 0x87, 0xfa, 0x29, 0x00, 0x48, 0x83, 0x83,
		0x08, 0x33, 0x00, 0x00, 0xe8, 0x48, 0x8b, 0xc6
	};
	// call the cave instead of memmove
	inline constexpr std::array<uint8_t, 5> queue_replacement = { 0xe8, 0x44, 0, 0, 0 };
	inline constexpr uintptr_t queue_cave_address = 0x231df3;
	inline constexpr std::array<uint8_t, 13> queue_cave_original = {
		0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc
	};
	// test r8, r8; je ret; jmp memmove; ret
	inline constexpr std::array<uint8_t, 11> queue_cave_replacement = {
		0x4d, 0x85, 0xc0, 0x74, 0x05, 0xe9, 0x39, 0xfa, 0x29, 0x00, 0xc3
	};

	// windows: the event state map gets looked up 2x in a row w/ the same key
	// so skip the 2nd one & reuse the first
	inline constexpr uintptr_t event_lookup_signature_address = 0x232025;
	inline constexpr std::array<uint8_t, 58> event_lookup_original = {
		0x48,
		0x8d,
		0x50,
		0x08,
		0x48,
		0x81,
		0xc1,
		0x98,
		0x05,
		0x00,
		0x00,
		0xe8,
		0xcb,
		0xe7,
		0xf9,
		0xff,
		0x8b,
		0x96,
		0xe0,
		0x03,
		0x00,
		0x00,
		0x39,
		0x10,
		0x0f,
		0x84,
		0x94,
		0x00,
		0x00,
		0x00,
		0x48,
		0x8d,
		0x94,
		0x24,
		0x80,
		0x00,
		0x00,
		0x00,
		0x48,
		0x8d,
		0x8e,
		0x98,
		0x05,
		0x00,
		0x00,
		0xe8,
		0xa9,
		0xe7,
		0xf9,
		0xff,
		0x8b,
		0x8e,
		0xe0,
		0x03,
		0x00,
		0x00,
		0x89,
		0x08,
	};
	inline constexpr uintptr_t event_lookup_address = 0x232043;
	// jmp over the second lookup
	inline constexpr std::array<uint8_t, 2> event_lookup_replacement = { 0xeb, 0x12 };
	// the lookup function, hashed in case someone hooked it
	inline constexpr uintptr_t event_map_address = 0x1d0800;
	inline constexpr size_t event_map_size = 0x101;
	inline constexpr uint64_t event_map_hash = 0xc75bfd8bb81b108d; // gd code hash
}
