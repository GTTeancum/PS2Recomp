#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

#define PS2X_VU_NATIVE_PAIR_USE_HOST_MATH 1
#include "ps2_vu1_math.inl"
#undef PS2X_VU_NATIVE_PAIR_USE_HOST_MATH
#include "ps2_vu1_upper.cpp"
#include "ps2_vu1_lower.cpp"

template <uint32_t Lower, uint32_t Upper>
void VU1Interpreter::execPairNative(const DecodedInstructionPair &decoded,
                                    uint8_t *vuData, uint32_t dataSize,
                                    GS &gs, PS2Memory *memory)
{
    if constexpr ((Upper & 0x80000000u) != 0u)
    {
        execUpperNative<Upper>();
        float immediate = 0.0f;
        constexpr uint32_t immediateBits = Lower;
        std::memcpy(&immediate, &immediateBits, sizeof(immediate));
        m_state.i = normalizeOperand(immediate);
    }
    else if (decoded.upperVfShadowReg != 0u)
    {
        float oldVf[4]{};
        float upperVf[4]{};
        std::memcpy(oldVf, m_state.vf[decoded.upperVfShadowReg], sizeof(oldVf));
        execUpperNative<Upper>();
        std::memcpy(upperVf, m_state.vf[decoded.upperVfShadowReg], sizeof(upperVf));
        std::memcpy(m_state.vf[decoded.upperVfShadowReg], oldVf, sizeof(oldVf));
        execLowerNative<Lower>(vuData, dataSize, gs, memory, Upper);
        std::memcpy(m_state.vf[decoded.upperVfShadowReg], upperVf, sizeof(upperVf));
    }
    else
    {
        execUpperNative<Upper>();
        execLowerNative<Lower>(vuData, dataSize, gs, memory, Upper);
    }
}

struct VUNativePairAccess
{
    template <uint32_t Lower, uint32_t Upper>
    static void execute(VU1Interpreter *vu, const void *decoded,
                        uint8_t *vuData, uint32_t dataSize,
                        GS &gs, PS2Memory *memory)
    {
        const auto &pair = *static_cast<const VU1Interpreter::DecodedInstructionPair *>(decoded);
        vu->execPairNative<Lower, Upper>(pair, vuData, dataSize, gs, memory);
    }
};
