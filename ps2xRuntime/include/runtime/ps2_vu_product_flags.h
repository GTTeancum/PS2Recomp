#pragma once

#if defined(_MSC_VER) && defined(_M_X64) && defined(__AVX2__)
#define PS2X_VU_AVX2_PRODUCT_FLAGS 1

#include <cstdint>
#include <immintrin.h>
#include <limits>

namespace VUFlags
{
    inline uint32_t productStickyAvx2(__m128 left, __m128 right, uint8_t dest)
    {
        static_assert(sizeof(long double) == sizeof(double));
        // Widen before multiplication: float products would lose the range
        // information used by sticky underflow/overflow flags.
        const __m256d product = _mm256_mul_pd(_mm256_cvtps_pd(left), _mm256_cvtps_pd(right));
        const __m256d magnitude = _mm256_andnot_pd(_mm256_set1_pd(-0.0), product);
        const uint32_t zero = static_cast<uint32_t>(_mm256_movemask_pd(
            _mm256_cmp_pd(magnitude, _mm256_setzero_pd(), _CMP_EQ_OQ)));
        const uint32_t small = static_cast<uint32_t>(_mm256_movemask_pd(
            _mm256_cmp_pd(magnitude, _mm256_set1_pd(std::numeric_limits<float>::min()), _CMP_LT_OQ)));
        const uint32_t overflow = static_cast<uint32_t>(_mm256_movemask_pd(
            _mm256_cmp_pd(magnitude, _mm256_set1_pd(std::numeric_limits<float>::max()), _CMP_GT_OQ)));
        const uint32_t negative = static_cast<uint32_t>(_mm256_movemask_pd(product));
        const uint32_t lanes = ((dest & 8u) >> 3u) | ((dest & 4u) >> 1u) |
                               ((dest & 2u) << 1u) | ((dest & 1u) << 3u);
        return ((small & lanes) != 0u ? 1u : 0u) |
               ((negative & lanes) != 0u ? 2u : 0u) |
               ((small & ~zero & lanes) != 0u ? 4u : 0u) |
               ((overflow & lanes) != 0u ? 8u : 0u);
    }
}
#endif
