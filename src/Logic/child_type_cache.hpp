#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// caches what type a child is NOT whos a child
template <class Object>
class ChildTypeCache {
	struct Entry {
		Object* object = nullptr;
		unsigned lifetime = 0;
		bool matches = false;
	};

	std::vector<Entry> entries;

public:
	template <class Children, class Identity, class Match>
	bool contains(const Children& children, Identity&& identity, Match&& match) {
		if (entries.size() != children.size())
			entries.resize(children.size());
		for (size_t i = 0; i < children.size(); ++i) {
			auto* object = children[i];
			auto lifetime = object ? identity(object) : 0;
			auto& entry = entries[i];
			if (entry.object != object || entry.lifetime != lifetime) {
				entry = { object, lifetime, object && match(object) };
			}
			if (entry.matches)
				return true;
		}
		return false;
	}
};
