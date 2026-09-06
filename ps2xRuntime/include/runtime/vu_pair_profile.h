#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <tuple>

namespace VUPairProfile
{
    using Key = std::tuple<uint32_t, uint32_t, uint32_t>; // PC, lower, upper.
    struct Counts
    {
        uint64_t native = 0;
        uint64_t interpreted = 0;
    };

    struct Collector
    {
        static constexpr size_t capacity = 4096;
        std::map<Key, Counts> pairs;
        bool overflow = false;

        void record(uint32_t pc, uint32_t lower, uint32_t upper, bool native)
        {
            const Key key{pc, lower, upper};
            auto found = pairs.find(key);
            if (found == pairs.end())
            {
                if (pairs.size() == capacity)
                {
                    overflow = true;
                    return;
                }
                found = pairs.emplace(key, Counts{}).first;
            }
            ++(native ? found->second.native : found->second.interpreted);
        }
    };

    inline thread_local Collector *active = nullptr;
    inline thread_local const void *owner = nullptr;

    // A replay cold pass observes real dispatch, without shrinking its cycle budget.
    class Scope
    {
    public:
        Scope(Collector *collector, const void *vu) : previous(active), previousOwner(owner)
        {
            active = collector;
            owner = vu;
        }
        ~Scope() { active = previous; owner = previousOwner; }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;

    private:
        Collector *previous;
        const void *previousOwner;
    };

    inline void record(const void *vu, uint32_t pc, uint32_t lower, uint32_t upper, bool native)
    {
        if (active && owner == vu)
            active->record(pc, lower, upper, native);
    }
}
