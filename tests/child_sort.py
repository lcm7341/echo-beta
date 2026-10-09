"""Compare the large-child sort with Cocos' insertion sort on (z order, order of arrival)."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = r'''
#include "Rendering/child_sort.hpp"
#include <cassert>
#include <iostream>
#include <random>
struct Node { int z; unsigned arrival; int id; };
// sortAllChildrenNoIndex, as disassembled from the game's libcocos2d.
void native(std::vector<Node*>& a) {
    for (size_t i = 1; i < a.size(); ++i) {
        Node* x = a[i];
        long j = long(i) - 1;
        while (j >= 0 && (x->z < a[j]->z || (x->z == a[j]->z && x->arrival < a[j]->arrival))) { a[j + 1] = a[j]; --j; }
        a[j + 1] = x;
    }
}
int main() {
    std::mt19937 rng(2081);
    unsigned checks = 0;
    for (int round = 0; round < 3000; ++round) {
        unsigned n = rng() % 600;
        std::vector<Node> nodes(n);
        // Mostly visited children (arrival reset to 0) in order, then fresh ones with any z,
        // a few reordered in the middle, duplicate keys and negative z.
        for (unsigned i = 0; i < n; ++i) nodes[i] = {int(rng() % 9) - 4, 0u, int(i)};
        std::vector<Node*> a(n);
        for (unsigned i = 0; i < n; ++i) a[i] = &nodes[i];
        native(a);
        unsigned fresh = n ? rng() % (n / 3 + 1) : 0;
        for (unsigned i = 0; i < fresh; ++i) { auto* x = a[n - 1 - i]; x->z = int(rng() % 9) - 4; x->arrival = rng() % 4 ? 1000 + rng() % 50 : 0; }
        for (unsigned i = 0; i < n / 20; ++i) { auto* x = a[rng() % n]; x->z = int(rng() % 9) - 4; x->arrival = 5000 + i; }
        if (round % 7 == 0) for (auto* x : a) x->arrival = rng();
        auto expected = a, actual = a;
        native(expected);
        echo::nodes::stableSortChildren(reinterpret_cast<void**>(actual.data()), unsigned(actual.size()),
            [](void* p) { return static_cast<Node*>(p)->z; }, [](void* p) { return static_cast<Node*>(p)->arrival; });
        assert(actual == expected);
        ++checks;
    }
    std::cout << checks << " child orders match the native insertion sort\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'test.cpp'; binary = Path(tmp) / 'test'
    cpp.write_text(source)
    subprocess.run(['c++', '-std=c++20', '-O2', '-I', str(root / 'src'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
