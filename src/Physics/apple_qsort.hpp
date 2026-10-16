#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

// apple's qsort, copied by hand from their Libc (stdlib/FreeBSD/qsort.c & heapsort.c).
//
// gd sorts objects w/ whatever qsort the system has, & when 2 things compare equal which 1 ends up first is up
// to the sort. apple's & android's break those ties differently. on dark sheep 3 objects sit on the exact same
// spot & p2's spider landed 1 block lower on android than mac/ios bc of it. so android uses ts instead (see
// android_sort.cpp) & everything ties the same way everywhere. i THINK thats the only thing that differed
// there, it fixed that lvl at least.
//
// how it works as far as i can tell:
//  - 7 or less items = insertion sort
//  - otherwise pick a pivot (middle of 3, or middle of 3 middles on big arrays) & swap it to the front
//  - walk in from both ends. equal to the pivot gets parked at the ends, smaller goes left, bigger goes right,
//    then the parked equal stuff gets swapped into the middle
//  - if nothing moved its probably already sorted, so it tries an insertion sort that gives up after
//    count / 4 + 1 swaps
//  - smaller side gets sorted by calling itself, bigger side just loops
//  - too deep (bad pivots over & over) = bail to heapsort so it cant get super slow
//
// EVERY swap has to happen in the same order as apple's or the ties come out different & ts is pointless.
// checked it against the real qsort on my mac w/ a ton of arrays full of ties & it matched every time, but
// if i touch any of ts i need to run that check again

using Compare = int (*)(const void*, const void*);

inline void swap_items(char* a, char* b, size_t item_size) {
	for (size_t i = 0; i < item_size; i++)
		std::swap(a[i], b[i]);
}

inline char* median_of_3(char* a, char* b, char* c, Compare compare) {
	if (compare(a, b) < 0) {
		if (compare(b, c) < 0) return b;
		return compare(a, c) < 0 ? c : a;
	}
	if (compare(b, c) > 0) return b;
	return compare(a, c) < 0 ? a : c;
}

inline bool insertion_sort(char* start, size_t count, size_t item_size, Compare compare, size_t max_swaps) {
	size_t swaps = 0;
	for (char* item = start + item_size; item < start + count * item_size; item += item_size) {
		for (char* spot = item; spot > start && compare(spot - item_size, spot) > 0; spot -= item_size) {
			swap_items(spot, spot - item_size, item_size);
			swaps++;
			if (max_swaps != 0 && swaps > max_swaps) return false;
		}
	}
	return true;
}

inline void apple_heapsort(void* array, size_t count, size_t item_size, Compare compare) {
	if (count <= 1 || item_size == 0) return;
	std::vector<char> held_item(item_size);
	char* held = held_item.data();
	char* base = (char*)array - item_size;
	auto at = [&](size_t index) { return base + index * item_size; };

	for (size_t top = count / 2; top >= 1; top--) {
		size_t parent = top;
		size_t child;
		while ((child = parent * 2) <= count) {
			if (child < count && compare(at(child), at(child + 1)) < 0) child++;
			if (compare(at(child), at(parent)) <= 0) break;
			swap_items(at(parent), at(child), item_size);
			parent = child;
		}
	}

	while (count > 1) {
		std::memcpy(held, at(count), item_size);
		std::memcpy(at(count), at(1), item_size);
		count--;

		size_t parent = 1;
		size_t child;
		while ((child = parent * 2) <= count) {
			if (child < count && compare(at(child), at(child + 1)) < 0) child++;
			std::memcpy(at(parent), at(child), item_size);
			parent = child;
		}
		size_t hole = parent;
		while (true) {
			size_t above = hole / 2;
			if (hole == 1 || compare(held, at(above)) < 0) {
				std::memcpy(at(hole), held, item_size);
				break;
			}
			std::memcpy(at(hole), at(above), item_size);
			hole = above;
		}
	}
}

inline void apple_qsort_part(char* start, size_t count, size_t item_size, Compare compare, int depth_left) {
	while (true) {
		if (depth_left <= 0) {
			apple_heapsort(start, count, item_size, compare);
			return;
		}
		depth_left--;
		if (count <= 7) {
			insertion_sort(start, count, item_size, compare, 0);
			return;
		}

		char* first = start;
		char* middle = start + (count / 2) * item_size;
		char* last = start + (count - 1) * item_size;
		if (count > 40) {
			size_t gap = (count / 8) * item_size;
			first = median_of_3(first, first + gap, first + 2 * gap, compare);
			middle = median_of_3(middle - gap, middle, middle + gap, compare);
			last = median_of_3(last - 2 * gap, last - gap, last, compare);
		}
		middle = median_of_3(first, middle, last, compare);
		swap_items(start, middle, item_size);
		char* pivot = start;

		char* left_equal_end = start + item_size;
		char* left = left_equal_end;
		char* right = start + (count - 1) * item_size;
		char* right_equal_start = right;
		bool anything_moved = false;
		while (true) {
			int result;
			while (left <= right && (result = compare(left, pivot)) <= 0) {
				if (result == 0) {
					anything_moved = true;
					swap_items(left_equal_end, left, item_size);
					left_equal_end += item_size;
				}
				left += item_size;
			}
			while (left <= right && (result = compare(right, pivot)) >= 0) {
				if (result == 0) {
					anything_moved = true;
					swap_items(right, right_equal_start, item_size);
					right_equal_start -= item_size;
				}
				right -= item_size;
			}
			if (left > right) break;
			swap_items(left, right, item_size);
			anything_moved = true;
			left += item_size;
			right -= item_size;
		}

		char* end = start + count * item_size;
		size_t amount = std::min((size_t)(left_equal_end - start), (size_t)(left - left_equal_end));
		if (amount != 0) swap_items(start, left - amount, amount);
		amount = std::min((size_t)(right_equal_start - right), (size_t)(end - right_equal_start) - item_size);
		if (amount != 0) swap_items(left, end - amount, amount);

		if (!anything_moved && insertion_sort(start, count, item_size, compare, 1 + count / 4))
			return;

		size_t less_bytes = (size_t)(left - left_equal_end);
		size_t greater_bytes = (size_t)(right_equal_start - right);
		if (less_bytes <= greater_bytes) {
			if (less_bytes > item_size)
				apple_qsort_part(start, less_bytes / item_size, item_size, compare, depth_left);
			if (greater_bytes <= item_size) return;
			start = end - greater_bytes;
			count = greater_bytes / item_size;
		}
		else {
			if (greater_bytes > item_size)
				apple_qsort_part(end - greater_bytes, greater_bytes / item_size, item_size, compare, depth_left);
			if (less_bytes <= item_size) return;
			count = less_bytes / item_size;
		}
	}
}

inline void apple_qsort(void* array, size_t count, size_t item_size, Compare compare) {
	int bits = 0;
	for (size_t left = count; left > 0; left >>= 1)
		bits++;
	apple_qsort_part((char*)array, count, item_size, compare, 2 * (bits - 1));
}
