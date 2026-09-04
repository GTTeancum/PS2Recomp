#pragma once

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace RuntimeProfile
{
    enum class Phase : size_t
    {
        Scheduler, Guest, Events, Wait, Transfers, Vu, Gs,
        VuSample, VuDecode, VuHazard, VuUpper, VuLower, VuRetire, VuWriteback, Count
    };
    struct Totals
    {
        uint64_t calls = 0u;
        uint64_t inclusiveNs = 0u;
        uint64_t exclusiveNs = 0u;
    };
    class Scope;
    struct State
    {
        Scope *active = nullptr;
        std::array<Totals, static_cast<size_t>(Phase::Count)> totals{};
        std::chrono::steady_clock::time_point windowStart{};
        uint64_t tick = 0u;
    };
    inline thread_local State state{};
    inline thread_local bool sampleVu = false;

    inline bool enabled()
    {
        static const bool value = std::getenv("PS2X_RUNTIME_PHASE_PROFILE") != nullptr;
        return value;
    }

    class VuDetailSample
    {
    public:
        VuDetailSample() noexcept
        {
            static const bool requested = std::getenv("PS2X_VU_DETAIL_PROFILE") != nullptr;
            if (!requested || !enabled())
                return;
            m_enabled = true;
            m_previous = sampleVu;
            // Sample complete slices without synchronizing to microprogram loop lengths.
            static thread_local uint32_t random = 0x93A76C51u;
            random ^= random << 13u;
            random ^= random >> 17u;
            random ^= random << 5u;
            sampleVu = state.tick >= 1000u && (random & 63u) == 0u;
        }
        ~VuDetailSample()
        {
            if (m_enabled)
                sampleVu = m_previous;
        }
        VuDetailSample(const VuDetailSample &) = delete;
        VuDetailSample &operator=(const VuDetailSample &) = delete;

    private:
        bool m_enabled = false;
        bool m_previous = false;
    };

    // Nested stages are subtracted from their parents, including exception unwinds.
    class Scope
    {
    public:
        explicit Scope(Phase phase, bool active = enabled()) noexcept : m_phase(phase), m_enabled(active)
        {
            if (!m_enabled)
                return;
            m_start = std::chrono::steady_clock::now();
            if (state.windowStart == std::chrono::steady_clock::time_point{})
                state.windowStart = m_start;
            m_parent = state.active;
            state.active = this;
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

        ~Scope()
        {
            if (!m_enabled)
                return;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start).count());
            Totals &totals = state.totals[static_cast<size_t>(m_phase)];
            ++totals.calls;
            totals.inclusiveNs += elapsed;
            totals.exclusiveNs += elapsed - m_childNs;
            state.active = m_parent;
            if (m_parent)
                m_parent->m_childNs += elapsed;
            else if (now - state.windowStart >= std::chrono::seconds(1))
                report(now);
        }

    private:
        static void report(std::chrono::steady_clock::time_point now) noexcept
        {
            constexpr std::array names{
                "scheduler", "guest", "events", "wait", "transfers", "vu", "gs",
                "vu-sample", "vu-decode", "vu-hazard", "vu-upper", "vu-lower", "vu-retire", "vu-writeback"};
            static_assert(names.size() == static_cast<size_t>(Phase::Count));
            const double wallMs = std::chrono::duration<double, std::milli>(now - state.windowStart).count();
            char line[1536]{};
            size_t used = static_cast<size_t>(std::snprintf(line, sizeof(line),
                "[runtime:phase-profile] thread=%p tick=%llu wall-ms=%.3f",
                static_cast<void *>(&state), static_cast<unsigned long long>(state.tick), wallMs));
            for (size_t i = 0u; i < names.size() && used < sizeof(line); ++i)
            {
                const Totals &total = state.totals[i];
                const int written = std::snprintf(line + used, sizeof(line) - used,
                    " %s=%llu/%.3f/%.3f", names[i], static_cast<unsigned long long>(total.calls),
                    static_cast<double>(total.inclusiveNs) / 1000000.0,
                    static_cast<double>(total.exclusiveNs) / 1000000.0);
                if (written < 0)
                    break;
                used += static_cast<size_t>(written);
            }
            std::fprintf(stderr, "%s\n", line);
            state.totals = {};
            state.windowStart = now;
        }
        Phase m_phase;
        bool m_enabled;
        Scope *m_parent = nullptr;
        uint64_t m_childNs = 0u;
        std::chrono::steady_clock::time_point m_start{};
    };
}
