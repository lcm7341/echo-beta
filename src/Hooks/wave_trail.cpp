#include <Geode/Geode.hpp>
#include "Physics/moving_objects.hpp"
#if defined(GEODE_IS_WINDOWS) && defined(ECHO_THREADED_MOVE)
#include <Geode/modify/HardStreak.hpp>
#include <Geode/binding/PointNode.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

#define HARD_STREAK_UPDATE_STROKE 0x2b8ef0
#define TRAIL_EPSILON 10e-6f
#define INNER_BAND_EDGE 0.3333333
#define INNER_BAND_OPACITY 0.65
#define TRAIL_BANDS 2
#define TRIANGLE_VERTICES 3u
#define EDGES_PER_BAND 2

struct TrailCorner {
    CCPoint bisector;
    double miter;
    CCPoint direction;
};

struct TrailEdge {
    float offset;
    std::vector<CCPoint> raw, fixed;
    std::vector<unsigned char> labels;
};

struct TrailBand {
    float start, end, opacity;
    bool white, nonsolid_only;
    TrailEdge first, last;
    bool shown;
    ccColor4B color;
};

static std::vector<CCPoint> g_relative;
static std::vector<CCPoint> g_path;
static std::vector<TrailCorner> g_corners;
static TrailBand g_bands[TRAIL_BANDS] = {
    { -1.f, 1.f, 1.f, false, false },
    { (float)-INNER_BAND_EDGE, (float)INNER_BAND_EDGE, (float)INNER_BAND_OPACITY, true, true },
};
static unsigned g_triangle_counts[TRAIL_BANDS][MovingObjects::MoveThreads::PARTS];
static unsigned g_triangle_starts[TRAIL_BANDS][MovingObjects::MoveThreads::PARTS];

static bool trail_drawn_by_other_mod() {
    const char* const draw_fix_id = "nytelyte.wave_trail_draw_fix";
    const char* const draw_fix_custom_key = "_custom_configuration";
    static int other = -1;
    if (other >= 0) return other == 1;
    other = 0;
    for (auto* mod : Loader::get()->getAllMods()) {
        if (mod == Mod::get()) continue;
        bool same_draw_fix = mod->getID() == draw_fix_id && mod->getVersion() == VersionInfo(1, 2, 0) && !mod->hasSavedValue(draw_fix_custom_key);
        if (same_draw_fix) continue;
        for (auto* hook : mod->getHooks()) {
            if (hook->isEnabled() && hook->getAddress() == geode::base::get() + HARD_STREAK_UPDATE_STROKE) other = 1;
        }
    }
    return other == 1;
}

static void split(size_t total, unsigned part, size_t& from, size_t& to) {
    from = total * part / MovingObjects::MoveThreads::PARTS;
    to = total * (part + 1) / MovingObjects::MoveThreads::PARTS;
}

template <class Job>
static void run_parts(Job job) {
    MovingObjects::MoveThreads::get().run(job);
}

static CCPoint start_offset_point(CCPoint current, CCPoint next, float distance) {
    CCPoint normal = (current - next).getPerp().normalize();
    return current + normal * distance;
}

static CCPoint end_offset_point(CCPoint previous, CCPoint current, float distance) {
    CCPoint normal = (current - previous).getRPerp().normalize();
    return current + normal * distance;
}

static bool segments_cross(CCPoint first_from, CCPoint first_to, CCPoint second_from, CCPoint second_to, CCPoint& crossing) {
    if (first_from == first_to || second_from == second_to) return false;
    CCPoint first_direction = first_to - first_from;
    CCPoint second_direction = second_to - second_from;
    float cross = first_direction.cross(second_direction);
    if (std::fabs(cross) < TRAIL_EPSILON) return false;
    CCPoint to_second = second_from - first_from;
    float first_t = to_second.cross(second_direction) / cross;
    float second_t = to_second.cross(first_direction) / cross;
    if (first_t < -TRAIL_EPSILON || first_t > 1 + TRAIL_EPSILON || second_t < -TRAIL_EPSILON || second_t > 1 + TRAIL_EPSILON)
        return false;
    crossing = first_from + first_direction * first_t;
    return true;
}

static void fix_edge(float offset, std::vector<CCPoint>& points, std::vector<unsigned char>& labels, const std::vector<CCPoint>& other) {
    for (size_t at = 0; at < labels.size(); at++) {
        if (labels[at]) continue;
        if (at + 1 != labels.size() && !labels[at + 1] && g_path[at + 1].getDistanceSq(g_path[at]) > g_path[at + 2].getDistanceSq(g_path[at + 1]))
            continue;
        CCPoint next_from, next_to, previous_from, previous_to, crossing;
        bool next_finished = false, previous_finished = false;
        if (at == 0) {
            next_from = points[0];
            next_to = other[0];
        } else {
            next_from = points[at - 1];
            next_to = end_offset_point(g_path[at - 1], g_path[at], offset);
        }
        if (at + 2 >= points.size()) {
            previous_from = points.back();
            previous_to = other.back();
        } else {
            previous_from = points[at + 2];
            previous_to = start_offset_point(g_path[at + 1], g_path[at + 2], offset);
        }
        for (size_t step = 2; step < points.size() + 1; step++) {
            if (at + step >= points.size() && !next_finished) {
                next_finished = true;
                if (segments_cross(points.back(), other.back(), next_from, next_to, crossing)) {
                    for (size_t index = at; index < points.size(); index++) points[index] = crossing;
                    for (size_t index = at; index < labels.size(); index++) labels[index] = true;
                    break;
                }
            }
            if (at + 1 < step && !previous_finished) {
                previous_finished = true;
                if (segments_cross(points[0], other[0], previous_from, previous_to, crossing)) {
                    for (size_t index = 0; index < at + 2; index++) points[index] = crossing;
                    for (size_t index = 0; index < at + 1; index++) labels[index] = true;
                    break;
                }
            }
            if (!next_finished && segments_cross(points[at - 1 + step], points[at + step], next_from, next_to, crossing)) {
                for (size_t index = at; index < at + step; index++) points[index] = crossing;
                for (size_t index = at; index < at + step - 1; index++) labels[index] = true;
                break;
            }
            if (!previous_finished && segments_cross(points[at + 1 - step], points[at + 2 - step], previous_from, previous_to, crossing)) {
                for (size_t index = at + 2 - step; index < at + 2; index++) points[index] = crossing;
                for (size_t index = at + 2 - step; index < at + 1; index++) labels[index] = true;
                break;
            }
            if (next_finished && previous_finished) break;
        }
    }
}

static bool read_path(HardStreak* streak) {
    auto* nodes = streak->m_pointArray->data;
    size_t usable = nodes->num;
    for (size_t at = 0; at < nodes->num; ++at) {
        if (nodes->arr[at]) continue;
        usable = at;
        break;
    }
    CCPoint position = streak->getPosition();
    g_relative.resize(usable);
    run_parts([&](unsigned part) {
        size_t from, to;
        split(usable, part, from, to);
        for (size_t at = from; at < to; ++at) g_relative[at] = static_cast<PointNode*>(nodes->arr[at])->m_point - position;
    });
    g_path.clear();
    for (size_t at = 0; at < usable; ++at) {
        if (!g_path.empty() && g_path.back() == g_relative[at]) continue;
        g_path.push_back(g_relative[at]);
    }
    CCPoint current = streak->m_currentPoint - position;
    if (g_path.empty() || g_path.back() != current) g_path.push_back(current);
    return g_path.size() >= 2;
}

static CCPoint extended_start(float extension) {
    return g_path[0] - (g_path[1] - g_path[0]).normalize() * extension;
}

static CCPoint extended_end(float extension) {
    CCPoint previous = g_path[g_path.size() - 2], current = g_path.back();
    float length = previous.getDistance(current);
    const int end_pullback = 5;
    return current + (current - previous).normalize() * ((length > extension + end_pullback) ? -end_pullback : extension - length);
}

static void measure_corners(unsigned part) {
    size_t count = g_path.size(), from, to;
    split(count, part, from, to);
    from = std::max<size_t>(from, 1);
    to = std::min(to, count - 1);
    for (size_t at = from; at < to; ++at) {
        CCPoint current = g_path[at];
        CCPoint before = (current - g_path[at - 1]).getRPerp().normalize();
        CCPoint after = (current - g_path[at + 1]).getPerp().normalize();
        g_corners[at].bisector = (before + after).normalize();
        g_corners[at].miter = sqrt(0.5 + 0.5 * before.dot(after));
        g_corners[at].direction = (current - g_path[at - 1]).normalize();
    }
}

static void place_edge(TrailEdge& edge, float extension, unsigned part) {
    size_t count = g_path.size(), from, to;
    split(count, part, from, to);
    for (size_t at = from; at < to; ++at) {
        if (at == 0)
            edge.raw[0] = start_offset_point(extended_start(extension), g_path[1], edge.offset);
        else if (at == count - 1)
            edge.raw[at] = end_offset_point(g_path[at - 1], extended_end(extension), edge.offset);
        else {
            float length = edge.offset / g_corners[at].miter;
            edge.raw[at] = g_path[at] + g_corners[at].bisector * length;
        }
    }
}

static void label_edge(TrailEdge& edge, float extension, unsigned part) {
    size_t count = g_path.size(), from, to;
    split(count, part, from, to);
    from = std::max<size_t>(from, 1);
    for (size_t at = from; at < to; ++at) {
        CCPoint direction = at == count - 1 ? (extended_end(extension) - g_path[at - 1]).normalize() : g_corners[at].direction;
        CCPoint trail = edge.raw[at] - edge.raw[at - 1];
        if (trail.isZero()) edge.labels[at - 1] = true;
        else edge.labels[at - 1] = !(direction.dot(trail.normalize()) < -1.0f + TRAIL_EPSILON);
    }
}

static unsigned count_triangles(const TrailBand& band, size_t from, size_t to) {
    unsigned triangles = 0;
    const auto& previous = band.first.fixed;
    const auto& next = band.last.fixed;
    for (size_t at = from; at < to; ++at) {
        if (previous[at] != previous[at + 1]) ++triangles;
        if (next[at] != next[at + 1]) ++triangles;
    }
    return triangles;
}

static void write_triangles(const TrailBand& band, size_t from, size_t to, ccV2F_C4B_T2F* out) {
    const auto& previous = band.first.fixed;
    const auto& next = band.last.fixed;
    ccColor4B color = band.color;
    auto put = [&](const CCPoint& point) { *out++ = { { point.x, point.y }, color, { 0.f, 0.f } }; };
    for (size_t at = from; at < to; ++at) {
        if (previous[at] != previous[at + 1]) {
            put(previous[at]);
            put(previous[at + 1]);
            put(next[at + 1]);
        }
        if (next[at] != next[at + 1]) {
            put(next[at + 1]);
            put(next[at]);
            put(previous[at]);
        }
    }
}

static unsigned grown_capacity(unsigned capacity, unsigned vertices) {
    unsigned filled = 0;
    while (true) {
        unsigned trigger = capacity > TRIANGLE_VERTICES ? (capacity - TRIANGLE_VERTICES + 2) / TRIANGLE_VERTICES * TRIANGLE_VERTICES : 0;
        trigger = std::max(trigger, filled);
        if (trigger >= vertices) return capacity;
        capacity += std::max(capacity, TRIANGLE_VERTICES);
        filled = trigger + TRIANGLE_VERTICES;
    }
}

class $modify(EchoWaveTrail, HardStreak) {
    void draw_fixed_trail() {
        size_t count = g_path.size();
        const int wave_width_factor = 3;
        const float max_opacity = 255.f;
        float relative_factor = wave_width_factor * m_waveSize * m_pulseSize;
        g_corners.resize(count);
        for (auto& band : g_bands) {
            band.shown = !band.nonsolid_only || !m_isSolid;
            band.first.offset = m_isFlipped ? (0.f + band.end * relative_factor) : (0.f + band.start * relative_factor);
            band.last.offset = m_isFlipped ? (0.f + band.start * relative_factor) : (0.f + band.end * relative_factor);
            band.color = to4B(band.white ? ccColor3B{ 255, 255, 255 } : getColor(), std::clamp(band.opacity * getOpacity(), 0.f, max_opacity));
            for (auto* edge : { &band.first, &band.last }) {
                edge->raw.resize(count);
                edge->labels.resize(count - 1);
            }
        }
        run_parts([&](unsigned part) {
            measure_corners(part);
        });
        run_parts([&](unsigned part) {
            for (auto& band : g_bands) {
                if (!band.shown) continue;
                place_edge(band.first, relative_factor, part);
                place_edge(band.last, relative_factor, part);
            }
        });
        run_parts([&](unsigned part) {
            for (auto& band : g_bands) {
                if (!band.shown) continue;
                label_edge(band.first, relative_factor, part);
                label_edge(band.last, relative_factor, part);
            }
        });
        run_parts([&](unsigned part) {
            if (part >= TRAIL_BANDS * EDGES_PER_BAND) return;
            auto& band = g_bands[part / EDGES_PER_BAND];
            if (!band.shown) return;
            auto& edge = part % EDGES_PER_BAND == 0 ? band.first : band.last;
            const auto& other = part % EDGES_PER_BAND == 0 ? band.last.raw : band.first.raw;
            edge.fixed = edge.raw;
            fix_edge(edge.offset, edge.fixed, edge.labels, other);
        });
        run_parts([&](unsigned part) {
            size_t from, to;
            split(count - 1, part, from, to);
            for (unsigned at = 0; at < TRAIL_BANDS; ++at) g_triangle_counts[at][part] = g_bands[at].shown ? count_triangles(g_bands[at], from, to) : 0;
        });
        unsigned vertices = 0;
        for (unsigned band = 0; band < TRAIL_BANDS; ++band) {
            for (unsigned part = 0; part < MovingObjects::MoveThreads::PARTS; ++part) {
                g_triangle_starts[band][part] = vertices;
                vertices += g_triangle_counts[band][part] * TRIANGLE_VERTICES;
            }
        }
        unsigned capacity = grown_capacity(m_uBufferCapacity, vertices);
        if (capacity != m_uBufferCapacity) {
            m_uBufferCapacity = capacity;
            m_pBuffer = static_cast<ccV2F_C4B_T2F*>(realloc(m_pBuffer, m_uBufferCapacity * sizeof(ccV2F_C4B_T2F)));
        }
        run_parts([&](unsigned part) {
            size_t from, to;
            split(count - 1, part, from, to);
            for (unsigned at = 0; at < TRAIL_BANDS; ++at) {
                if (g_bands[at].shown)
                    write_triangles(g_bands[at], from, to, m_pBuffer + g_triangle_starts[at][part]);
            }
        });
        m_nBufferCount = (GLsizei)vertices;
        if (vertices > 0) m_bDirty = true;
    }

    void updateStroke(float dt) {
        if (trail_drawn_by_other_mod()) return HardStreak::updateStroke(dt);
        clear();
        if (!m_drawStreak || m_pointArray->count() == 0 || getOpacity() == 0) return;
        if (!read_path(this)) return;
        draw_fixed_trail();
    }
};
#endif
