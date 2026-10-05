// the sse 4x4 multiply itself: out = a * b, 1 result column at a time
// supported() says if it may be used: no nan / inf in either matrix & every float exception masked (mxcsr)
#pragma once
#include <immintrin.h>
#include <cstring>
#include <cstdint>

namespace echo::matrix {
	inline bool supported(float const* a, float const* b) {
		if ((_mm_getcsr() & 0x1f80) != 0x1f80)
			return false;
		auto exponent = _mm_set1_epi32(0x7f800000); // float exponent bits (inf / nan check)
		auto invalid = _mm_setzero_si128();
		for (int i = 0; i < 16; i += 4) {
			auto x = _mm_castps_si128(_mm_loadu_ps(a + i));
			auto y = _mm_castps_si128(_mm_loadu_ps(b + i));
			invalid = _mm_or_si128(invalid, _mm_cmpeq_epi32(_mm_and_si128(x, exponent), exponent));
			invalid = _mm_or_si128(invalid, _mm_cmpeq_epi32(_mm_and_si128(y, exponent), exponent));
		}
		return _mm_movemask_epi8(invalid) == 0;
	}

	inline float* multiply(float* out, float const* a, float const* b) {
		auto a0 = _mm_loadu_ps(a), a1 = _mm_loadu_ps(a + 4);
		auto a2 = _mm_loadu_ps(a + 8), a3 = _mm_loadu_ps(a + 12);
		auto b0 = _mm_loadu_ps(b), b1 = _mm_loadu_ps(b + 4);
		auto b2 = _mm_loadu_ps(b + 8), b3 = _mm_loadu_ps(b + 12);
		auto column = [&](auto v) {
			auto r = _mm_mul_ps(a0, _mm_shuffle_ps(v, v, 0x00));
			r = _mm_add_ps(r, _mm_mul_ps(a1, _mm_shuffle_ps(v, v, 0x55)));
			r = _mm_add_ps(r, _mm_mul_ps(_mm_shuffle_ps(v, v, 0xaa), a2));
			return _mm_add_ps(r, _mm_mul_ps(_mm_shuffle_ps(v, v, 0xff), a3));
		};
		auto r0 = column(b0), r1 = column(b1), r2 = column(b2), r3 = column(b3);
		_mm_storeu_ps(out, r0);
		_mm_storeu_ps(out + 4, r1);
		_mm_storeu_ps(out + 8, r2);
		_mm_storeu_ps(out + 12, r3);
		return out;
	}
}
