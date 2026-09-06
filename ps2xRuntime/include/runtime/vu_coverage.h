#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace VUCoverage
{
    enum Field : size_t { NativePairs, InterpretedPairs, BlockPairs, Attempts, Executions, Count };
    using Counters = std::array<uint64_t, Count>;
    struct Window
    {
        uint64_t begin;
        uint64_t end;
        Counters delta;
    };

    // Observe on the VU execution thread; never read its counters from the UI thread.
    class Sampler
    {
    public:
        std::optional<Window> observe(const void *owner, uint64_t tick, const Counters &counts)
        {
            bool reset = !m_initialized || owner != m_owner || tick < m_lastTick;
            for (size_t i = 0; i < Count; ++i)
                reset |= counts[i] < m_previous[i];
            m_owner = owner;
            m_lastTick = tick;
            m_previous = counts;
            if (reset)
            {
                m_initialized = true;
                m_begin = tick;
                m_base = counts;
                return std::nullopt;
            }
            if (tick - m_begin < 32u)
                return std::nullopt;
            Window result{m_begin, tick, {}};
            for (size_t i = 0; i < Count; ++i)
                result.delta[i] = counts[i] - m_base[i];
            m_begin = tick;
            m_base = counts;
            return result;
        }

    private:
        const void *m_owner = nullptr;
        bool m_initialized = false;
        uint64_t m_begin = 0;
        uint64_t m_lastTick = 0;
        Counters m_base{};
        Counters m_previous{};
    };
}
