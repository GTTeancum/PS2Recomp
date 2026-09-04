#include "runtime/ps2_vu1_native.h"
#include "ps2_vu1_detail.h"
#include <cmath>
#include <limits>

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

// Small generated translation units specialize the original arithmetic definitions.
#include "ps2_vu1_math.inl"
#include "ps2_vu1_upper.cpp"

struct VUNativeAccess
{
    template <uint32_t Word> static void execute(VU1Interpreter *vu) { vu->execUpperNative<Word>(); }
};
