#pragma once

#include <cstdint>
#include <cstdlib>
#include "capstone/shared/capstone/capstone.h"

namespace flamingo {

struct Trampoline;

struct TrampolineAllocator {
    static Trampoline Allocate(std::size_t trampolineSize);
    static void Free(Trampoline const& toFree);
};

// TODO: DO NOT EXPOSE THIS SYMBOL (USE IT FOR TESTING ONLY)
csh getHandle();

}  // namespace flamingo
