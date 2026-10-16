#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

const std::size_t NODE_LEFT = 0, NODE_PARENT = 8, NODE_RIGHT = 16, NODE_ISNIL = 25;
const std::size_t LIST_NEXT = 0, LIST_PREV = 8;

inline bool plausible_pointer(std::uintptr_t p) {
	return p >= 0x10000 && p < 0x7fff'ffff'ffff && (p & 0xf) == 0;
}

inline std::uintptr_t read_word_at(std::uintptr_t node, std::size_t offset) {
	return *reinterpret_cast<const std::uintptr_t*>(node + offset);
}

inline bool is_nil_node(std::uintptr_t node) {
	return *reinterpret_cast<const unsigned char*>(node + NODE_ISNIL) != 0;
}

inline bool walk_tree(std::uintptr_t head, std::size_t size) {
	if (!plausible_pointer(head) || !is_nil_node(head)) return false;
	auto root = read_word_at(head, NODE_PARENT);
	if (size == 0) return root == head;
	if (!plausible_pointer(root) || is_nil_node(root) || read_word_at(root, NODE_PARENT) != head)
		return false;
	auto node = read_word_at(head, NODE_LEFT);
	std::size_t seen = 0;
	while (node != head) {
		if (!plausible_pointer(node) || is_nil_node(node) || ++seen > size) return false;
		for (auto side : { NODE_LEFT, NODE_RIGHT }) {
			auto child = read_word_at(node, side);
			if (child == head) continue;
			if (!plausible_pointer(child) || is_nil_node(child) || read_word_at(child, NODE_PARENT) != node)
				return false;
		}
		auto right = read_word_at(node, NODE_RIGHT);
		if (right != head) {
			node = right;
			for (std::size_t depth = 0; read_word_at(node, NODE_LEFT) != head; ++depth) {
				if (depth > size) return false;
				node = read_word_at(node, NODE_LEFT);
				if (!plausible_pointer(node)) return false;
			}
			continue;
		}
		auto parent = read_word_at(node, NODE_PARENT);
		for (std::size_t depth = 0; parent != head && node == read_word_at(parent, NODE_RIGHT); ++depth) {
			if (depth > size || !plausible_pointer(parent)) return false;
			node = parent;
			parent = read_word_at(node, NODE_PARENT);
		}
		if (parent != head && !plausible_pointer(parent)) return false;
		node = parent;
	}
	return seen == size && read_word_at(head, NODE_RIGHT) != 0;
}

inline bool walk_list(std::uintptr_t head, std::size_t size) {
	if (!plausible_pointer(head)) return false;
	auto node = read_word_at(head, LIST_NEXT);
	std::size_t seen = 0;
	while (node != head) {
		if (!plausible_pointer(node) || ++seen > size || !plausible_pointer(read_word_at(node, LIST_NEXT)) || read_word_at(read_word_at(node, LIST_NEXT), LIST_PREV) != node)
			return false;
		node = read_word_at(node, LIST_NEXT);
	}
	return seen == size && read_word_at(head, LIST_PREV) != 0;
}

inline bool guarded_walk(bool (*walk)(std::uintptr_t, std::size_t), std::uintptr_t head, std::size_t size) {
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
	return guarded_walk(walk_tree, reinterpret_cast<std::uintptr_t>(map.end()._Ptr), map.size());
}

template <class Table>
bool list_ok(const Table& table) {
	using Node = std::remove_pointer_t<decltype(table.end()._Ptr)>;
	static_assert(offsetof(Node, _Next) == LIST_NEXT && offsetof(Node, _Prev) == LIST_PREV);
	return guarded_walk(walk_list, reinterpret_cast<std::uintptr_t>(table.end()._Ptr), table.size());
}
