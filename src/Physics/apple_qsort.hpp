#pragma once
#include <cstddef>
#include <cstring>
#include <vector>

// apples qsort, written out from their Libc (stdlib/FreeBSD/qsort.c). WHICH of 2 equal things ends up first is up to the sort,
// & androids puts them another way round, so a spider landing on stacked objects picked a different one there
// checked against the real one on a mac: 400k arrays full of ties, same order every time
namespace echo::sort {
	using Compare = int (*)(void const*, void const*);

	inline void swap_bytes(char* a, char* b, std::size_t size) {
		while (size--) {
			char held = *a;
			*a++ = *b;
			*b++ = held;
		}
	}

	inline char* med3(char* a, char* b, char* c, Compare cmp) {
		return cmp(a, b) < 0 ? (cmp(b, c) < 0 ? b : (cmp(a, c) < 0 ? c : a))
							 : (cmp(b, c) > 0 ? b : (cmp(a, c) < 0 ? a : c));
	}

	// limit 0 = always finishes
	inline bool insertion(char* a, std::size_t n, std::size_t es, Compare cmp, std::size_t limit) {
		std::size_t swaps = 0;
		for (char* pm = a + es; pm < a + n * es; pm += es)
			for (char* pl = pm; pl > a && cmp(pl - es, pl) > 0; pl -= es) {
				swap_bytes(pl, pl - es, es);
				if (limit && ++swaps > limit)
					return false;
			}
		return true;
	}

	// what it falls back on when it has gone too deep (their heapsort.c). items count from 1 in there
	inline void apple_heapsort(void* vbase, std::size_t nmemb, std::size_t size, Compare cmp) {
		if (nmemb <= 1 || !size)
			return;
		std::vector<char> held(size);
		char* k = held.data();
		char* base = static_cast<char*>(vbase) - size;
		std::size_t i = 0, j = 0;
		for (std::size_t l = nmemb / 2 + 1; --l;) {
			for (i = l; (j = i * 2) <= nmemb; i = j) {
				char* child = base + j * size;
				if (j < nmemb && cmp(child, child + size) < 0) {
					child += size;
					++j;
				}
				char* par = base + i * size;
				if (cmp(child, par) <= 0)
					break;
				swap_bytes(par, child, size);
			}
		}
		while (nmemb > 1) {
			std::memcpy(k, base + nmemb * size, size);
			std::memcpy(base + nmemb * size, base + size, size);
			--nmemb;
			for (i = 1; (j = i * 2) <= nmemb; i = j) {
				char* child = base + j * size;
				if (j < nmemb && cmp(child, child + size) < 0) {
					child += size;
					++j;
				}
				std::memcpy(base + i * size, child, size);
			}
			for (;;) {
				j = i;
				i = j / 2;
				char* child = base + j * size;
				char* par = base + i * size;
				if (j == 1 || cmp(k, par) < 0) {
					std::memcpy(child, k, size);
					break;
				}
				std::memcpy(child, par, size);
			}
		}
	}

	inline void apple_qsort_at(char* a, std::size_t n, std::size_t es, Compare cmp, int depth) {
		for (;;) {
			if (depth-- <= 0) {
				apple_heapsort(a, n, es, cmp);
				return;
			}
			if (n <= 7) {
				insertion(a, n, es, cmp, 0);
				return;
			}
			char* pl = a;
			char* pm = a + (n / 2) * es;
			char* pn = a + (n - 1) * es;
			if (n > 40) {
				std::size_t d = (n / 8) * es;
				pl = med3(pl, pl + d, pl + 2 * d, cmp);
				pm = med3(pm - d, pm, pm + d, cmp);
				pn = med3(pn - 2 * d, pn - d, pn, cmp);
			}
			pm = med3(pl, pm, pn, cmp);
			swap_bytes(a, pm, es);
			char* pa = a + es;
			char* pb = pa;
			char* pc = a + (n - 1) * es;
			char* pd = pc;
			bool swapped = false;
			int r = 0;
			for (;;) {
				while (pb <= pc && (r = cmp(pb, a)) <= 0) {
					if (r == 0) {
						swapped = true;
						swap_bytes(pa, pb, es);
						pa += es;
					}
					pb += es;
				}
				while (pb <= pc && (r = cmp(pc, a)) >= 0) {
					if (r == 0) {
						swapped = true;
						swap_bytes(pc, pd, es);
						pd -= es;
					}
					pc -= es;
				}
				if (pb > pc)
					break;
				swap_bytes(pb, pc, es);
				swapped = true;
				pb += es;
				pc -= es;
			}
			pn = a + n * es;
			auto least = [](std::size_t x, std::size_t y) {
				return x < y ? x : y;
			};
			std::size_t d1 = least(static_cast<std::size_t>(pa - a), static_cast<std::size_t>(pb - pa));
			if (d1)
				swap_bytes(a, pb - d1, d1);
			d1 = least(static_cast<std::size_t>(pd - pc), static_cast<std::size_t>(pn - pd) - es);
			if (d1)
				swap_bytes(pb, pn - d1, d1);
			// nothing moved = nearly sorted, try insertion but give up after n/4 + 1 swaps
			if (!swapped && insertion(a, n, es, cmp, 1 + n / 4))
				return;
			d1 = static_cast<std::size_t>(pb - pa);
			std::size_t d2 = static_cast<std::size_t>(pd - pc);
			if (d1 <= d2) {
				if (d1 > es)
					apple_qsort_at(a, d1 / es, es, cmp, depth);
				if (d2 <= es)
					return;
				a = pn - d2;
				n = d2 / es;
			}
			else {
				if (d2 > es)
					apple_qsort_at(pn - d2, d2 / es, es, cmp, depth);
				if (d1 <= es)
					return;
				n = d1 / es;
			}
		}
	}

	inline void apple_qsort(void* base, std::size_t n, std::size_t es, Compare cmp) {
		// 2 * (the highest set bit of n), same as their DEPTH()
		int bits = 0;
		for (std::size_t left = n; left; left >>= 1)
			++bits;
		apple_qsort_at(static_cast<char*>(base), n, es, cmp, 2 * (bits - 1));
	}
}
