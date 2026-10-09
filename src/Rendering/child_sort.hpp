#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace echo::nodes {
// Cocos' sortAllChildrenNoIndex is an insertion sort on (z order, order of arrival): a child moves
// left while its z is lower, or equal with a lower (unsigned) arrival. Insertion sort is stable,
// so the result is the stable sort of the current array by that key. With hundreds of thousands
// of children and thousands added per frame, each new child walked past every later-z sibling.
// This reads each key once, keeps the already-ordered prefix, stable-sorts the rest and merges
// it in: the same order, in about linear time.
struct ChildKey {
    int z;
    unsigned arrival;
    void* node;
};
inline bool before(ChildKey const& a, ChildKey const& b) {
    return a.z < b.z || (a.z == b.z && a.arrival < b.arrival);
}
// Returns true when the array was out of order and has been rewritten.
template<class ZOrder, class Arrival>
bool stableSortChildren(void** nodes, unsigned count, ZOrder zOrder, Arrival arrival) {
    if (count < 2) return false;
    static std::vector<ChildKey> keys;
    keys.resize(count);
    for (unsigned i = 0; i < count; ++i) keys[i] = {zOrder(nodes[i]), arrival(nodes[i]), nodes[i]};
    unsigned sorted = 1;
    while (sorted < count && !before(keys[sorted], keys[sorted - 1])) ++sorted;
    if (sorted == count) return false;
    std::stable_sort(keys.begin() + sorted, keys.end(), before);
    std::inplace_merge(keys.begin(), keys.begin() + sorted, keys.end(), before);
    for (unsigned i = 0; i < count; ++i) nodes[i] = keys[i].node;
    return true;
}
}
