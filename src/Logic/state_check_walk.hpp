#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

const std::size_t NODE_LEFT = 0, NODE_PARENT = 8, NODE_RIGHT = 16, NODE_ISNIL = 25;
const std::size_t LIST_NEXT = 0, LIST_PREV = 8;

inline bool looks_like_pointer(std::uintptr_t p) {
	return p >= 0x10000 && p < 0x7fff'ffff'ffff && (p & 0xf) == 0;
}

inline std::uintptr_t read_pointer(std::uintptr_t node, std::size_t offset) {
	return *reinterpret_cast<const std::uintptr_t*>(node + offset);
}

inline bool is_nil_node(std::uintptr_t node) {
	return *reinterpret_cast<const unsigned char*>(node + NODE_ISNIL) != 0;
}

inline bool map_intact(std::uintptr_t head, std::size_t size) {
	if (!looks_like_pointer(head) || !is_nil_node(head)) return false;
	auto root = read_pointer(head, NODE_PARENT);
	if (size == 0) return root == head;
	if (!looks_like_pointer(root) || is_nil_node(root) || read_pointer(root, NODE_PARENT) != head)
		return false;
	auto node = read_pointer(head, NODE_LEFT);
	std::size_t seen = 0;
	while (node != head) {
		if (!looks_like_pointer(node) || is_nil_node(node) || ++seen > size) return false;
		for (auto side : { NODE_LEFT, NODE_RIGHT }) {
			auto child = read_pointer(node, side);
			if (child == head) continue;
			if (!looks_like_pointer(child) || is_nil_node(child) || read_pointer(child, NODE_PARENT) != node)
				return false;
		}
		auto right = read_pointer(node, NODE_RIGHT);
		if (right != head) {
			node = right;
			for (std::size_t depth = 0; read_pointer(node, NODE_LEFT) != head; ++depth) {
				if (depth > size) return false;
				node = read_pointer(node, NODE_LEFT);
				if (!looks_like_pointer(node)) return false;
			}
			continue;
		}
		auto parent = read_pointer(node, NODE_PARENT);
		for (std::size_t depth = 0; parent != head && node == read_pointer(parent, NODE_RIGHT); ++depth) {
			if (depth > size || !looks_like_pointer(parent)) return false;
			node = parent;
			parent = read_pointer(node, NODE_PARENT);
		}
		if (parent != head && !looks_like_pointer(parent)) return false;
		node = parent;
	}
	return seen == size && read_pointer(head, NODE_RIGHT) != 0;
}

inline bool list_intact(std::uintptr_t head, std::size_t size) {
	if (!looks_like_pointer(head)) return false;
	auto node = read_pointer(head, LIST_NEXT);
	std::size_t seen = 0;
	while (node != head) {
		if (!looks_like_pointer(node) || ++seen > size || !looks_like_pointer(read_pointer(node, LIST_NEXT)) || read_pointer(read_pointer(node, LIST_NEXT), LIST_PREV) != node)
			return false;
		node = read_pointer(node, LIST_NEXT);
	}
	return seen == size && read_pointer(head, LIST_PREV) != 0;
}

inline bool walk_safely(bool (*walk)(std::uintptr_t, std::size_t), std::uintptr_t head, std::size_t size) {
	__try {
		return walk(head, size);
	}
	__except (1) {
		return false;
	}
}

template <class Map>
bool tree_ok(const Map& map) {
	using Node = std::remove_pointer_t<decltype(map.end()._Ptr)>;
	static_assert(offsetof(Node, _Left) == NODE_LEFT && offsetof(Node, _Parent) == NODE_PARENT && offsetof(Node, _Right) == NODE_RIGHT &&
				  offsetof(Node, _Isnil) == NODE_ISNIL);
	return walk_safely(map_intact, reinterpret_cast<std::uintptr_t>(map.end()._Ptr), map.size());
}

template <class Table>
bool list_ok(const Table& table) {
	using Node = std::remove_pointer_t<decltype(table.end()._Ptr)>;
	static_assert(offsetof(Node, _Next) == LIST_NEXT && offsetof(Node, _Prev) == LIST_PREV);
	return walk_safely(list_intact, reinterpret_cast<std::uintptr_t>(table.end()._Ptr), table.size());
}
