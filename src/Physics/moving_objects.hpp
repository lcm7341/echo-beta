#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <initializer_list>
#include <thread>
#include <vector>
#include <limits>
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#include <xmmintrin.h>
#endif

namespace echo::movement {
void prepare();
template<class T> inline T read(void const* object, size_t offset) {
    T value; std::memcpy(&value, static_cast<char const*>(object) + offset, sizeof(T)); return value;
}
template<class T> inline void write(void* object, size_t offset, T value) {
    std::memcpy(static_cast<char*>(object) + offset, &value, sizeof(T));
}
// Native Windows 2.2081 section conversion. Above 1e7, native multiplies
// in FLOAT precision; below it, in DOUBLE precision. Preserve both branches.
inline bool section(double position, float factor, int& result) {
    if (!std::isfinite(position) || !std::isfinite(factor)) return false;
    double scaled = position <= 0. ? 0. : position >= 10000000. ? double(factor * 10000000.f) : double(factor) * position;
    if (!std::isfinite(scaled) || scaled < std::numeric_limits<int>::min() || scaled > std::numeric_limits<int>::max()) return false;
    result = static_cast<int>(scaled);
    return true;
}
// px/py are the object's current position at 0x3b8/0x3c0. The move loop passes the values
// it just stored: reloading them as one 16-byte read right after two 8-byte stores cannot be
// store-forwarded and stalls until both stores reach the cache.
inline bool sectionUnchanged(void const* layer, void const* object, double px, double py) {
    int oldX = read<int>(object, 0x278);
    if (oldX < 0) return true; // Native returns before reading position.
#if defined(__SSE2__) || defined(_M_X64)
    // X and Y use independent IEEE double operations, with the native
    // multiplication and truncation rounding. SIMD changes no simulation time.
    __m128d position = _mm_set_pd(py, px);
    __m128d factors = _mm_cvtps_pd(_mm_castsi128_ps(_mm_cvtsi64_si128(read<int64_t>(layer, 0x36a0))));
    __m128d validPosition = _mm_and_pd(_mm_cmpge_pd(position, _mm_set1_pd(-std::numeric_limits<double>::max())),
                                      _mm_cmplt_pd(position, _mm_set1_pd(10000000.)));
    __m128d validFactor = _mm_and_pd(_mm_cmpge_pd(factors, _mm_setzero_pd()),
                                    _mm_cmple_pd(factors, _mm_set1_pd(200.)));
    if (_mm_movemask_pd(_mm_and_pd(validPosition, validFactor)) == 3) {
        __m128i cell = _mm_cvttpd_epi32(_mm_mul_pd(_mm_max_pd(position, _mm_setzero_pd()), factors));
        __m128i oldCell = _mm_cvtsi64_si128(read<int64_t>(object, 0x278));
        return (_mm_movemask_epi8(_mm_cmpeq_epi32(cell, oldCell)) & 0xff) == 0xff;
    }
#else
    float fx = read<float>(layer, 0x36a0), fy = read<float>(layer, 0x36a4);
    if (px >= 0. && px < 10000000. && py >= 0. && py < 10000000. &&
        fx >= 0.f && fx <= 200.f && fy >= 0.f && fy <= 200.f) {
        return static_cast<int>(double(fx) * px) == oldX &&
               static_cast<int>(double(fy) * py) == read<int>(object, 0x27c);
    }
#endif
    int x, y;
    return section(px, read<float>(layer, 0x36a0), x) &&
           section(py, read<float>(layer, 0x36a4), y) &&
           x == oldX && y == read<int>(object, 0x27c);
}
inline bool sectionUnchanged(void const* layer, void const* object) {
    return sectionUnchanged(layer, object, read<double>(object, 0x3b8), read<double>(object, 0x3c0));
}
// Group members are scattered over the heap and each one's fields span about seven cache lines,
// so the loop mostly waits on memory. The pointer array says which objects come next; asking for
// their lines a few objects early overlaps those misses. Prefetches change no values.
inline void prefetchObject(void const* object) {
#if defined(__SSE2__) || defined(_M_X64)
    auto* p = static_cast<char const*>(object);
    for (size_t offset : {0x278, 0x27f, 0x2c8, 0x351, 0x369, 0x3b8, 0x3c7, 0x4d0, 0x4df, 0x512})
        _mm_prefetch(p + offset, _MM_HINT_T0);
#else
    (void)object;
#endif
}
constexpr unsigned kPrefetchAhead = 8;

// One member's whole update, in native order. Returns whether its section changed; the caller
// runs that transition. Touches only this object's own fields and reads the layer.
inline bool moveOne(void* layer, void* object, double dx, double dy, bool moveX, bool moveY) {
    double x = read<double>(object, 0x3b8), y = read<double>(object, 0x3c0);
    if (!read<bool>(object, 0x512) && read<int>(object, 0x4dc) != read<int>(layer, 0x3e0)) {
        write(object, 0x4d0, static_cast<float>(x));
        write(object, 0x4d4, static_cast<float>(y));
        write(object, 0x4dc, read<int>(layer, 0x3e0));
        write(object, 0x368, true); write(object, 0x369, true);
    }
    if (moveX && !read<bool>(object, 0x2c8)) write(object, 0x3b8, x += dx);
    if (moveY) write(object, 0x3c0, y += dy);
    write(object, 0x351, true); write(object, 0x352, true);
    return !sectionUnchanged(layer, object, x, y);
}

#if defined(__SSE2__) || defined(_M_X64)
#define ECHO_PARALLEL_MOVE 1
// Helper threads for large groups. At high TPS one group of thousands of objects moves every
// tick and the loop waits on memory. Helpers spin briefly between jobs (a move arrives every few microseconds while
// physics is behind) and then sleep. The pool is never destroyed: joining threads while the
// DLL unloads would deadlock, and the process ends them.
class MovePool {
public:
    static constexpr unsigned kHelpers = 5;
    static MovePool& get() {
        static MovePool* pool = new MovePool();
        return *pool;
    }
    unsigned parts() const { return kHelpers + 1; }
    // Runs job(part) for every part, part 0 on the calling thread, and returns once all finish.
    template<class Job> void run(Job& job) {
        fn = [](void* context, unsigned part) { (*static_cast<Job*>(context))(part); };
        context = &job;
        csr = _mm_getcsr();
        pending.store(kHelpers, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_seq_cst);
        if (sleepers.load(std::memory_order_seq_cst) > 0) generation.notify_all();
        job(0u);
        while (pending.load(std::memory_order_acquire) != 0) _mm_pause();
    }
private:
    MovePool() {
        for (unsigned i = 0; i < kHelpers; ++i) std::thread([this, i] { work(i + 1); }).detach();
    }
    void work(unsigned part) {
        uint32_t seen = 0; // The pool starts at generation 0, before any job is posted.
        for (;;) {
            auto spinStart = std::chrono::steady_clock::now();
            for (unsigned spins = 0; generation.load(std::memory_order_acquire) == seen; ++spins) {
                _mm_pause();
                if ((spins & 255) != 255) continue;
                if (std::chrono::steady_clock::now() - spinStart < std::chrono::milliseconds(2)) continue;
                sleepers.fetch_add(1, std::memory_order_seq_cst);
                if (generation.load(std::memory_order_seq_cst) == seen) generation.wait(seen, std::memory_order_seq_cst);
                sleepers.fetch_sub(1, std::memory_order_seq_cst);
                spinStart = std::chrono::steady_clock::now();
            }
            seen = generation.load(std::memory_order_acquire);
            // The main thread's rounding and denormal modes, so every slice does the same math.
            if (_mm_getcsr() != csr) _mm_setcsr(csr);
            fn(context, part);
            pending.fetch_sub(1, std::memory_order_release);
        }
    }
    void (*fn)(void*, unsigned) = nullptr;
    void* context = nullptr;
    unsigned csr = 0;
    std::atomic<uint32_t> generation{0};
    std::atomic<unsigned> pending{0};
    std::atomic<unsigned> sleepers{0};
};

// Splitting a group across threads is exact only when no object is listed twice (native moves
// a duplicate twice, in order) and no entry is null (native stops at the first). Only
// addToGroup appends to a group array (PlayLayer's version without a duplicate check), so
// removals and reordering cannot break a verified array. groupEpoch counts addToGroup calls; a
// verified array stays trusted while it has not changed and has not grown.
inline std::uint64_t groupEpoch = 1;
struct GroupCheck { void* array; std::uint64_t epoch; unsigned size; bool clean; };
inline GroupCheck groupChecks[64]{};
// Members to move in parallel, or 0 to move sequentially.
inline unsigned verifiedMembers(void* array, void* const* objects, unsigned size) {
    auto& entry = groupChecks[(reinterpret_cast<uintptr_t>(array) >> 4) & 63];
    if (entry.array == array && entry.epoch == groupEpoch && size <= entry.size) return entry.clean ? size : 0;
    unsigned live = 0;
    while (live < size && objects[live]) ++live;
    std::vector<void*> sorted(objects, objects + live);
    std::sort(sorted.begin(), sorted.end());
    bool unique = std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
    entry = {array, groupEpoch, size, unique && live == size};
    return unique ? live : 0;
}

#endif

inline unsigned parallelMinimum = 512;

// This loop is enabled only for the fingerprinted Windows routines and the
// native PlayLayer section implementation. All movement arithmetic stays scalar
// double, in native order. Callers retain section transitions and final callback.
template<class Count, class Data, class UpdateSection, class Finish>
void move(void* layer, void* array, double dx, double dy,
          Count count, Data data, UpdateSection updateSection, Finish finish) {
    unsigned total = count(array);
    write(layer, 0x3724, read<uint32_t>(layer, 0x3724) + total);
    auto [objects, size] = data(array);
    bool moveX = dx != 0., moveY = dy != 0.;
    // Native CCArray loop stops at the first null.
#ifdef ECHO_PARALLEL_MOVE
    if (unsigned live = size >= parallelMinimum ? verifiedMembers(array, reinterpret_cast<void* const*>(objects), size) : 0;
        live >= parallelMinimum) {
        // Each slice moves its own objects and marks section changes. The transitions then run
        // here in array order: they read only their own object, whose position is final for this
        // move, and the section lists that only these transitions change.
        static std::vector<uint8_t> changed;
        changed.resize(live);
        auto& pool = MovePool::get();
        // Threads take chunks from a shared cursor until the array is done, so a helper that
        // starts late or shares a core does less instead of holding everyone up.
        constexpr unsigned kChunk = 128;
        std::atomic<unsigned> cursor{0};
        auto slice = [&](unsigned) {
            for (;;) {
                unsigned begin = cursor.fetch_add(kChunk, std::memory_order_relaxed);
                if (begin >= live) return;
                unsigned end = std::min(begin + kChunk, live);
                for (unsigned i = begin; i < end && i < begin + kPrefetchAhead; ++i) prefetchObject(objects[i]);
                for (unsigned i = begin; i < end; ++i) {
                    if (i + kPrefetchAhead < end) prefetchObject(objects[i + kPrefetchAhead]);
                    changed[i] = moveOne(layer, objects[i], dx, dy, moveX, moveY);
                }
            }
        };
        pool.run(slice);
        for (unsigned i = 0; i < live; ++i)
            if (changed[i]) updateSection(objects[i]);
        finish(array);
        return;
    }
#endif
    unsigned live = 0;
    while (live < size && objects[live]) ++live;
    for (unsigned i = 0; i < live && i < kPrefetchAhead; ++i) prefetchObject(objects[i]);
    for (unsigned i = 0; i < live; ++i) {
        if (i + kPrefetchAhead < live) prefetchObject(objects[i + kPrefetchAhead]);
        if (moveOne(layer, objects[i], dx, dy, moveX, moveY)) updateSection(objects[i]);
    }
    finish(array);
}
}
