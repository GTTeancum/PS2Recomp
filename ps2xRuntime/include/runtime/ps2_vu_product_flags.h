#pragma once

#if defined(_MSC_VER) && defined(_M_X64) && defined(__AVX2__)
#define PS2X_VU_AVX2_PRODUCT_FLAGS 1

#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <limits>

namespace VUFlags
{
    inline void normalizeExactAvx2(__m256d exact, float *value, uint8_t *laneFlags, uint8_t dest)
    {
        static_assert(sizeof(long double) == sizeof(double));
        const __m256d magnitude = _mm256_andnot_pd(_mm256_set1_pd(-0.0), exact);
        const __m256d zero = _mm256_cmp_pd(magnitude, _mm256_setzero_pd(), _CMP_EQ_OQ);
        const __m256d small = _mm256_cmp_pd(magnitude,
            _mm256_set1_pd(std::numeric_limits<float>::min()), _CMP_LT_OQ);
        const __m256d overflow = _mm256_cmp_pd(magnitude,
            _mm256_set1_pd(std::numeric_limits<float>::max()), _CMP_GT_OQ);
        const auto narrowMask = [](__m256d mask)
        {
            const __m256i words = _mm256_permutevar8x32_epi32(_mm256_castpd_si256(mask),
                _mm256_setr_epi32(0, 2, 4, 6, 0, 2, 4, 6));
            return _mm256_castsi256_si128(words);
        };
        const __m128i smallMask = narrowMask(small);
        const __m128i overflowMask = narrowMask(overflow);
        // Keep the double sign without a float conversion that could overflow.
        // Normal results retain the instruction's existing float rounding.
        const __m128i high = _mm256_castsi256_si128(_mm256_permutevar8x32_epi32(
            _mm256_castpd_si256(exact), _mm256_setr_epi32(1, 3, 5, 7, 1, 3, 5, 7)));
        const __m128i sign = _mm_and_si128(high, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i active = _mm_setr_epi32(-static_cast<int>((dest >> 3u) & 1u),
            -static_cast<int>((dest >> 2u) & 1u), -static_cast<int>((dest >> 1u) & 1u),
            -static_cast<int>(dest & 1u));
        const __m128i replace = _mm_and_si128(active, _mm_or_si128(smallMask, overflowMask));
        const __m128i saturated = _mm_or_si128(sign,
            _mm_and_si128(overflowMask, _mm_set1_epi32(0x7F7FFFFF)));
        _mm_storeu_ps(value, _mm_castsi128_ps(_mm_blendv_epi8(
            _mm_castps_si128(_mm_loadu_ps(value)), saturated, replace)));

        const __m128i underflow = _mm_andnot_si128(narrowMask(zero), smallMask);
        const __m128i zeroSign = _mm_or_si128(_mm_srli_epi32(sign, 30),
            _mm_and_si128(smallMask, _mm_set1_epi32(1)));
        const __m128i range = _mm_or_si128(_mm_and_si128(underflow, _mm_set1_epi32(4)),
            _mm_and_si128(overflowMask, _mm_set1_epi32(8)));
        const __m128i flags = _mm_and_si128(active, _mm_or_si128(zeroSign, range));
        const __m128i shorts = _mm_packus_epi32(flags, flags);
        const uint32_t bytes = static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(shorts, shorts)));
        std::memcpy(laneFlags, &bytes, sizeof(bytes));
    }

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
