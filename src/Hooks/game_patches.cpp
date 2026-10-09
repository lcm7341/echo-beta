#include <Geode/Geode.hpp>
#include "game_patches.hpp"
#include "Rendering/circle_signatures.hpp"
#include <cstring>

using namespace geode::prelude;
namespace {
template <class T, size_t N>
bool matches(uintptr_t offset, std::array<T, N> const& expected) {
    return std::memcmp(reinterpret_cast<void*>(geode::base::get() + offset),
                       expected.data(), sizeof(expected)) == 0;
}
template <class T, size_t N>
auto install(uintptr_t offset, std::array<T, N> const& replacement) {
    auto* bytes = reinterpret_cast<uint8_t const*>(replacement.data());
    return Mod::get()->patch(reinterpret_cast<void*>(geode::base::get() + offset),
                            std::vector<uint8_t>(bytes, bytes + sizeof(replacement)));
}
}
$execute {
    using namespace echo::patches;
#ifdef GEODE_IS_ANDROID64
    if (matches(pointAddress, pointOriginal)) {
        if (!install(pointAddress, pointReplacement)) log::warn("Echo point-copy optimization failed");
    } else log::warn("Echo point-copy instructions differ; optimization skipped");
    if (matches(ringsAddress, ringsOriginal)) {
        if (!install(ringsAddress + 4, ringsReplacement)) log::warn("Echo empty-ring optimization failed");
    } else log::warn("Echo ring-clear instructions differ; optimization skipped");
#endif
#ifdef GEODE_IS_WINDOWS
    // Guard the lookup implementation as well: a detour could make repeated
    // lookups observable. Only skip it when the original native helper is intact.
    if (matches(eventLookupSignatureAddress, eventLookupOriginal) &&
        echo::circles::fingerprint(reinterpret_cast<uint8_t const*>(geode::base::get() + eventMapAddress),
                                  eventMapSize) == eventMapHash) {
        if (!install(eventLookupAddress, eventLookupReplacement))
            log::warn("Echo event-state lookup optimization failed");
        else log::info("Echo Windows event-state lookup optimization enabled");
    } else log::warn("Echo event-state instructions differ; optimization skipped");
    if (matches(queueAddress, queueOriginal) && matches(queueCaveAddress, queueCaveOriginal)) {
        auto cave = install(queueCaveAddress, queueCaveReplacement);
        if (!cave) log::warn("Echo queued-button optimization failed");
        else if (!install(queueAddress, queueReplacement)) {
            (void)cave.unwrap()->disable();
            log::warn("Echo queued-button optimization failed");
        }
    } else log::warn("Echo queued-button instructions differ; optimization skipped");
#endif
}
