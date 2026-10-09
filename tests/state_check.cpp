// Runs src/Logic/state_check_walk.hpp on real MSVC containers (see state_check.py).
#include "Logic/state_check_walk.hpp"
#include <windows.h>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <unordered_set>

using namespace echo::state_check::detail;

static int failures = 0;
static void expect(bool ok, char const* what) {
	std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok) ++failures;
}

int main() {
	std::map<int, double> empty;
	expect(tree(empty), "empty map");
	std::map<int, double> map;
	for (int i = 0; i < 1000; ++i) map[(i * 7919) % 1000] = i;
	expect(tree(map), "1000-node map");
	std::unordered_map<int, int> hash;
	for (int i = 0; i < 1000; ++i) hash[i * 31] = i;
	expect(list(hash), "1000-node unordered_map");
	std::unordered_set<int> set;
	expect(list(set), "empty unordered_set");
	auto copy = map;
	expect(tree(copy), "copied map");

	// The crash: the head's root pointer overwritten with 0x14e4bf.
	{
		auto* head = map.end()._Ptr;
		auto* root = head->_Parent;
		head->_Parent = reinterpret_cast<decltype(root)>(std::uintptr_t{0x14e4bf});
		expect(!tree(map), "head root overwritten with 0x14e4bf");
		head->_Parent = root;
		expect(tree(map), "restored map");
	}
	// A node zeroed after a free and reuse: a child link becomes null.
	{
		auto* root = map.end()._Ptr->_Parent;
		auto* right = root->_Right;
		root->_Right = nullptr;
		expect(!tree(map), "null child link");
		root->_Right = right;
	}
	// A wrong size (a node lost or added behind the map's back).
	{
		auto* raw = reinterpret_cast<std::size_t*>(&copy);
		raw[1] += 1;
		expect(!tree(copy), "size one more than nodes");
		raw[1] -= 1;
	}
	// An aligned but unmapped node is caught by the SEH guard instead of crashing.
	{
		auto* page = static_cast<char*>(VirtualAlloc(nullptr, 1 << 16, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		VirtualFree(page, 0, MEM_RELEASE);
		auto* root = map.end()._Ptr->_Parent;
		auto* left = root->_Left;
		root->_Left = reinterpret_cast<decltype(left)>(page);
		expect(!tree(map), "unmapped child");
		root->_Left = left;
		expect(tree(map), "restored after unmapped child");
	}
	// A list link pointing back at the wrong node.
	{
		auto* head = hash.end()._Ptr;
		auto* second = head->_Next->_Next;
		auto* prev = second->_Prev;
		second->_Prev = head;
		expect(!list(hash), "unordered_map broken prev link");
		second->_Prev = prev;
		expect(list(hash), "restored unordered_map");
	}
	std::printf("%d failures\n", failures);
	return failures ? 1 : 0;
}
