#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct Vif1TraceProvenance
{
    uint32_t flatStart = 0u;
    uint32_t flatEnd = 0u;
    uint32_t sourceAddr = 0u;
    uint32_t tagAddr = 0u;
};

void registerVif1TraceProvenance(
    const uint8_t *data,
    std::vector<Vif1TraceProvenance> &&ranges);
void beginVif1TraceProvenance(const uint8_t *data);
void endVif1TraceProvenance(const uint8_t *data);
bool resolveVif1TraceProvenance(
    size_t streamOffset,
    uint32_t &sourceAddr,
    uint32_t &tagAddr);
