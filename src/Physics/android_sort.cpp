#include <Geode/Geode.hpp>
#ifdef GEODE_IS_ANDROID64
#include "Physics/apple_qsort.hpp"
#include "Logic/logic.hpp"
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <vector>

using namespace geode::prelude;

// gd sorts w/ the systems qsort in spiderTestJumpX/Y, sortAllGroupsX, sortGroups & createObjectsFromSetupFinished
// equal things come out in whatever order that qsort feels like, & androids is NOT apples
// DARK SHEEP: p2s spider landed 1 block lower than on mac & ios (frame 58683) bc 3 objects sit on the same spot
// so the games qsort slot (0x1228de8 in the arm64 lib off my phone) gets apples sort, only the game uses that slot
namespace echo::sort {
    // for the log
    char const* status = "not tried";
    unsigned long calls = 0;
}

namespace {
    void echo_qsort(void* base, size_t count, size_t size, int (*compare)(void const*, void const*)) {
        ++echo::sort::calls;
        if (!base || !compare || count < 2 || !size)
            return;
        // the slot is plain memory NOT a geode patch, so disable bot is honoured in here
        if (Logic::bot_disabled()) {
            qsort(base, count, size, compare);
            return;
        }
        echo::sort::apple_qsort(base, count, size, compare);
    }
}

$execute {
    auto* slot = reinterpret_cast<void**>(geode::base::get() + 0x1228de8);
    // only if the slot really is qsort rn, another lib version would have something else there
    void* current = nullptr;
    std::memcpy(&current, slot, sizeof(current));
    if (current != reinterpret_cast<void*>(&qsort)) {
        echo::sort::status = "slot is not qsort";
        return;
    }
    void* mine = reinterpret_cast<void*>(&echo_qsort);
    std::vector<uint8_t> bytes(sizeof(mine));
    std::memcpy(bytes.data(), &mine, sizeof(mine));
    // thru geode first so other mods can see it
    if (Mod::get()->patch(slot, bytes)) {
        std::memcpy(&current, slot, sizeof(current));
        if (current == mine) {
            echo::sort::status = "on";
            return;
        }
    }
    // its a table of addresses not code. if geode didnt take it open the page & write it myself
    long page = sysconf(_SC_PAGESIZE);
    auto start = reinterpret_cast<uintptr_t>(slot) & ~static_cast<uintptr_t>(page - 1);
    auto end = (reinterpret_cast<uintptr_t>(slot) + sizeof(mine) + page - 1) & ~static_cast<uintptr_t>(page - 1);
    if (page <= 0 || mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE) != 0) {
        echo::sort::status = "could not open the page";
        return;
    }
    std::memcpy(slot, bytes.data(), bytes.size());
    std::memcpy(&current, slot, sizeof(current));
    echo::sort::status = current == mine ? "on" : "wrote but slot unchanged";
}
#endif
