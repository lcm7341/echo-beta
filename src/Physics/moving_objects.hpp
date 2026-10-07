// moving big groups of objects fast
// a move trigger on thousands of objects mostly waits on memory, every object is somewhere else on the heap
// moveOne does to 1 object what gd does. the ones coming up are prefetched while it runs (prefetch_object)
// huge groups are split over a few threads (MovePool), only when the group has no duplicates so 2 threads never touch the same object
// gd keeps positions as double under 1e7 & float above it, both ways are kept (section)
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>
#include <vector>
#include <limits>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace chrono = std::chrono;

namespace echo::movement {
	void prepare();

	template <class T>
	inline T read(void const* object, size_t offset) {
		T value;
		std::memcpy(&value, static_cast<char const*>(object) + offset, sizeof(T));
		return value;
	}

	template <class T>
	inline void write(void* object, size_t offset, T value) {
		std::memcpy(static_cast<char*>(object) + offset, &value, sizeof(T));
	}

	// above 1e7 gd uses FLOAT below it DOUBLE. keep both
	inline bool section(double position, float factor, int& result) {
		if (!std::isfinite(position) || !std::isfinite(factor))
			return false;
		double scaled = position <= 0. ? 0. : position >= 10000000. ? double(factor * 10000000.f)
																	: double(factor) * position;
		if (!std::isfinite(scaled) || scaled < std::numeric_limits<int>::min() || scaled > std::numeric_limits<int>::max())
			return false;
		result = static_cast<int>(scaled);
		return true;
	}

	inline bool section_unchanged(void const* layer, void const* object, double px, double py) {
		int old_x = read<int>(object, 0x278);
		if (old_x < 0)
			return true; // gd returns before reading pos
#if defined(__SSE2__) || defined(_M_X64)
		__m128d position = _mm_set_pd(py, px);
		__m128d factors = _mm_cvtps_pd(_mm_castsi128_ps(_mm_cvtsi64_si128(read<int64_t>(layer, 0x36a0))));
		__m128d valid_pos = _mm_and_pd(_mm_cmpge_pd(position, _mm_set1_pd(-std::numeric_limits<double>::max())),
			_mm_cmplt_pd(position, _mm_set1_pd(10000000.)));
		__m128d valid_factor = _mm_and_pd(_mm_cmpge_pd(factors, _mm_setzero_pd()),
			_mm_cmple_pd(factors, _mm_set1_pd(200.)));
		if (_mm_movemask_pd(_mm_and_pd(valid_pos, valid_factor)) == 3) {
			__m128i cell = _mm_cvttpd_epi32(_mm_mul_pd(_mm_max_pd(position, _mm_setzero_pd()), factors));
			__m128i old_cell = _mm_cvtsi64_si128(read<int64_t>(object, 0x278));
			return (_mm_movemask_epi8(_mm_cmpeq_epi32(cell, old_cell)) & 0xff) == 0xff;
		}
#else
		float fx = read<float>(layer, 0x36a0), fy = read<float>(layer, 0x36a4);
		if (px >= 0. && px < 10000000. && py >= 0. && py < 10000000. &&
			fx >= 0.f && fx <= 200.f && fy >= 0.f && fy <= 200.f) {
			return static_cast<int>(double(fx) * px) == old_x &&
				   static_cast<int>(double(fy) * py) == read<int>(object, 0x27c);
		}
#endif
		int x, y;
		return section(px, read<float>(layer, 0x36a0), x) &&
			   section(py, read<float>(layer, 0x36a4), y) &&
			   x == old_x && y == read<int>(object, 0x27c);
	}

	inline bool section_unchanged(void const* layer, void const* object) {
		return section_unchanged(layer, object, read<double>(object, 0x3b8), read<double>(object, 0x3c0));
	}

	inline void prefetch_object(void const* object) {
#if defined(__SSE2__) || defined(_M_X64)
		auto* p = static_cast<char const*>(object);
		for (size_t offset : { 0x278, 0x27f, 0x2c8, 0x351, 0x369, 0x3b8, 0x3c7, 0x4d0, 0x4df, 0x512 })
			_mm_prefetch(p + offset, _MM_HINT_T0);
#else
		(void)object;
#endif
	}

	constexpr unsigned PREFETCH_AHEAD = 8;

	inline bool moveOne(void* layer, void* object, double dx, double dy, bool move_x, bool move_y) {
		double x = read<double>(object, 0x3b8), y = read<double>(object, 0x3c0);
		if (!read<bool>(object, 0x512) && read<int>(object, 0x4dc) != read<int>(layer, 0x3e0)) {
			write(object, 0x4d0, static_cast<float>(x));
			write(object, 0x4d4, static_cast<float>(y));
			write(object, 0x4dc, read<int>(layer, 0x3e0));
			write(object, 0x368, true);
			write(object, 0x369, true);
		}
		if (move_x && !read<bool>(object, 0x2c8))
			write(object, 0x3b8, x += dx);
		if (move_y)
			write(object, 0x3c0, y += dy);
		write(object, 0x351, true);
		write(object, 0x352, true);
		return !section_unchanged(layer, object, x, y);
	}

	inline std::uint64_t group_epoch = 1;

#if defined(__SSE2__) || defined(_M_X64)
#define ECHO_PARALLEL_MOVE 1

	struct GroupCheck {
		void* array;
		std::uint64_t epoch;
		unsigned size;
		bool clean;
	};

	inline GroupCheck group_checks[64]{};

	inline unsigned verified_members(void* array, void* const* objects, unsigned size) {
		auto& entry = group_checks[(reinterpret_cast<uintptr_t>(array) >> 4) & 63];
		if (entry.array == array && entry.epoch == group_epoch && size <= entry.size)
			return entry.clean ? size : 0;
		unsigned live = 0;
		while (live < size && objects[live])
			++live;
		std::vector<void*> sorted(objects, objects + live);
		std::sort(sorted.begin(), sorted.end());
		bool unique = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
		entry = { array, group_epoch, size, unique && live == size };
		return unique ? live : 0;
	}

#endif

	inline unsigned parallel_minimum = 512;

	template <class Count, class Data, class UpdateSection, class Finish>
	void move(void* layer, void* array, double dx, double dy,
		Count count, Data data, UpdateSection update_section, Finish finish) {
		unsigned total = count(array);
		write(layer, 0x3724, read<uint32_t>(layer, 0x3724) + total);
		auto [objects, size] = data(array);
		bool move_x = dx != 0., move_y = dy != 0.;
#ifdef ECHO_PARALLEL_MOVE
		if (unsigned live = size >= parallel_minimum ? verified_members(array, reinterpret_cast<void* const*>(objects), size) : 0;
			live >= parallel_minimum) {
			static std::vector<uint8_t> changed;
			changed.resize(live);
			auto& pool = MovePool::get();
			constexpr unsigned CHUNK = 128;
			std::atomic<unsigned> cursor{ 0 };
			auto slice = [&](unsigned) {
				for (;;) {
					unsigned begin = cursor.fetch_add(CHUNK, std::memory_order_relaxed);
					if (begin >= live)
						return;
					unsigned end = std::min(begin + CHUNK, live);
					for (unsigned i = begin; i < end && i < begin + PREFETCH_AHEAD; ++i)
						prefetch_object(objects[i]);
					for (unsigned i = begin; i < end; ++i) {
						if (i + PREFETCH_AHEAD < end)
							prefetch_object(objects[i + PREFETCH_AHEAD]);
						changed[i] = moveOne(layer, objects[i], dx, dy, move_x, move_y);
					}
				}
			};
			pool.run(slice);
			for (unsigned i = 0; i < live; ++i)
				if (changed[i])
					update_section(objects[i]);
			finish(array);
			return;
		}
#endif
		unsigned live = 0;
		while (live < size && objects[live])
			++live;
		for (unsigned i = 0; i < live && i < PREFETCH_AHEAD; ++i)
			prefetch_object(objects[i]);
		for (unsigned i = 0; i < live; ++i) {
			if (i + PREFETCH_AHEAD < live)
				prefetch_object(objects[i + PREFETCH_AHEAD]);
			if (moveOne(layer, objects[i], dx, dy, move_x, move_y))
				update_section(objects[i]);
		}
		finish(array);
	}
}
