#pragma once

#include <cstdint>

namespace GSColorFilter
{
    // Interpolate RGBA8 colors for fractional coordinates in [0, 1].
    uint32_t bilinearScalar(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11,
                            float fx, float fy);
    uint32_t bilinearPacked(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11,
                            float fx, float fy);
}
