#include <Geode/Geode.hpp>
#include "Physics/apple_qsort.hpp"
#include "Logic/logic.hpp"
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

// makes gd on android sort w/ apple's qsort instead of android's, see apple_qsort.hpp for why.
//
// gd calls qsort in spiderTestJumpX/Y, sortAllGroupsX, sortGroups & createObjectsFromSetupFinished. it doesnt
// call it directly tho, it goes thru a slot in a table of addresses that gets filled in when the game loads.
// so if i put my_qsort's address in that slot every qsort the game does comes to me, & nothing else on the
// phone changes since only gd uses its own slot. its not a geode patch so disable bot doesnt undo it, my_qsort
// just hands it back to the real qsort while the bot is off.
//
// 0x1228de8 is from the arm64 lib off my phone so its only right for that gd version. it only writes there if
// the slot really holds qsort rn, if it doesnt i'm probably on another version & it does nothing. tries thru
// geode 1st, if that doesnt stick it opens the page & writes it by hand. not sure the by hand part ever
// actually runs but its there. status & calls go in the step trace so i can see if it turned on

using namespace geode::prelude;

namespace echo::sort {
    char const* status = "not tried";
    unsigned long calls = 0;
}

static void my_qsort(void* array, size_t count, size_t item_size, int (*compare)(void const*, void const*)) {
    echo::sort::calls++;
    if (array == nullptr || compare == nullptr || count < 2 || item_size == 0)
        return;
    if (Logic::bot_disabled())
        qsort(array, count, item_size, compare);
    else
        echo::sort::apple_qsort(array, count, item_size, compare);
}

static void* read_slot(void** slot) {
    void* value;
    std::memcpy(&value, slot, sizeof(value));
    return value;
}

$execute {
    void** slot = (void**)(geode::base::get() + 0x1228de8);
    void* mine = (void*)&my_qsort;

    if (read_slot(slot) != (void*)&qsort) {
        echo::sort::status = "slot is not qsort";
        return;
    }

    geode::ByteVector bytes(sizeof(mine));
    std::memcpy(bytes.data(), &mine, sizeof(mine));
    if (Mod::get()->patch(slot, bytes) && read_slot(slot) == mine) {
        echo::sort::status = "on";
        return;
    }

    long page_size = sysconf(_SC_PAGESIZE);
    uintptr_t page_start = (uintptr_t)slot & ~(uintptr_t)(page_size - 1);
    uintptr_t page_end = ((uintptr_t)slot + sizeof(mine) + page_size - 1) & ~(uintptr_t)(page_size - 1);
    if (page_size <= 0 || mprotect((void*)page_start, page_end - page_start, PROT_READ | PROT_WRITE) != 0) {
        echo::sort::status = "could not open the page";
        return;
    }
    std::memcpy(slot, &mine, sizeof(mine));
    echo::sort::status = read_slot(slot) == mine ? "on" : "wrote but slot unchanged";
}
