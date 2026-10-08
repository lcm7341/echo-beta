#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

namespace echo::state_check::detail {
	constexpr std::size_t NODE_LEFT = 0, NODE_PARENT = 8, NODE_RIGHT = 16, NODE_ISNIL = 25;
	constexpr std::size_t LIST_NEXT = 0, LIST_PREV = 8;

	inline bool plausible(std::uintptr_t p) {
		return p >= 0x10000 && p < 0x7fff'ffff'ffff && (p & 0xf) == 0;
	}

	inline std::uintptr_t at(std::uintptr_t node, std::size_t offset) {
		return *reinterpret_cast<std::uintptr_t const*>(node + offset);
	}

	inline bool nil(std::uintptr_t node) {
		return *reinterpret_cast<std::uint8_t const*>(node + NODE_ISNIL) != 0;
	}

	inline bool walk_tree(std::uintptr_t head, std::size_t size) {
		if (!plausible(head) || !nil(head))
			return false;
		auto root = at(head, NODE_PARENT);
		if (size == 0)
			return root == head;
		if (!plausible(root) || nil(root) || at(root, NODE_PARENT) != head)
			return false;
		auto node = at(head, NODE_LEFT);
		std::size_t seen = 0;
		while (node != head) {
			if (!plausible(node) || nil(node) || ++seen > size)
				return false;
			for (auto side : { NODE_LEFT, NODE_RIGHT }) {
				auto child = at(node, side);
				if (child == head)
					continue;
				if (!plausible(child) || nil(child) || at(child, NODE_PARENT) != node)
					return false;
			}
			auto right = at(node, NODE_RIGHT);
			if (right != head) {
				node = right;
				for (std::size_t depth = 0; at(node, NODE_LEFT) != head; ++depth) {
					if (depth > size)
						return false;
					node = at(node, NODE_LEFT);
					if (!plausible(node))
						return false;
				}
				continue;
			}
			auto parent = at(node, NODE_PARENT);
			for (std::size_t depth = 0; parent != head && node == at(parent, NODE_RIGHT); ++depth) {
				if (depth > size || !plausible(parent))
					return false;
				node = parent;
				parent = at(node, NODE_PARENT);
			}
			if (parent != head && !plausible(parent))
				return false;
			node = parent;
		}
		return seen == size && at(head, NODE_RIGHT) != 0;
	}

	inline bool walk_list(std::uintptr_t head, std::size_t size) {
		if (!plausible(head))
			return false;
		auto node = at(head, LIST_NEXT);
		std::size_t seen = 0;
		while (node != head) {
			if (!plausible(node) || ++seen > size || !plausible(at(node, LIST_NEXT)) || at(at(node, LIST_NEXT), LIST_PREV) != node)
				return false;
			node = at(node, LIST_NEXT);
		}
		return seen == size && at(head, LIST_PREV) != 0;
	}

	inline bool guarded(bool (*walk)(std::uintptr_t, std::size_t), std::uintptr_t head, std::size_t size) {
		__try {
			return walk(head, size);
		}
		__except (1) {
			return false;
		}
	}

	template <class Map>
	bool tree(Map const& map) {
		using Node = std::remove_pointer_t<decltype(map.end()._Ptr)>;
		static_assert(offsetof(Node, _Left) == NODE_LEFT && offsetof(Node, _Parent) == NODE_PARENT && offsetof(Node, _Right) == NODE_RIGHT &&
					  offsetof(Node, _Isnil) == NODE_ISNIL);
		return guarded(walk_tree, reinterpret_cast<std::uintptr_t>(map.end()._Ptr), map.size());
	}

	template <class Hash>
	bool list(Hash const& hash) {
		using Node = std::remove_pointer_t<decltype(hash.end()._Ptr)>;
		static_assert(offsetof(Node, _Next) == LIST_NEXT && offsetof(Node, _Prev) == LIST_PREV);
		return guarded(walk_list, reinterpret_cast<std::uintptr_t>(hash.end()._Ptr), hash.size());
	}
}
