#pragma once

// The pointer-checked container walks behind state_check.cpp, kept free of Geode so
// tests/state_check.py can run them under Wine. MSVC STL only.
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace echo::state_check::detail {
// MSVC's node layouts: a tree node is left, parent, right, color, isnil; a list node is next, prev.
// The head of each is a sentinel (a tree head has isnil set and its parent is the root).
constexpr std::size_t kLeft = 0, kParent = 8, kRight = 16, kIsnil = 25;
constexpr std::size_t kNext = 0, kPrev = 8;

inline bool plausible(std::uintptr_t p) {
	return p >= 0x10000 && p < 0x7fff'ffff'ffff && (p & 0xf) == 0;
}
inline std::uintptr_t at(std::uintptr_t node, std::size_t offset) {
	return *reinterpret_cast<std::uintptr_t const*>(node + offset);
}
inline bool nil(std::uintptr_t node) {
	return *reinterpret_cast<std::uint8_t const*>(node + kIsnil) != 0;
}

// In-order walk by parent links, so it needs no stack. Every link is checked before it is followed.
inline bool walkTree(std::uintptr_t head, std::size_t size) {
	if (!plausible(head) || !nil(head)) return false;
	auto root = at(head, kParent);
	if (size == 0) return root == head;
	if (!plausible(root) || nil(root) || at(root, kParent) != head) return false;
	auto node = at(head, kLeft);
	std::size_t seen = 0;
	while (node != head) {
		if (!plausible(node) || nil(node) || ++seen > size) return false;
		for (auto side : {kLeft, kRight}) {
			auto child = at(node, side);
			if (child == head) continue;
			if (!plausible(child) || nil(child) || at(child, kParent) != node) return false;
		}
		auto right = at(node, kRight);
		if (right != head) {
			node = right;
			for (std::size_t depth = 0; at(node, kLeft) != head; ++depth) {
				if (depth > size) return false;
				node = at(node, kLeft);
				if (!plausible(node)) return false;
			}
			continue;
		}
		auto parent = at(node, kParent);
		for (std::size_t depth = 0; parent != head && node == at(parent, kRight); ++depth) {
			if (depth > size || !plausible(parent)) return false;
			node = parent;
			parent = at(node, kParent);
		}
		if (parent != head && !plausible(parent)) return false;
		node = parent;
	}
	return seen == size && at(head, kRight) != 0;
}

inline bool walkList(std::uintptr_t head, std::size_t size) {
	if (!plausible(head)) return false;
	auto node = at(head, kNext);
	std::size_t seen = 0;
	while (node != head) {
		if (!plausible(node) || ++seen > size || !plausible(at(node, kNext)) || at(at(node, kNext), kPrev) != node) return false;
		node = at(node, kNext);
	}
	return seen == size && at(head, kPrev) != 0;
}

// A pointer that passes `plausible` can still be unmapped once its pages are released.
inline bool guarded(bool (*walk)(std::uintptr_t, std::size_t), std::uintptr_t head, std::size_t size) {
	__try {
		return walk(head, size);
	} __except (1) {
		return false;
	}
}

template <class Map>
bool tree(Map const& map) {
	using Node = std::remove_pointer_t<decltype(map.end()._Ptr)>;
	static_assert(offsetof(Node, _Left) == kLeft && offsetof(Node, _Parent) == kParent && offsetof(Node, _Right) == kRight &&
		offsetof(Node, _Isnil) == kIsnil);
	return guarded(walkTree, reinterpret_cast<std::uintptr_t>(map.end()._Ptr), map.size());
}

template <class Hash>
bool list(Hash const& hash) {
	using Node = std::remove_pointer_t<decltype(hash.end()._Ptr)>;
	static_assert(offsetof(Node, _Next) == kNext && offsetof(Node, _Prev) == kPrev);
	return guarded(walkList, reinterpret_cast<std::uintptr_t>(hash.end()._Ptr), hash.size());
}

}
