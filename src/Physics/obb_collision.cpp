#include <Geode/Geode.hpp>
#ifndef GEODE_IS_WINDOWS
#include <Geode/modify/OBB2D.hpp>
#include "obb_collision.hpp"

using namespace geode::prelude;

class $modify(EchoBoxCollision, OBB2D) {
    void computeAxes() {
        echo::obb::compute_axes(m_corners, m_edges, m_projections);
    }

    bool overlaps1Way(OBB2D* other) {
        return echo::obb::overlaps_one_way(m_edges, m_projections, other->m_corners);
    }

    bool overlaps(OBB2D* other) {
        return echo::obb::overlaps_one_way(m_edges, m_projections, other->m_corners) &&
               echo::obb::overlaps_one_way(other->m_edges, other->m_projections, m_corners);
    }
};
#endif
