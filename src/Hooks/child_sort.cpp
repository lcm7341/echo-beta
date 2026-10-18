#include <Geode/Geode.hpp>
#include <Geode/modify/CCNode.hpp>
#include <algorithm>
#include <vector>

using namespace geode::prelude;

struct ChildOrder {
    int z_order;
    unsigned arrival;
    CCObject* child;
};

static std::vector<ChildOrder> g_child_orders;

static bool comes_before(const ChildOrder& left, const ChildOrder& right) {
    if (left.z_order != right.z_order) return left.z_order < right.z_order;
    return left.arrival < right.arrival;
}

class $modify(EchoChildSort, CCNode) {
    void sortAllChildren() {
        const unsigned min_fast_sort_children = 1024;
        if (!m_bReorderChildDirty || m_bUseChildIndex || !m_pChildren || m_pChildren->count() < min_fast_sort_children) {
            CCNode::sortAllChildren();
            return;
        }
        auto* children = m_pChildren->data;
        g_child_orders.clear();
        for (unsigned at = 0; at < children->num; ++at) {
            auto* child = static_cast<CCNode*>(children->arr[at]);
            g_child_orders.push_back({ child->getZOrder(), child->getOrderOfArrival(), children->arr[at] });
        }
        if (!std::is_sorted(g_child_orders.begin(), g_child_orders.end(), comes_before)) {
            std::stable_sort(g_child_orders.begin(), g_child_orders.end(), comes_before);
            for (unsigned at = 0; at < children->num; ++at)
                children->arr[at] = g_child_orders[at].child;
        }
        m_bReorderChildDirty = false;
    }
};
