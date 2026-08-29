#include "runtime/ps2_vu1.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <ps2_log.h>

namespace
{
    thread_local bool xmenTraceVu1Program = false;
    thread_local uint32_t xmenCurrentVuProgramStart = UINT32_MAX;
    thread_local uint64_t xmenCurrentVuTick = 0u;
    thread_local uint32_t xmenXgkickProgramStart = UINT32_MAX;
    thread_local uint32_t xmenXgkickIssuePc = UINT32_MAX;
    thread_local uint64_t xmenXgkickIssueTick = 0u;

    struct XmenTargetXgkickConfig
    {
        bool enabled = false;
        uint64_t tagLo = 0u;
        uint64_t minTick = 0u;
        uint32_t programStart = UINT32_MAX;
        uint32_t issuePc = UINT32_MAX;
    };

    const XmenTargetXgkickConfig &xmenTargetXgkickConfig()
    {
        static const XmenTargetXgkickConfig config = []()
        {
            XmenTargetXgkickConfig result{};
            const char *tagValue = std::getenv("PS2X_TRACE_XGKICK_TAG_LO");
            if (!tagValue || tagValue[0] == '\0')
                return result;

            char *tagEnd = nullptr;
            result.tagLo = std::strtoull(tagValue, &tagEnd, 0);
            if (!tagEnd || tagEnd == tagValue || *tagEnd != '\0')
                return XmenTargetXgkickConfig{};

            const char *tickValue = std::getenv("PS2X_TRACE_XGKICK_MIN_TICK");
            if (tickValue && tickValue[0] != '\0')
            {
                char *tickEnd = nullptr;
                const uint64_t parsedTick = std::strtoull(tickValue, &tickEnd, 0);
                if (tickEnd && tickEnd != tickValue && *tickEnd == '\0')
                    result.minTick = parsedTick;
            }

            const char *programValue =
                std::getenv("PS2X_TRACE_XGKICK_PROGRAM_START");
            if (programValue && programValue[0] != '\0')
            {
                char *programEnd = nullptr;
                const unsigned long parsedProgram =
                    std::strtoul(programValue, &programEnd, 0);
                if (programEnd && programEnd != programValue &&
                    *programEnd == '\0')
                {
                    result.programStart = static_cast<uint32_t>(parsedProgram);
                }
            }

            const char *issuePcValue =
                std::getenv("PS2X_TRACE_XGKICK_ISSUE_PC");
            if (issuePcValue && issuePcValue[0] != '\0')
            {
                char *issuePcEnd = nullptr;
                const unsigned long parsedIssuePc =
                    std::strtoul(issuePcValue, &issuePcEnd, 0);
                if (issuePcEnd && issuePcEnd != issuePcValue &&
                    *issuePcEnd == '\0')
                {
                    result.issuePc = static_cast<uint32_t>(parsedIssuePc);
                }
            }
            result.enabled = true;
            return result;
        }();
        return config;
    }

    struct XmenXgkickCensusConfig
    {
        bool enabled = false;
        uint64_t minTick = 0u;
        uint32_t maxEntries = 256u;
    };

    const XmenXgkickCensusConfig &xmenXgkickCensusConfig()
    {
        static const XmenXgkickCensusConfig config = []()
        {
            XmenXgkickCensusConfig result{};
            const char *tickValue =
                std::getenv("PS2X_TRACE_XGKICK_CENSUS_MIN_TICK");
            if (!tickValue || tickValue[0] == '\0')
                return result;

            char *tickEnd = nullptr;
            result.minTick = std::strtoull(tickValue, &tickEnd, 0);
            if (!tickEnd || tickEnd == tickValue || *tickEnd != '\0')
                return XmenXgkickCensusConfig{};

            const char *maxValue =
                std::getenv("PS2X_TRACE_XGKICK_CENSUS_MAX");
            if (maxValue && maxValue[0] != '\0')
            {
                char *maxEnd = nullptr;
                const unsigned long parsedMax =
                    std::strtoul(maxValue, &maxEnd, 0);
                if (maxEnd && maxEnd != maxValue && *maxEnd == '\0' &&
                    parsedMax > 0u)
                {
                    result.maxEntries = static_cast<uint32_t>(
                        std::min<unsigned long>(parsedMax, 65536u));
                }
            }
            result.enabled = true;
            return result;
        }();
        return config;
    }

    struct XmenTitleVuTrace
    {
        bool active = false;
        uint64_t tick = 0u;
        uint32_t executionIndex = 0u;
        uint32_t xgkickStarts = 0u;
        uint32_t xgkickFinishes = 0u;
        uint64_t xgkickBytes = 0u;
    };

    thread_local XmenTitleVuTrace xmenTitleVuTrace{};

    struct XmenGameplayVuSummary
    {
        bool active = false;
        uint64_t tick = 0u;
        uint64_t startCycle = 0u;
        uint32_t startPc = 0u;
        uint32_t top = 0u;
        uint32_t itop = 0u;
        uint32_t slices = 0u;
        uint32_t xgkickStarts = 0u;
        uint32_t xgkickFinishes = 0u;
        uint64_t xgkickBytes = 0u;
        uint32_t largestXgkickBytes = 0u;
        uint32_t largestXgkickSource = 0u;
        uint32_t stores = 0u;
        uint32_t outputStores = 0u;
        uint32_t tagStores = 0u;
        uint32_t minStoreAddress = UINT32_MAX;
        uint32_t maxStoreAddress = 0u;
        uint64_t startTagLo = 0u;
        uint64_t endTagLo = 0u;
    };

    thread_local XmenGameplayVuSummary xmenGameplayVuSummary{};
    thread_local std::array<uint8_t, 2048> xmenVuCensusCounts{};

    void finishXmenGameplayVuSummary(const VU1State &state, uint64_t endCycle)
    {
        if (!xmenGameplayVuSummary.active)
            return;

        static uint32_t traceCount = 0u;
        if (traceCount++ < 256u)
        {
            std::fprintf(stdout,
                         "[xmen-gameplay-vu] tick=%llu start=0x%x top=0x%x itop=0x%x "
                         "cycles=%llu slices=%u end=0x%x kicks=%u/%u bytes=%llu "
                         "largest=%u@0x%x stores=%u range=0x%x..0x%x output=%u tagStores=%u "
                         "tag=%016llx->%016llx status=0x%x mac=0x%x clip=0x%x q=%g\n",
                         static_cast<unsigned long long>(xmenGameplayVuSummary.tick),
                         xmenGameplayVuSummary.startPc,
                         xmenGameplayVuSummary.top,
                         xmenGameplayVuSummary.itop,
                         static_cast<unsigned long long>(endCycle - xmenGameplayVuSummary.startCycle),
                         xmenGameplayVuSummary.slices,
                         state.pc,
                         xmenGameplayVuSummary.xgkickStarts,
                         xmenGameplayVuSummary.xgkickFinishes,
                         static_cast<unsigned long long>(xmenGameplayVuSummary.xgkickBytes),
                         xmenGameplayVuSummary.largestXgkickBytes,
                         xmenGameplayVuSummary.largestXgkickSource,
                         xmenGameplayVuSummary.stores,
                         xmenGameplayVuSummary.minStoreAddress == UINT32_MAX
                             ? 0u
                             : xmenGameplayVuSummary.minStoreAddress,
                         xmenGameplayVuSummary.maxStoreAddress,
                         xmenGameplayVuSummary.outputStores,
                         xmenGameplayVuSummary.tagStores,
                         static_cast<unsigned long long>(xmenGameplayVuSummary.startTagLo),
                         static_cast<unsigned long long>(xmenGameplayVuSummary.endTagLo),
                         state.status,
                         state.mac,
                         state.clip,
                         state.q);
            std::fflush(stdout);
        }
        xmenGameplayVuSummary = {};
    }

    constexpr uint8_t laneForComponent(uint32_t component)
    {
        return static_cast<uint8_t>(1u << (3u - component));
    }
}

void VU1Interpreter::addVfRead(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (lanes == 0u)
        return;
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
        {
            usage.vfRead[index].lanes |= lanes;
            return;
        }
    }
    if (usage.vfReadCount < usage.vfRead.size())
        usage.vfRead[usage.vfReadCount++] = {reg, lanes};
}

void VU1Interpreter::addVfWrite(InstructionUsage &usage, uint8_t reg, uint8_t lanes)
{
    if (reg == 0u || lanes == 0u)
        return;
    if (usage.vfWrite.reg == 0u)
        usage.vfWrite = {reg, lanes};
    else if (usage.vfWrite.reg == reg)
        usage.vfWrite.lanes |= lanes;
}

uint8_t VU1Interpreter::vfReadLanes(const InstructionUsage &usage, uint8_t reg)
{
    for (uint32_t index = 0; index < usage.vfReadCount; ++index)
    {
        if (usage.vfRead[index].reg == reg)
            return usage.vfRead[index].lanes;
    }
    return 0u;
}

VU1Interpreter::VU1Interpreter(Unit unit)
    : m_unit(unit)
{
    reset();
}

void VU1Interpreter::resetScheduler()
{
    m_flagPipeline = {};
    m_fdiv = {};
    m_efu = {};
    m_storePipeline = {};
    m_vfWritePipeline = {};
    m_viWritePipeline = {};
    m_accWritePipeline = {};
    m_xgkick = {};
    m_vfReady = {};
    m_viReady = {};
    m_accReady = {};
    m_vfLatestWrite = {};
    m_viLatestWrite = {};
    m_accLatestWrite = {};
    m_nextWriteSequence = 0;
    m_efuResourceReady = 0;
    m_workingClip = m_state.clip;
    m_viBranchBackupValue = 0;
    m_viBranchBackupReg = 0;
    m_viBranchBackupValid = false;
    m_stopRequested = false;
    m_pendingHaltD = false;
    m_pendingHaltT = false;
}

void VU1Interpreter::reset()
{
    std::memset(&m_state, 0, sizeof(m_state));
    m_state.vf[0][3] = 1.0f;
    m_state.q = 1.0f;
    m_state.r = 0x3F800000u;
    m_cycle = 0;
    m_running = false;
    resetScheduler();
}

float VU1Interpreter::broadcast(const float *vf, uint8_t bc)
{
    return normalizeOperand(vf[bc & 3u]);
}

float VU1Interpreter::normalizeOperand(float value) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    if (exponent == 0u)
    {
        bits &= 0x80000000u;
    }
    else if (exponent == 0xFFu)
    {
        bits = (bits & 0x80000000u) | 0x7F7FFFFFu;
    }
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float VU1Interpreter::normalizeResult(float value, uint32_t &laneFlags) const
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = bits & 0x80000000u;
    const uint32_t magnitude = bits & 0x7FFFFFFFu;
    const uint32_t exponent = (bits >> 23) & 0xFFu;

    laneFlags = sign != 0u ? 0x2u : 0u;
    if (magnitude == 0u)
    {
        laneFlags |= 0x1u;
    }
    else if (exponent == 0u)
    {
        laneFlags |= 0x5u;
        bits = sign;
    }
    else if (exponent == 0xFFu)
    {
        laneFlags |= 0x8u;
        bits = sign | 0x7F7FFFFFu;
    }

    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t VU1Interpreter::microAddressMask() const
{
    return m_unit == Unit::VU1 ? 0x3FFFu : 0x0FFFu;
}

int32_t VU1Interpreter::readBranchVi(uint8_t reg) const
{
    if (reg == 0u)
        return 0;
    if (m_viBranchBackupValid &&
        m_viBranchBackupReg == reg)
    {
        return m_viBranchBackupValue;
    }
    return m_state.vi[reg];
}

void VU1Interpreter::recordViWriteForBranch(uint8_t reg, int32_t oldValue)
{
    if (reg == 0u)
        return;
    m_viBranchBackupValue = oldValue;
    m_viBranchBackupReg = reg;
    m_viBranchBackupValid = true;
}

void VU1Interpreter::applyDest(float *dst, const float *result, uint8_t dest)
{
    if (dest & 0x8u)
        dst[0] = result[0];
    if (dest & 0x4u)
        dst[1] = result[1];
    if (dest & 0x2u)
        dst[2] = result[2];
    if (dest & 0x1u)
        dst[3] = result[3];
}

void VU1Interpreter::applyDestAcc(const float *result, uint8_t dest)
{
    applyDest(m_state.acc, result, dest);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest,
                                         uint8_t laneFlags[4])
{
    for (uint32_t component = 0; component < 4u; ++component)
    {
        laneFlags[component] = 0u;
        if ((dest & laneForComponent(component)) == 0u)
            continue;

        long double exactResult = 0.0L;
        if (calculateFmacExactResult(component, exactResult))
        {
            laneFlags[component] = normalizeFmacExactResult(result[component], exactResult);
            continue;
        }

        uint32_t flags = 0u;
        result[component] = normalizeResult(result[component], flags);
        laneFlags[component] = static_cast<uint8_t>(flags);
    }
}

bool VU1Interpreter::calculateFmacExactResult(uint32_t component,
                                               long double &result) const
{
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu
                                ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu))
                                : 0xFFu;
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);

    const auto operand = [this](float value)
    {
        return static_cast<long double>(normalizeOperand(value));
    };
    const auto vs = [&](uint32_t lane)
    {
        return operand(m_state.vf[fs][lane]);
    };
    const auto vt = [&](uint32_t lane)
    {
        return operand(m_state.vf[ft][lane]);
    };
    const auto acc = [&](uint32_t lane)
    {
        return operand(m_state.acc[lane]);
    };

    const long double q = operand(m_state.q);
    const long double i = operand(m_state.i);

    if (op < 0x3Cu)
    {
        if (op <= 0x03u)
            result = vs(component) + vt(op & 3u);
        else if (op <= 0x07u)
            result = vs(component) - vt(op & 3u);
        else if (op <= 0x0Bu)
            result = acc(component) + vs(component) * vt(op & 3u);
        else if (op <= 0x0Fu)
            result = acc(component) - vs(component) * vt(op & 3u);
        else if (op >= 0x18u && op <= 0x1Bu)
            result = vs(component) * vt(op & 3u);
        else
        {
            switch (op)
            {
            case 0x1Cu:
                result = vs(component) * q;
                break;
            case 0x1Eu:
                result = vs(component) * i;
                break;
            case 0x20u:
                result = vs(component) + q;
                break;
            case 0x21u:
                result = acc(component) + vs(component) * q;
                break;
            case 0x22u:
                result = vs(component) + i;
                break;
            case 0x23u:
                result = acc(component) + vs(component) * i;
                break;
            case 0x24u:
                result = vs(component) - q;
                break;
            case 0x25u:
                result = acc(component) - vs(component) * q;
                break;
            case 0x26u:
                result = vs(component) - i;
                break;
            case 0x27u:
                result = acc(component) - vs(component) * i;
                break;
            case 0x28u:
                result = vs(component) + vt(component);
                break;
            case 0x29u:
                result = acc(component) + vs(component) * vt(component);
                break;
            case 0x2Au:
                result = vs(component) * vt(component);
                break;
            case 0x2Cu:
                result = vs(component) - vt(component);
                break;
            case 0x2Du:
                result = acc(component) - vs(component) * vt(component);
                break;
            case 0x2Eu:
            {
                static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
                static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
                result = component == 3u
                             ? 0.0L
                             : acc(component) - vs(left[component]) * vt(right[component]);
                break;
            }
            default:
                return false;
            }
        }
        return true;
    }

    if (special <= 0x03u)
        result = vs(component) + vt(special & 3u);
    else if (special <= 0x07u)
        result = vs(component) - vt(special & 3u);
    else if (special <= 0x0Bu)
        result = acc(component) + vs(component) * vt(special & 3u);
    else if (special <= 0x0Fu)
        result = acc(component) - vs(component) * vt(special & 3u);
    else if (special >= 0x18u && special <= 0x1Bu)
        result = vs(component) * vt(special & 3u);
    else
    {
        switch (special)
        {
        case 0x1Cu:
            result = vs(component) * q;
            break;
        case 0x1Eu:
            result = vs(component) * i;
            break;
        case 0x20u:
            result = vs(component) + q;
            break;
        case 0x21u:
            result = acc(component) + vs(component) * q;
            break;
        case 0x22u:
            result = vs(component) + i;
            break;
        case 0x23u:
            result = acc(component) + vs(component) * i;
            break;
        case 0x24u:
            result = vs(component) - q;
            break;
        case 0x25u:
            result = acc(component) - vs(component) * q;
            break;
        case 0x26u:
            result = vs(component) - i;
            break;
        case 0x27u:
            result = acc(component) - vs(component) * i;
            break;
        case 0x28u:
            result = vs(component) + vt(component);
            break;
        case 0x29u:
            result = acc(component) + vs(component) * vt(component);
            break;
        case 0x2Au:
            result = vs(component) * vt(component);
            break;
        case 0x2Cu:
            result = vs(component) - vt(component);
            break;
        case 0x2Du:
            result = acc(component) - vs(component) * vt(component);
            break;
        case 0x2Eu:
        {
            static constexpr uint8_t left[4] = {1u, 2u, 0u, 3u};
            static constexpr uint8_t right[4] = {2u, 0u, 1u, 3u};
            result = component == 3u
                         ? 0.0L
                         : vs(left[component]) * vt(right[component]);
            break;
        }
        default:
            return false;
        }
    }
    return true;
}

uint8_t VU1Interpreter::normalizeFmacExactResult(float &value,
                                                  long double exactResult) const
{
    const bool negative = std::signbit(exactResult);
    const long double magnitude = std::fabs(exactResult);
    const long double maximum = static_cast<long double>(std::numeric_limits<float>::max());
    const long double minimum = static_cast<long double>(std::numeric_limits<float>::min());
    uint8_t flags = negative ? 0x2u : 0u;

    uint32_t bits = negative ? 0x80000000u : 0u;
    if (magnitude == 0.0L)
    {
        flags |= 0x1u;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude > maximum)
    {
        flags |= 0x8u;
        bits |= 0x7F7FFFFFu;
        std::memcpy(&value, &bits, sizeof(value));
    }
    else if (magnitude < minimum)
    {
        flags |= 0x5u;
        std::memcpy(&value, &bits, sizeof(value));
    }

    return flags;
}

uint32_t VU1Interpreter::calculateFmacProductSticky(uint8_t dest) const
{
    uint32_t extraSticky = 0u;
    const uint32_t upper = m_currentUpperInstruction;
    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t special = op >= 0x3Cu ? static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu)) : 0xFFu;
    const bool productSum =
        (op >= 0x08u && op <= 0x0Fu) ||
        op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
        op == 0x29u || op == 0x2Du || op == 0x2Eu ||
        (special >= 0x08u && special <= 0x0Fu) ||
        special == 0x21u || special == 0x23u || special == 0x25u ||
        special == 0x27u || special == 0x29u || special == 0x2Du;
    if (!productSum)
        return 0u;

    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((dest & laneForComponent(component)) == 0u)
            continue;
        static constexpr uint8_t crossLeft[4] = {1u, 2u, 0u, 3u};
        static constexpr uint8_t crossRight[4] = {2u, 0u, 1u, 3u};
        const uint8_t leftComponent = op == 0x2Eu ? crossLeft[component] : static_cast<uint8_t>(component);
        const float left = normalizeOperand(m_state.vf[fs][leftComponent]);
        float right = 0.0f;
        if ((op >= 0x08u && op <= 0x0Fu) || (special >= 0x08u && special <= 0x0Fu))
        {
            right = normalizeOperand(m_state.vf[ft][(op >= 0x08u && op <= 0x0Fu ? op : special) & 3u]);
        }
        else if (op == 0x21u || op == 0x25u || special == 0x21u || special == 0x25u)
        {
            right = normalizeOperand(m_state.q);
        }
        else if (op == 0x23u || op == 0x27u || special == 0x23u || special == 0x27u)
        {
            right = normalizeOperand(m_state.i);
        }
        else if (op == 0x2Eu)
        {
            right = normalizeOperand(m_state.vf[ft][crossRight[component]]);
        }
        else
        {
            right = normalizeOperand(m_state.vf[ft][component]);
        }

        float product = left * right;
        const long double exactProduct = static_cast<long double>(left) * static_cast<long double>(right);
        const uint8_t productFlags = normalizeFmacExactResult(product, exactProduct);
        // Product-sum instructions report Z/S/U/O from the add/subtract result
        // as current flags, while every product condition accumulates into the
        // corresponding sticky flag.
        extraSticky |= productFlags & 0xFu;
    }
    return extraSticky;
}

void VU1Interpreter::updateFmacFlags(const uint8_t laneFlags[4], uint8_t dest,
                                     uint32_t extraSticky)
{
    if (dest == 0u)
        return;

    uint32_t mac = 0u;
    uint32_t status = 0u;
    for (uint32_t component = 0; component < 4u; ++component)
    {
        const uint8_t lane = laneForComponent(component);
        if ((dest & lane) == 0u)
            continue;

        const uint32_t flags = laneFlags[component];
        if ((flags & 0x1u) != 0u)
            mac |= lane;
        if ((flags & 0x2u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 4;
        if ((flags & 0x4u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 8;
        if ((flags & 0x8u) != 0u)
            mac |= static_cast<uint32_t>(lane) << 12;
        status |= flags;
    }

    FlagPipelineEntry *entry = nullptr;
    for (FlagPipelineEntry &candidate : m_flagPipeline)
    {
        if (!candidate.valid)
        {
            entry = &candidate;
            break;
        }
    }
    if (!entry)
    {
        reportReservedInstruction(true, 0xFFFFFFFFu);
        return;
    }

    *entry = {};
    entry->valid = true;
    entry->issueCycle = m_cycle;
    entry->readyCycle = m_cycle + kFmacLatency;
    entry->mac = mac;
    entry->status = status;
    entry->extraSticky = extraSticky;
    entry->writesMac = true;
    entry->writesStatus = true;
}

void VU1Interpreter::applyFmacDest(float *dst, float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDest(dst, result, dest);
}

void VU1Interpreter::applyFmacDestAcc(float *result, uint8_t dest)
{
    uint8_t laneFlags[4]{};
    normalizeFmacResult(result, dest, laneFlags);
    updateFmacFlags(laneFlags, dest, calculateFmacProductSticky(dest));
    applyDestAcc(result, dest);
}

void VU1Interpreter::queueFsset(uint16_t immediate)
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesStatus = false;
    }

    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.status = static_cast<uint32_t>(immediate) & 0xFC0u;
            entry.writesSticky = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFEu);
}

void VU1Interpreter::queueClip(uint32_t clip)
{
    m_workingClip = ((m_workingClip << 6) | (clip & 0x3Fu)) & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFFDu);
}

void VU1Interpreter::queueFcset(uint32_t clip)
{
    m_workingClip = clip & 0xFFFFFFu;
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (entry.valid && entry.issueCycle == m_cycle)
            entry.writesClip = false;
    }
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid)
        {
            entry = {};
            entry.valid = true;
            entry.issueCycle = m_cycle;
            entry.readyCycle = m_cycle + kFmacLatency;
            entry.clip = m_workingClip;
            entry.writesClip = true;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFAu);
}

void VU1Interpreter::queueQ(float value, uint32_t latency, uint32_t statusDi)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    m_fdiv.valid = true;
    m_fdiv.readyCycle = m_cycle + latency;
    m_fdiv.value = value;
    m_fdiv.statusDi = statusDi & 0x30u;
}

void VU1Interpreter::queueP(float value, uint32_t latency)
{
    uint32_t ignoredFlags = 0u;
    value = normalizeResult(value, ignoredFlags);
    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (!entry.valid)
        {
            entry.valid = true;
            entry.readyCycle = m_cycle + latency;
            entry.value = value;
            // EFU throughput is one cycle shorter than result visibility.
            m_efuResourceReady = m_cycle + (latency > 0u ? latency - 1u : 0u);
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF9u);
}

void VU1Interpreter::queueStore(uint32_t address, const uint32_t words[4], uint8_t laneMask)
{
    if (xmenGameplayVuSummary.active)
    {
        ++xmenGameplayVuSummary.stores;
        xmenGameplayVuSummary.minStoreAddress =
            std::min(xmenGameplayVuSummary.minStoreAddress, address);
        xmenGameplayVuSummary.maxStoreAddress =
            std::max(xmenGameplayVuSummary.maxStoreAddress, address);
        if (address >= 0x490u && address < 0x1000u)
            ++xmenGameplayVuSummary.outputStores;
        if (address == 0x490u)
            ++xmenGameplayVuSummary.tagStores;
    }
    if (xmenTraceVu1Program)
    {
        std::fprintf(stderr,
                     "[xmen-vu1:store] cycle=%llu pc=0x%x address=0x%x mask=0x%x words=%08x,%08x,%08x,%08x\n",
                     static_cast<unsigned long long>(m_cycle), m_state.pc, address,
                     laneMask, words[0], words[1], words[2], words[3]);
    }
    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid)
        {
            store.valid = true;
            store.readyCycle = m_cycle + 1u;
            store.address = address;
            store.laneMask = laneMask;
            std::copy(words, words + 4, store.words.begin());
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFFCu);
}

void VU1Interpreter::queueVfWrite(uint8_t reg, uint8_t laneMask,
                                  const float value[4], uint32_t latency)
{
    if (reg == 0u || laneMask == 0u)
        return;
    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_vfLatestWrite[reg][component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF7u);
}

void VU1Interpreter::queueViWrite(uint8_t reg, int32_t value, uint32_t latency)
{
    if (reg == 0u)
        return;
    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.reg = reg;
            write.value = value;
            m_viLatestWrite[reg] = write.sequence;
            return;
        }
    }
    reportReservedInstruction(false, 0xFFFFFFF6u);
}

void VU1Interpreter::queueAccWrite(uint8_t laneMask, const float value[4], uint32_t latency)
{
    if (laneMask == 0u)
        return;
    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid)
        {
            write = {};
            write.valid = true;
            write.readyCycle = m_cycle + latency;
            write.sequence = ++m_nextWriteSequence;
            write.laneMask = laneMask;
            std::copy(value, value + 4, write.value.begin());
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((laneMask & laneForComponent(component)) != 0u)
                    m_accLatestWrite[component] = write.sequence;
            }
            return;
        }
    }
    reportReservedInstruction(true, 0xFFFFFFF5u);
}

void VU1Interpreter::commitReadyPipelines()
{
    for (FlagPipelineEntry &entry : m_flagPipeline)
    {
        if (!entry.valid || entry.readyCycle > m_cycle)
            continue;

        if (entry.writesMac)
            m_state.mac = entry.mac;
        if (entry.writesStatus)
        {
            const uint32_t current = entry.status & 0xFu;
            m_state.status = (m_state.status & 0xFF0u) | current | ((current | entry.extraSticky) << 6);
        }
        if (entry.writesSticky)
        {
            m_state.status = (m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
        }
        if (entry.writesClip)
            m_state.clip = entry.clip;
        entry = {};
    }

    if (m_fdiv.valid && m_fdiv.readyCycle <= m_cycle)
    {
        m_state.q = m_fdiv.value;
        const uint32_t currentDi = m_fdiv.statusDi & 0x30u;
        m_state.status = (m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
        m_fdiv = {};
    }

    for (ScalarPipelineEntry &entry : m_efu)
    {
        if (entry.valid && entry.readyCycle <= m_cycle)
        {
            m_state.p = entry.value;
            entry = {};
        }
    }

    for (PendingStore &store : m_storePipeline)
    {
        if (!store.valid || store.readyCycle > m_cycle)
            continue;
        if (m_activeVuData && store.address + 16u <= m_activeVuDataSize)
        {
            uint32_t oldWords[4]{};
            std::memcpy(oldWords, m_activeVuData + store.address, sizeof(oldWords));
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((store.laneMask & laneForComponent(component)) != 0u)
                    oldWords[component] = store.words[component];
            }
            std::memcpy(m_activeVuData + store.address, oldWords, sizeof(oldWords));
        }
        store = {};
    }

    for (PendingVfWrite &write : m_vfWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_vfLatestWrite[write.reg][component] == write.sequence)
            {
                m_state.vf[write.reg][component] = write.value[component];
            }
        }
        write = {};
    }

    for (PendingViWrite &write : m_viWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        if (m_viLatestWrite[write.reg] == write.sequence)
            m_state.vi[write.reg] = static_cast<int16_t>(write.value);
        write = {};
    }

    for (PendingAccWrite &write : m_accWritePipeline)
    {
        if (!write.valid || write.readyCycle > m_cycle)
            continue;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((write.laneMask & laneForComponent(component)) != 0u &&
                m_accLatestWrite[component] == write.sequence)
            {
                m_state.acc[component] = write.value[component];
            }
        }
        write = {};
    }
}

void VU1Interpreter::progressXgkick()
{
    if (!m_xgkick.active || !m_activeVuData || m_activeVuDataSize == 0u)
        return;

    ++m_xgkick.cycleCredit;
    while (m_xgkick.active && m_xgkick.cycleCredit >= 2u)
    {
        m_xgkick.cycleCredit -= 2u;
        if (m_xgkick.copiedBytes > XgkickPipeline::kBufferSize - 16u)
        {
            reportReservedInstruction(false, 0xFFFFFFFBu);
            m_xgkick.active = false;
            return;
        }

        const uint32_t qwordOffset = m_xgkick.copiedBytes;
        for (uint32_t i = 0; i < 16u; ++i)
        {
            const uint32_t source = (m_xgkick.sourceAddress + m_xgkick.copiedBytes + i) % m_activeVuDataSize;
            m_xgkick.packet[m_xgkick.copiedBytes + i] = m_activeVuData[source];
        }
        m_xgkick.copiedBytes += 16u;

        if (m_xgkick.currentTagEnd == 0u)
        {
            uint64_t tagLo = 0;
            std::memcpy(&tagLo, m_xgkick.packet.data() + qwordOffset, sizeof(tagLo));
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t tagBytes = 16u;
            if (format == 0u)
                tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
            else if (format == 1u)
                tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
            else if (format == GIF_FMT_IMAGE || format == GIF_FMT_IMAGE2)
                tagBytes += static_cast<uint64_t>(nloop) * 16u;
            else
            {
                reportReservedInstruction(false, 0xFFFFFFF8u);
                m_xgkick.active = false;
                return;
            }

            if (tagBytes > XgkickPipeline::kBufferSize - qwordOffset)
            {
                reportReservedInstruction(false, 0xFFFFFFFBu);
                m_xgkick.active = false;
                return;
            }
            m_xgkick.currentTagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
            m_xgkick.currentTagEop = ((tagLo >> 15) & 1u) != 0u;
            if (m_xgkick.currentTagEop)
                m_xgkick.totalBytes = m_xgkick.currentTagEnd;
        }

        if (m_xgkick.copiedBytes >= m_xgkick.currentTagEnd)
        {
            if (m_xgkick.currentTagEop)
                finishXgkick();
            else
            {
                // The next transferred qword is another GIFtag.
                m_xgkick.currentTagEnd = 0u;
                m_xgkick.currentTagEop = false;
            }
        }
    }
}

void VU1Interpreter::finishXgkick()
{
    if (!m_xgkick.active)
        return;

    const XmenTargetXgkickConfig &targetConfig = xmenTargetXgkickConfig();
    if (targetConfig.enabled && m_xgkick.totalBytes >= 16u &&
        xmenXgkickIssueTick >= targetConfig.minTick)
    {
        uint64_t targetTagLo = 0u;
        uint64_t targetTagHi = 0u;
        std::memcpy(&targetTagLo, m_xgkick.packet.data(), sizeof(targetTagLo));
        std::memcpy(&targetTagHi,
                    m_xgkick.packet.data() + sizeof(targetTagLo),
                    sizeof(targetTagHi));
        static uint32_t targetTraceCount = 0u;
        const bool programMatches = targetConfig.programStart == UINT32_MAX ||
                                    xmenXgkickProgramStart == targetConfig.programStart;
        const bool issuePcMatches = targetConfig.issuePc == UINT32_MAX ||
                                    xmenXgkickIssuePc == targetConfig.issuePc;
        if (targetTagLo == targetConfig.tagLo && programMatches && issuePcMatches &&
            targetTraceCount++ < 64u)
        {
            std::fprintf(stderr,
                         "[xmen-vu1:target-xgkick] tick=%llu program=0x%x issuePc=0x%x "
                         "source=0x%x bytes=%u copied=%u issueCycle=%llu finishCycle=%llu "
                         "tagLo=0x%016llx tagHi=0x%016llx\n",
                         static_cast<unsigned long long>(xmenXgkickIssueTick),
                         xmenXgkickProgramStart,
                         xmenXgkickIssuePc,
                         m_xgkick.sourceAddress,
                         m_xgkick.totalBytes,
                         m_xgkick.copiedBytes,
                         static_cast<unsigned long long>(m_xgkick.issueCycle),
                         static_cast<unsigned long long>(m_cycle),
                         static_cast<unsigned long long>(targetTagLo),
                         static_cast<unsigned long long>(targetTagHi));
            std::fflush(stderr);
        }
    }

    const XmenXgkickCensusConfig &censusConfig = xmenXgkickCensusConfig();
    static uint32_t censusTraceCount = 0u;
    if (censusConfig.enabled && m_xgkick.totalBytes >= 16u &&
        xmenXgkickIssueTick >= censusConfig.minTick &&
        censusTraceCount++ < censusConfig.maxEntries)
    {
        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
        std::memcpy(&tagLo, m_xgkick.packet.data(), sizeof(tagLo));
        std::memcpy(&tagHi,
                    m_xgkick.packet.data() + sizeof(tagLo),
                    sizeof(tagHi));
        std::fprintf(stderr,
                     "[xmen-vu1:xgkick-census] tick=%llu program=0x%x issuePc=0x%x "
                     "source=0x%x bytes=%u copied=%u issueCycle=%llu finishCycle=%llu "
                     "tagLo=0x%016llx tagHi=0x%016llx\n",
                     static_cast<unsigned long long>(xmenXgkickIssueTick),
                     xmenXgkickProgramStart,
                     xmenXgkickIssuePc,
                     m_xgkick.sourceAddress,
                     m_xgkick.totalBytes,
                     m_xgkick.copiedBytes,
                     static_cast<unsigned long long>(m_xgkick.issueCycle),
                     static_cast<unsigned long long>(m_cycle),
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi));
        std::fflush(stderr);
    }

    static uint32_t finishTraceCount = 0u;
    if (xmenTraceVu1Program || finishTraceCount++ < 32u)
    {
        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
        std::memcpy(&tagLo, m_xgkick.packet.data(), sizeof(tagLo));
        std::memcpy(&tagHi, m_xgkick.packet.data() + sizeof(tagLo), sizeof(tagHi));
        std::fprintf(stderr,
                     "[vu1:xgkick-finish] source=0x%x bytes=%u copied=%u cycle=%llu tagLo=0x%016llx tagHi=0x%016llx",
                     m_xgkick.sourceAddress,
                     m_xgkick.totalBytes,
                     m_xgkick.copiedBytes,
                     static_cast<unsigned long long>(m_cycle),
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi));
        const uint32_t payloadCount = (m_xgkick.totalBytes - 16u) / 16u;
        for (uint32_t i = 0u; i < payloadCount; ++i)
        {
            uint64_t payloadLo = 0u;
            uint64_t payloadHi = 0u;
            std::memcpy(&payloadLo, m_xgkick.packet.data() + 16u + i * 16u, sizeof(payloadLo));
            std::memcpy(&payloadHi, m_xgkick.packet.data() + 24u + i * 16u, sizeof(payloadHi));
            std::fprintf(stderr,
                         " ad%u=%02llx:%016llx",
                         i,
                         static_cast<unsigned long long>(payloadHi & 0xFFu),
                         static_cast<unsigned long long>(payloadLo));
        }
        std::fprintf(stderr, "\n");
    }

    if (xmenTitleVuTrace.active)
    {
        uint64_t tagLo = 0u;
        uint64_t tagHi = 0u;
        std::memcpy(&tagLo, m_xgkick.packet.data(), sizeof(tagLo));
        std::memcpy(&tagHi, m_xgkick.packet.data() + sizeof(tagLo), sizeof(tagHi));
        ++xmenTitleVuTrace.xgkickFinishes;
        xmenTitleVuTrace.xgkickBytes += m_xgkick.totalBytes;
        if (xmenTitleVuTrace.xgkickFinishes <= 1u)
        {
            std::fprintf(stdout,
                         "[xmen-title-vu-xgkick] exec=%u tick=%llu source=0x%x bytes=%u copied=%u tagLo=0x%016llx tagHi=0x%016llx\n",
                         xmenTitleVuTrace.executionIndex,
                         static_cast<unsigned long long>(xmenTitleVuTrace.tick),
                         m_xgkick.sourceAddress,
                         m_xgkick.totalBytes,
                         m_xgkick.copiedBytes,
                         static_cast<unsigned long long>(tagLo),
                         static_cast<unsigned long long>(tagHi));
        }
    }
    if (xmenGameplayVuSummary.active)
    {
        ++xmenGameplayVuSummary.xgkickFinishes;
        xmenGameplayVuSummary.xgkickBytes += m_xgkick.totalBytes;
        if (m_xgkick.totalBytes > xmenGameplayVuSummary.largestXgkickBytes)
        {
            xmenGameplayVuSummary.largestXgkickBytes = m_xgkick.totalBytes;
            xmenGameplayVuSummary.largestXgkickSource = m_xgkick.sourceAddress;
        }
    }

    static uint32_t submitTraceCount = 0u;
    if (xmenTraceVu1Program || submitTraceCount++ < 64u)
    {
        std::fprintf(stderr,
                     "[vu1:xgkick-submit] source=0x%x bytes=%u copied=%u memory=%p gs=%p\n",
                     m_xgkick.sourceAddress,
                     m_xgkick.totalBytes,
                     m_xgkick.copiedBytes,
                     static_cast<void *>(m_activeMemory),
                     static_cast<void *>(m_activeGs));
    }

    if (m_activeMemory)
        m_activeMemory->submitGifPacket(GifPathId::Path1, m_xgkick.packet.data(), m_xgkick.totalBytes);
    else if (m_activeGs)
        m_activeGs->processGIFPacket(m_xgkick.packet.data(), m_xgkick.totalBytes);
    m_xgkick.active = false;
}

void VU1Interpreter::startXgkick(uint32_t qwordAddress)
{
    if (m_unit != Unit::VU1 || !m_activeVuData || m_activeVuDataSize < 16u)
        return;

    const uint32_t sourceAddress = (qwordAddress * 16u) % m_activeVuDataSize;
    xmenXgkickProgramStart = xmenCurrentVuProgramStart;
    xmenXgkickIssuePc = m_state.pc;
    xmenXgkickIssueTick = xmenCurrentVuTick;
    if (xmenTitleVuTrace.active)
        ++xmenTitleVuTrace.xgkickStarts;
    if (xmenGameplayVuSummary.active)
        ++xmenGameplayVuSummary.xgkickStarts;
    static uint32_t startTraceCount = 0u;
    if (xmenTraceVu1Program || startTraceCount++ < 32u)
    {
        std::fprintf(stderr,
                     "[vu1:xgkick-start] qword=0x%x source=0x%x pc=0x%x cycle=%llu\n",
                     qwordAddress,
                     sourceAddress,
                     m_state.pc,
                     static_cast<unsigned long long>(m_cycle));
    }
    m_xgkick = {};
    m_xgkick.active = true;
    m_xgkick.sourceAddress = sourceAddress;
    m_xgkick.cycleCredit = 1u; // XGKICK's issue cycle counts toward PATH1.
    m_xgkick.issueCycle = m_cycle;
}

void VU1Interpreter::advanceOneCycle()
{
    ++m_cycle;
    m_state.cycles = m_cycle;
    // LSU commits become visible at the cycle boundary before PATH1 consumes
    // its next qword from VU memory.
    commitReadyPipelines();
    progressXgkick();
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    while (m_cycle < targetCycle)
        advanceOneCycle();
}

bool VU1Interpreter::pipelinesPending() const
{
    if (m_fdiv.valid || m_xgkick.active)
        return true;
    for (const ScalarPipelineEntry &entry : m_efu)
        if (entry.valid)
            return true;
    for (const FlagPipelineEntry &entry : m_flagPipeline)
        if (entry.valid)
            return true;
    for (const PendingStore &store : m_storePipeline)
        if (store.valid)
            return true;
    for (const PendingVfWrite &write : m_vfWritePipeline)
        if (write.valid)
            return true;
    for (const PendingViWrite &write : m_viWritePipeline)
        if (write.valid)
            return true;
    for (const PendingAccWrite &write : m_accWritePipeline)
        if (write.valid)
            return true;
    return false;
}

void VU1Interpreter::flushPipelines()
{
    while (pipelinesPending())
        advanceOneCycle();
}

uint64_t VU1Interpreter::calculatePairReadyCycle(const DecodedInstructionPair &decoded) const
{
    uint64_t ready = m_cycle;
    const InstructionUsage *usages[2] = {
        &decoded.upperUsage,
        &decoded.lowerUsage};
    for (const InstructionUsage *usage : usages)
    {
        if (!usage)
            continue;
        for (uint32_t index = 0; index < usage->vfReadCount; ++index)
        {
            const VfAccess &access = usage->vfRead[index];
            for (uint32_t component = 0; component < 4u; ++component)
            {
                if ((access.lanes & laneForComponent(component)) != 0u)
                    ready = std::max(ready, m_vfReady[access.reg][component]);
            }
        }
        for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
        {
            if ((usage->viRead & (1u << reg)) != 0u)
                ready = std::max(ready, m_viReady[reg]);
        }
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((usage->accRead & laneForComponent(component)) != 0u)
                ready = std::max(ready, m_accReady[component]);
        }
    }

    if (decoded.lowerUsage.pipeline == PipelineFdiv && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.pipeline == PipelineEfu)
        ready = std::max(ready, m_efuResourceReady);
    if (decoded.lowerUsage.waitQ && m_fdiv.valid)
        ready = std::max(ready, m_fdiv.readyCycle);
    if (decoded.lowerUsage.waitP)
    {
        for (const ScalarPipelineEntry &entry : m_efu)
            if (entry.valid)
                ready = std::max(ready, entry.readyCycle);
    }
    if (decoded.lowerUsage.pipeline == PipelineXgkick && m_xgkick.active)
        ready = std::max(ready, m_cycle + 1u);
    return ready;
}

void VU1Interpreter::markPairWrites(const DecodedInstructionPair &decoded)
{
    const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
    if (lowerWrite.reg != 0u &&
        decoded.suppressedLowerVf != lowerWrite.reg)
    {
        const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                     ? decoded.lowerUsage.vfLatency
                                     : decoded.lowerUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((lowerWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[lowerWrite.reg][component] = m_cycle + latency;
        }
    }

    const VfAccess upperWrite = decoded.upperUsage.vfWrite;
    if (upperWrite.reg != 0u)
    {
        const uint32_t latency = decoded.upperUsage.vfLatency != 0u
                                     ? decoded.upperUsage.vfLatency
                                     : decoded.upperUsage.latency;
        for (uint32_t component = 0; component < 4u; ++component)
        {
            if ((upperWrite.lanes & laneForComponent(component)) != 0u)
                m_vfReady[upperWrite.reg][component] = m_cycle + latency;
        }
    }

    for (uint32_t reg = 1; reg < m_viReady.size(); ++reg)
    {
        if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            m_viReady[reg] = m_cycle + (decoded.lowerUsage.viLatency != 0u ? decoded.lowerUsage.viLatency : decoded.lowerUsage.latency);
    }
    for (uint32_t component = 0; component < 4u; ++component)
    {
        if ((decoded.upperUsage.accWrite & laneForComponent(component)) != 0u)
            m_accReady[component] = m_cycle + kAccForwardLatency;
    }
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeUpperUsage(uint32_t upper) const
{
    InstructionUsage usage;
    usage.pipeline = PipelineFmac;
    usage.latency = kFmacLatency;

    const uint8_t op = static_cast<uint8_t>(upper & 0x3Fu);
    const uint8_t dest = DEST(upper);
    const uint8_t fs = FS(upper);
    const uint8_t ft = FT(upper);
    const uint8_t fd = FD(upper);

    if (op <= 0x2Fu)
    {
        addVfRead(usage, fs, dest);
        addVfWrite(usage, fd, dest);
        if (op <= 0x1Bu)
            addVfRead(usage, ft, laneForComponent(op & 3u));
        else if (op >= 0x28u)
            addVfRead(usage, ft, op == 0x2Eu ? 0xEu : dest);
        if (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
            op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
            op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
            op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if (op >= 0x3Cu)
    {
        const uint8_t special = static_cast<uint8_t>((upper & 3u) | ((upper >> 4) & 0x7Cu));
        const bool writesAcc =
            special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) ||
            special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        if (writesAcc)
        {
            addVfRead(usage, fs, dest);
            if (special <= 0x1Bu)
                addVfRead(usage, ft, laneForComponent(special & 3u));
            else if ((special >= 0x28u && special <= 0x2Eu))
                addVfRead(usage, ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if ((special >= 0x08u && special <= 0x0Fu) ||
                special == 0x21u || special == 0x23u || special == 0x25u ||
                special == 0x27u || special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if (special >= 0x10u && special <= 0x17u)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Du)
        {
            addVfRead(usage, fs, dest);
            addVfWrite(usage, ft, dest);
        }
        else if (special == 0x1Fu)
        {
            addVfRead(usage, fs, 0xEu);
            addVfRead(usage, ft, 0x1u);
            usage.writesClip = true;
        }
        else if (special != 0x2Fu && special != 0x30u)
        {
            usage.reserved = true;
        }
        return usage;
    }

    usage.reserved = true;
    return usage;
}

VU1Interpreter::InstructionUsage VU1Interpreter::decodeLowerUsage(uint32_t lower) const
{
    InstructionUsage usage;
    if (lower == 0u || lower == 0x8000033Cu)
        return usage;

    const uint8_t opHi = static_cast<uint8_t>((lower >> 25) & 0x7Fu);
    const uint8_t vfT = FT(lower);
    const uint8_t vfS = FS(lower);
    const uint8_t viT = VIT(lower);
    const uint8_t viS = VIS(lower);
    const uint8_t viD = VID(lower);
    const uint8_t dest = DEST(lower);
    auto readVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    auto writeVi = [&](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWrite |= static_cast<uint16_t>(1u << reg);
    };

    switch (opHi)
    {
    case 0x00:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        return usage;
    case 0x01:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viT);
        addVfRead(usage, vfS, dest);
        return usage;
    case 0x04:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x05:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x08:
    case 0x09:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x10:
    case 0x12:
    case 0x13:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(1u);
        return usage;
    case 0x11:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        usage.writesClip = true;
        return usage;
    case 0x14:
    case 0x16:
    case 0x17:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x15:
        usage.pipeline = PipelineFmac;
        usage.latency = kFmacLatency;
        return usage;
    case 0x18:
    case 0x1A:
    case 0x1B:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x1C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.readsClip = true;
        writeVi(viT);
        return usage;
    case 0x20:
        usage.pipeline = PipelineBranch;
        return usage;
    case 0x21:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        writeVi(viT);
        return usage;
    case 0x24:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x25:
        usage.pipeline = PipelineBranch;
        usage.latency = 1u;
        readVi(viS);
        writeVi(viT);
        return usage;
    case 0x28:
    case 0x29:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        readVi(viT);
        return usage;
    case 0x2C:
    case 0x2D:
    case 0x2E:
    case 0x2F:
        usage.pipeline = PipelineBranch;
        readVi(viS);
        return usage;
    case 0x40:
        break;
    default:
        usage.reserved = true;
        return usage;
    }

    const uint8_t direct = static_cast<uint8_t>(lower & 0x3Fu);
    if (direct == 0x30u || direct == 0x31u || direct == 0x34u || direct == 0x35u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        readVi(viT);
        writeVi(viD);
        return usage;
    }
    if (direct == 0x32u)
    {
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viT);
        return usage;
    }
    if (direct < 0x3Cu)
    {
        usage.reserved = true;
        return usage;
    }

    const uint8_t special = static_cast<uint8_t>((lower & 3u) | ((lower >> 4) & 0x7Cu));
    switch (special)
    {
    case 0x30:
    case 0x31:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfRead(usage, vfS, special == 0x31u ? 0xFu : dest);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x34:
    case 0x36:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        usage.viLatency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viS);
        writeVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x35:
    case 0x37:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        readVi(viT);
        writeVi(viT);
        addVfRead(usage, vfS, dest);
        break;
    case 0x38:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x39:
        usage.pipeline = PipelineFdiv;
        usage.latency = 7u;
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3A:
        usage.pipeline = PipelineFdiv;
        usage.latency = 13u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        addVfRead(usage, vfT, laneForComponent((lower >> 23) & 3u));
        break;
    case 0x3B:
        usage.pipeline = PipelineFdiv;
        usage.waitQ = true;
        break;
    case 0x3C:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        usage.delaysNextBranchRead = true;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        writeVi(viT);
        break;
    case 0x3D:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        readVi(viS);
        addVfWrite(usage, vfT, dest);
        break;
    case 0x3E:
        usage.pipeline = PipelineLsu;
        usage.latency = 4u;
        readVi(viS);
        writeVi(viT);
        break;
    case 0x3F:
        usage.pipeline = PipelineLsu;
        usage.latency = 1u;
        readVi(viS);
        readVi(viT);
        break;
    case 0x40:
    case 0x41:
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x42:
    case 0x43:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x64:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineFmac;
        usage.latency = 4u;
        addVfWrite(usage, vfT, dest);
        break;
    case 0x68:
    case 0x69:
        usage.pipeline = PipelineIalu;
        usage.latency = 1u;
        writeVi(viT);
        break;
    case 0x6C:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineXgkick;
        usage.latency = 2u;
        readVi(viS);
        break;
    case 0x70:
    case 0x71:
    case 0x72:
    case 0x73:
    case 0x74:
    case 0x75:
    case 0x76:
    case 0x77:
    case 0x78:
    case 0x79:
    case 0x7A:
    case 0x7C:
    case 0x7D:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        switch (special)
        {
        case 0x70:
            usage.latency = 11u;
            break;
        case 0x71:
        case 0x72:
        case 0x77:
            usage.latency = 18u;
            break;
        case 0x73:
            usage.latency = 24u;
            break;
        case 0x74:
        case 0x75:
        case 0x7C:
            usage.latency = 54u;
            break;
        case 0x76:
        case 0x78:
        case 0x7A:
            usage.latency = 12u;
            break;
        case 0x79:
            usage.latency = 29u;
            break;
        case 0x7D:
            usage.latency = 44u;
            break;
        default:
            break;
        }
        if (special >= 0x70u && special <= 0x73u)
            addVfRead(usage, vfS, 0xEu);
        else if (special == 0x74u)
            addVfRead(usage, vfS, 0xCu);
        else if (special == 0x75u)
            addVfRead(usage, vfS, 0xAu);
        else if (special == 0x76u)
            addVfRead(usage, vfS, 0xFu);
        else
            addVfRead(usage, vfS, laneForComponent((lower >> 21) & 3u));
        break;
    case 0x7B:
        if (m_unit == Unit::VU0)
        {
            usage.reserved = true;
            break;
        }
        usage.pipeline = PipelineEfu;
        usage.waitP = true;
        break;
    default:
        usage.reserved = true;
        break;
    }
    return usage;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::decodeInstructionPair(const uint8_t *vuCode, uint32_t pc) const
{
    DecodedInstructionPair decoded;
    std::memcpy(&decoded.lower, vuCode + pc, sizeof(decoded.lower));
    std::memcpy(&decoded.upper, vuCode + pc + sizeof(decoded.lower), sizeof(decoded.upper));
    decoded.iBit = (decoded.upper & 0x80000000u) != 0u;
    decoded.eBit = (decoded.upper & 0x40000000u) != 0u;
    decoded.mBit = (decoded.upper & 0x20000000u) != 0u;
    decoded.dBit = (decoded.upper & 0x10000000u) != 0u;
    decoded.tBit = (decoded.upper & 0x08000000u) != 0u;
    decoded.upperUsage = decodeUpperUsage(decoded.upper);
    if (!decoded.iBit)
        decoded.lowerUsage = decodeLowerUsage(decoded.lower);

    const uint8_t upperWriteReg = decoded.upperUsage.vfWrite.reg;
    if (upperWriteReg != 0u && (vfReadLanes(decoded.lowerUsage, upperWriteReg) != 0u || decoded.lowerUsage.vfWrite.reg == upperWriteReg))
    {
        decoded.upperVfShadowReg = upperWriteReg;
        if (decoded.lowerUsage.vfWrite.reg == upperWriteReg)
            decoded.suppressedLowerVf = upperWriteReg;
    }
    return decoded;
}

void VU1Interpreter::rebuildDecodedCodeCache(const uint8_t *vuCode, uint32_t codeSize,
                                             const PS2Memory *memory, uint64_t generation)
{
    const uint32_t pairCount = std::min<uint32_t>(codeSize / 8u, kMaxDecodedPairs);
    for (uint32_t i = 0; i < pairCount; ++i)
        m_decodedCodeCache[i] = decodeInstructionPair(vuCode, i * 8u);

    m_cachedVuCode = vuCode;
    m_cachedMemory = memory;
    m_cachedCodeSize = codeSize;
    m_cachedCodeGeneration = generation;
    m_decodedCodeCacheValid = true;
}

VU1Interpreter::DecodedInstructionPair VU1Interpreter::getDecodedInstructionPairForPc(
    const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t pc)
{
    if ((pc & 7u) != 0u)
        return decodeInstructionPair(vuCode, pc);

    const bool trackedVu1Code = memory != nullptr &&
                                ((m_unit == Unit::VU1 && vuCode == memory->getVU1Code()) ||
                                 (m_unit == Unit::VU0 && vuCode == memory->getVU0Code()));
    if (!trackedVu1Code)
        return decodeInstructionPair(vuCode, pc);

    const uint64_t generation = m_unit == Unit::VU1 ? memory->getVU1CodeGeneration() : memory->getVU0CodeGeneration();
    if (!m_decodedCodeCacheValid ||
        m_cachedVuCode != vuCode ||
        m_cachedMemory != memory ||
        m_cachedCodeSize != codeSize ||
        m_cachedCodeGeneration != generation)
    {
        rebuildDecodedCodeCache(vuCode, codeSize, memory, generation);
    }
    const uint32_t pairIndex = pc / 8u;
    if (pairIndex >= kMaxDecodedPairs)
        return decodeInstructionPair(vuCode, pc);
    return m_decodedCodeCache[pairIndex];
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    RUNTIME_ERROR(
        "[VU" << (m_unit == Unit::VU1 ? "1" : "0")
              << " reserved " << (upper ? "upper" : "lower")
              << "] cycle=" << m_cycle
              << " pc=0x" << std::hex << m_state.pc
              << " instruction=0x" << instruction
              << std::dec << '\n');
    m_stopRequested = true;
}

void VU1Interpreter::execute(uint8_t *vuCode, uint32_t codeSize,
                             uint8_t *vuData, uint32_t dataSize,
                             GS &gs, PS2Memory *memory,
                             uint32_t startPC, uint32_t top, uint32_t itop,
                             uint32_t maxCycles)
{
    static std::atomic<uint32_t> titleExecutionCount{0u};
    const uint64_t titleTick = memory
        ? memory->gs().vsyncTick.load(std::memory_order_relaxed)
        : 0u;
    if (m_unit == Unit::VU1)
    {
        xmenCurrentVuProgramStart = startPC;
        xmenCurrentVuTick = titleTick;
    }
    static const uint64_t xmenVuCensusMinTick = []()
    {
        const char *value = std::getenv("PS2X_XMEN_VU_CENSUS_MIN_TICK");
        if (!value || value[0] == '\0')
            return uint64_t{600u};

        char *end = nullptr;
        const uint64_t parsed = std::strtoull(value, &end, 0);
        return end && end != value && *end == '\0' ? parsed : uint64_t{600u};
    }();
    const uint32_t censusIndex = (startPC & microAddressMask()) / 8u;
    const bool traceVuCensus =
        m_unit == Unit::VU1 && std::getenv("PS2X_XMEN_VU_CENSUS") != nullptr &&
        titleTick >= xmenVuCensusMinTick && censusIndex < xmenVuCensusCounts.size() &&
        xmenVuCensusCounts[censusIndex] < 8u;
    if (traceVuCensus)
        ++xmenVuCensusCounts[censusIndex];
    if (m_unit == Unit::VU1 && (titleTick == 635u || traceVuCensus))
    {
        xmenGameplayVuSummary = {};
        xmenGameplayVuSummary.active = true;
        xmenGameplayVuSummary.tick = titleTick;
        xmenGameplayVuSummary.startCycle = m_cycle;
        xmenGameplayVuSummary.startPc = startPC;
        xmenGameplayVuSummary.top = top;
        xmenGameplayVuSummary.itop = itop;
        xmenGameplayVuSummary.slices = 1u;
        if (vuData && dataSize >= 0x498u)
        {
            std::memcpy(&xmenGameplayVuSummary.startTagLo,
                        vuData + 0x490u,
                        sizeof(xmenGameplayVuSummary.startTagLo));
            xmenGameplayVuSummary.endTagLo = xmenGameplayVuSummary.startTagLo;
        }
    }
    uint32_t titleExecutionIndex = UINT32_MAX;
    if (m_unit == Unit::VU1 && titleTick >= 1910u && titleTick <= 1960u)
    {
        titleExecutionIndex =
            titleExecutionCount.fetch_add(1u, std::memory_order_relaxed);
    }
    const bool traceTitleExecution = titleExecutionIndex < 512u;
    if (traceTitleExecution)
    {
        xmenTitleVuTrace = {};
        xmenTitleVuTrace.active = true;
        xmenTitleVuTrace.tick = titleTick;
        xmenTitleVuTrace.executionIndex = titleExecutionIndex;
    }

    static bool dumpedFirstMainProgram = false;
    if (m_unit == Unit::VU1 && startPC == 0u && !dumpedFirstMainProgram)
    {
        dumpedFirstMainProgram = true;
        for (uint32_t pc = 0u; pc <= 0x230u && pc + 8u <= codeSize; pc += 8u)
        {
            uint32_t lower = 0u;
            uint32_t upper = 0u;
            std::memcpy(&lower, vuCode + pc, sizeof(lower));
            std::memcpy(&upper, vuCode + pc + 4u, sizeof(upper));
            std::fprintf(stderr, "[vu1:main-code] pc=0x%04x lower=%08x upper=%08x\n",
                         pc, lower, upper);
        }
        for (const uint32_t qword : {0x4Eu, 0x4Fu, 0x78u, 0x79u})
        {
            uint32_t words[4]{};
            const uint32_t address = qword * 16u;
            if (address + sizeof(words) <= dataSize)
                std::memcpy(words, vuData + address, sizeof(words));
            std::fprintf(stderr,
                         "[vu1:main-data] qword=0x%03x words=%08x,%08x,%08x,%08x\n",
                         qword, words[0], words[1], words[2], words[3]);
        }
    }
    static uint32_t tracedXmenMeshProgram = 0u;
    static uint32_t tracedXmenGameplayKickPrograms = 0u;
    static uint32_t tracedXmenTargetProgram = 0u;
    const char *targetProgramEnvironment =
        std::getenv("PS2X_XMEN_VU_TRACE_START");
    const uint32_t targetProgram = targetProgramEnvironment
        ? static_cast<uint32_t>(std::strtoul(targetProgramEnvironment, nullptr, 0))
        : UINT32_MAX;
    static const uint32_t targetProgramTraceLimit = []()
    {
        const char *value = std::getenv("PS2X_XMEN_VU_TRACE_COUNT");
        if (!value || value[0] == '\0')
            return 1u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        return end && end != value && *end == '\0'
            ? static_cast<uint32_t>(std::min<unsigned long>(parsed, 256u))
            : 1u;
    }();
    const bool traceMeshProgram =
        m_unit == Unit::VU1 && startPC == 0x230u && tracedXmenMeshProgram < 8u;
    const bool traceGameplayKickProgram =
        m_unit == Unit::VU1 && startPC == 0x230u && titleTick >= 640u &&
        tracedXmenGameplayKickPrograms < 24u;
    const bool traceTargetProgram =
        m_unit == Unit::VU1 && targetProgramEnvironment != nullptr &&
        startPC == (targetProgram & microAddressMask()) && titleTick >= xmenVuCensusMinTick &&
        tracedXmenTargetProgram < targetProgramTraceLimit;
    const bool traceThisProgram =
        traceMeshProgram || traceGameplayKickProgram || traceTargetProgram;
    if (traceThisProgram)
    {
        if (traceMeshProgram)
            ++tracedXmenMeshProgram;
        if (traceGameplayKickProgram)
            ++tracedXmenGameplayKickPrograms;
        if (traceTargetProgram)
            ++tracedXmenTargetProgram;
        xmenTraceVu1Program = true;
        std::fprintf(stderr,
                     "[xmen-vu1:begin] kind=%s tick=%llu start=0x%x top=0x%x itop=0x%x\n",
                     traceTargetProgram ? "target" :
                         (traceGameplayKickProgram ? "gameplay-kick" : "mesh"),
                     static_cast<unsigned long long>(titleTick),
                     startPC, top, itop);

        const auto dumpDataRange = [&](uint32_t firstQword, uint32_t lastQword)
        {
            for (uint32_t qword = firstQword; qword <= lastQword; ++qword)
            {
                uint32_t words[4]{};
                const uint32_t sourceQword = qword & 0x3FFu;
                const uint32_t address = sourceQword * 16u;
                if (address + sizeof(words) <= dataSize)
                    std::memcpy(words, vuData + address, sizeof(words));
                std::fprintf(stderr,
                             "[xmen-vu1:input] qword=0x%x words=%08x,%08x,%08x,%08x\n",
                             sourceQword, words[0], words[1], words[2], words[3]);
            }
        };

        dumpDataRange(top, top + 7u);

        const char *dumpFirstValue =
            std::getenv("PS2X_XMEN_VU_TRACE_DATA_FIRST");
        const char *dumpLastValue =
            std::getenv("PS2X_XMEN_VU_TRACE_DATA_LAST");
        if (dumpFirstValue && dumpLastValue)
        {
            char *firstEnd = nullptr;
            char *lastEnd = nullptr;
            const unsigned long first =
                std::strtoul(dumpFirstValue, &firstEnd, 0);
            const unsigned long last =
                std::strtoul(dumpLastValue, &lastEnd, 0);
            if (firstEnd && firstEnd != dumpFirstValue && *firstEnd == '\0' &&
                lastEnd && lastEnd != dumpLastValue && *lastEnd == '\0' &&
                first <= last && last - first < 256u)
            {
                dumpDataRange(static_cast<uint32_t>(first),
                              static_cast<uint32_t>(last));
            }
        }
    }
    resetScheduler();
    m_state.pc = startPC & microAddressMask();
    m_state.ebit = false;
    m_state.haltAfterDelaySlot = false;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_state.top = top;
    m_state.itop = itop;
    m_state.branchPending = false;
    m_state.branchTarget = 0;
    m_state.branchDelay = 0;
    m_state.vf[0][0] = 0.0f;
    m_state.vf[0][1] = 0.0f;
    m_state.vf[0][2] = 0.0f;
    m_state.vf[0][3] = 1.0f;
    const uint64_t startCycle = m_cycle;
    m_running = true;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    const uint64_t elapsedCycles = m_cycle - startCycle;
    if (xmenGameplayVuSummary.active && !m_running)
    {
        if (vuData && dataSize >= 0x498u)
            std::memcpy(&xmenGameplayVuSummary.endTagLo,
                        vuData + 0x490u,
                        sizeof(xmenGameplayVuSummary.endTagLo));
        finishXmenGameplayVuSummary(m_state, m_cycle);
    }
    if (traceTitleExecution)
    {
        std::fprintf(stdout,
                     "[xmen-title-vu-exec] index=%u tick=%llu start=0x%x top=0x%x itop=0x%x cycles=%llu/%u end=0x%x starts=%u finishes=%u bytes=%llu stop=%u ebit=%u\n",
                     xmenTitleVuTrace.executionIndex,
                     static_cast<unsigned long long>(titleTick),
                     startPC,
                     top,
                     itop,
                     static_cast<unsigned long long>(elapsedCycles),
                     maxCycles,
                     m_state.pc,
                     xmenTitleVuTrace.xgkickStarts,
                     xmenTitleVuTrace.xgkickFinishes,
                     static_cast<unsigned long long>(xmenTitleVuTrace.xgkickBytes),
                     m_stopRequested ? 1u : 0u,
                     m_state.ebit ? 1u : 0u);
        std::fflush(stdout);
        xmenTitleVuTrace = {};
    }
    static uint32_t executeTraceCount = 0u;
    if (executeTraceCount++ < 96u)
    {
        std::fprintf(stderr,
            "[vu1:execute-summary] start=0x%x top=0x%x itop=0x%x cycles=%llu/%u "
            "end-pc=0x%x stop=%u ebit=%u branch=%u target=0x%x delay=%u\n",
            startPC, top, itop, static_cast<unsigned long long>(elapsedCycles), maxCycles,
            m_state.pc, m_stopRequested ? 1u : 0u, m_state.ebit ? 1u : 0u,
            m_state.branchPending ? 1u : 0u, m_state.branchTarget, m_state.branchDelay);
    }
    if (traceThisProgram)
    {
        std::fprintf(stderr,
                     "[xmen-vu1:slice] pc=0x%x cycle=%llu running=%u status=0x%x mac=0x%x clip=0x%x q=%g\n",
                     m_state.pc, static_cast<unsigned long long>(m_cycle),
                     m_running ? 1u : 0u, m_state.status, m_state.mac, m_state.clip, m_state.q);
        if (!m_running)
            xmenTraceVu1Program = false;
    }
}

void VU1Interpreter::resume(uint8_t *vuCode, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize,
                            GS &gs, PS2Memory *memory,
                            uint32_t top, uint32_t itop, uint32_t maxCycles)
{
    if (m_unit == Unit::VU1 && memory)
        xmenCurrentVuTick = memory->gs().vsyncTick.load(std::memory_order_relaxed);
    m_state.top = top;
    m_state.itop = itop;
    m_state.stoppedByD = false;
    m_state.stoppedByT = false;
    m_running = true;
    if (xmenGameplayVuSummary.active)
        ++xmenGameplayVuSummary.slices;
    run(vuCode, codeSize, vuData, dataSize, gs, memory, maxCycles);
    if (xmenGameplayVuSummary.active && !m_running)
    {
        if (vuData && dataSize >= 0x498u)
            std::memcpy(&xmenGameplayVuSummary.endTagLo,
                        vuData + 0x490u,
                        sizeof(xmenGameplayVuSummary.endTagLo));
        finishXmenGameplayVuSummary(m_state, m_cycle);
    }
    if (xmenTraceVu1Program && !m_running)
    {
        std::fprintf(stderr,
                     "[xmen-vu1:complete] pc=0x%x cycle=%llu status=0x%x mac=0x%x clip=0x%x q=%g\n",
                     m_state.pc, static_cast<unsigned long long>(m_cycle), m_state.status,
                     m_state.mac, m_state.clip, m_state.q);
        xmenTraceVu1Program = false;
    }
}

void VU1Interpreter::run(uint8_t *vuCode, uint32_t codeSize,
                         uint8_t *vuData, uint32_t dataSize,
                         GS &gs, PS2Memory *memory, uint32_t maxCycles)
{
    struct BudgetTraceEntry
    {
        uint32_t pc = 0;
        uint32_t lower = 0;
        uint32_t upper = 0;
        int32_t vi[16]{};
    };
    BudgetTraceEntry budgetTrace[32]{};
    uint32_t budgetTraceCount = 0;

    m_activeVuData = vuData;
    m_activeVuDataSize = dataSize;
    m_activeGs = &gs;
    m_activeMemory = memory;

    const int previousRoundingMode = std::fegetround();
    const bool useVuRounding = std::fesetround(FE_TOWARDZERO) == 0;
    const uint64_t budgetEnd = m_cycle + maxCycles;
    bool programEnded = false;
    while (m_cycle < budgetEnd && !m_stopRequested)
    {
        commitReadyPipelines();
        if (m_state.pc + 8u > codeSize)
        {
            programEnded = true;
            break;
        }

        const DecodedInstructionPair decoded = getDecodedInstructionPairForPc(vuCode, codeSize, memory, m_state.pc);
        BudgetTraceEntry &budgetEntry = budgetTrace[budgetTraceCount++ & 31u];
        budgetEntry.pc = m_state.pc;
        budgetEntry.lower = decoded.lower;
        budgetEntry.upper = decoded.upper;
        std::memcpy(budgetEntry.vi, m_state.vi, sizeof(budgetEntry.vi));
        if (xmenTraceVu1Program)
        {
            std::fprintf(stderr,
                         "[xmen-vu1:pair] cycle=%llu pc=0x%03x lower=%08x upper=%08x "
                         "vi=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d "
                         "q=%g clip=0x%x workingClip=0x%x\n",
                         static_cast<unsigned long long>(m_cycle), m_state.pc,
                         decoded.lower, decoded.upper,
                         m_state.vi[1], m_state.vi[2], m_state.vi[3], m_state.vi[4],
                         m_state.vi[5], m_state.vi[6], m_state.vi[7], m_state.vi[8],
                         m_state.vi[9], m_state.vi[10], m_state.vi[11], m_state.vi[12],
                         m_state.vi[13], m_state.vi[14], m_state.vi[15], m_state.q,
                         m_state.clip, m_workingClip);
            const auto dumpVfReads = [&](const char *slot, const InstructionUsage &usage)
            {
                for (uint32_t index = 0u; index < usage.vfReadCount; ++index)
                {
                    const VfAccess access = usage.vfRead[index];
                    std::fprintf(stderr,
                                 "[xmen-vu1:read] cycle=%llu pc=0x%03x slot=%s "
                                 "vf%u mask=0x%x value=%g,%g,%g,%g\n",
                                 static_cast<unsigned long long>(m_cycle), m_state.pc,
                                 slot, access.reg, access.lanes,
                                 m_state.vf[access.reg][0], m_state.vf[access.reg][1],
                                 m_state.vf[access.reg][2], m_state.vf[access.reg][3]);
                }
            };
            dumpVfReads("upper", decoded.upperUsage);
            dumpVfReads("lower", decoded.lowerUsage);
            if (m_state.pc >= 0x280u && m_state.pc <= 0x2B8u)
            {
                const uint32_t matrixBase = static_cast<uint32_t>(m_state.vi[5]);
                std::fprintf(stderr,
                             "[xmen-vu1:matrix-load] cycle=%llu pc=0x%03x vi5=0x%x",
                             static_cast<unsigned long long>(m_cycle), m_state.pc, matrixBase);
                for (uint32_t row = 0u; row < 4u; ++row)
                {
                    uint32_t words[4]{};
                    const uint32_t address = ((matrixBase + row) * 16u) % dataSize;
                    if (address + sizeof(words) <= dataSize)
                        std::memcpy(words, vuData + address, sizeof(words));
                    std::fprintf(stderr, " row%u=%08x,%08x,%08x,%08x",
                                 row, words[0], words[1], words[2], words[3]);
                }
                std::fprintf(stderr,
                             " vf1-4.w=%g,%g,%g,%g\n",
                             m_state.vf[1][3], m_state.vf[2][3],
                             m_state.vf[3][3], m_state.vf[4][3]);
            }
            if ((m_state.pc >= 0x2E0u && m_state.pc <= 0x340u) &&
                (m_state.pc & 7u) == 0u)
            {
                uint32_t sourceWords[4]{};
                const uint32_t sourceAddress =
                    (static_cast<uint32_t>(m_state.vi[13]) * 16u) % dataSize;
                if (sourceAddress + sizeof(sourceWords) <= dataSize)
                    std::memcpy(sourceWords, vuData + sourceAddress, sizeof(sourceWords));
                std::fprintf(stderr,
                             "[xmen-vu1:operand] cycle=%llu pc=0x%03x "
                             "vi9-15=%d,%d,%d,%d,%d,%d,%d vi13-qword=0x%x "
                             "data=%08x,%08x,%08x,%08x "
                             "vf19=%g,%g,%g,%g vf21=%g,%g,%g,%g q=%g\n",
                             static_cast<unsigned long long>(m_cycle), m_state.pc,
                             m_state.vi[9], m_state.vi[10], m_state.vi[11], m_state.vi[12],
                             m_state.vi[13], m_state.vi[14], m_state.vi[15], sourceAddress / 16u,
                             sourceWords[0], sourceWords[1], sourceWords[2], sourceWords[3],
                             m_state.vf[19][0], m_state.vf[19][1],
                             m_state.vf[19][2], m_state.vf[19][3],
                             m_state.vf[21][0], m_state.vf[21][1],
                             m_state.vf[21][2], m_state.vf[21][3], m_state.q);
                if (m_state.pc >= 0x2F8u && m_state.pc <= 0x320u)
                {
                    std::fprintf(stderr,
                                 "[xmen-vu1:matrix] cycle=%llu pc=0x%03x "
                                 "vf1=%g,%g,%g,%g vf2=%g,%g,%g,%g "
                                 "vf3=%g,%g,%g,%g vf4=%g,%g,%g,%g "
                                 "vf29=%g,%g,%g,%g acc=%g,%g,%g,%g\n",
                                 static_cast<unsigned long long>(m_cycle), m_state.pc,
                                 m_state.vf[1][0], m_state.vf[1][1], m_state.vf[1][2], m_state.vf[1][3],
                                 m_state.vf[2][0], m_state.vf[2][1], m_state.vf[2][2], m_state.vf[2][3],
                                 m_state.vf[3][0], m_state.vf[3][1], m_state.vf[3][2], m_state.vf[3][3],
                                 m_state.vf[4][0], m_state.vf[4][1], m_state.vf[4][2], m_state.vf[4][3],
                                 m_state.vf[29][0], m_state.vf[29][1], m_state.vf[29][2], m_state.vf[29][3],
                                 m_state.acc[0], m_state.acc[1], m_state.acc[2], m_state.acc[3]);
                }
            }
        }
        if (decoded.upperUsage.reserved || decoded.lowerUsage.reserved)
        {
            reportReservedInstruction(decoded.upperUsage.reserved, decoded.upperUsage.reserved ? decoded.upper : decoded.lower);
            break;
        }

        uint64_t readyCycle = calculatePairReadyCycle(decoded);
        while (readyCycle > m_cycle)
        {
            if (readyCycle >= budgetEnd)
            {
                advanceTo(budgetEnd);
                break;
            }
            advanceTo(readyCycle);
            readyCycle = calculatePairReadyCycle(decoded);
        }
        if (m_cycle >= budgetEnd)
            break;

        uint8_t writtenVi = 0u;
        int32_t oldVi = 0;
        for (uint32_t reg = 1; reg < 16u; ++reg)
        {
            if ((decoded.lowerUsage.viWrite & (1u << reg)) != 0u)
            {
                writtenVi = static_cast<uint8_t>(reg);
                oldVi = m_state.vi[reg];
                break;
            }
        }

        const VfAccess upperWrite = decoded.upperUsage.vfWrite;
        const VfAccess lowerWrite = decoded.lowerUsage.vfWrite;
        const bool hasUpperWrite = upperWrite.reg != 0u;
        const bool hasLowerWrite = lowerWrite.reg != 0u && decoded.suppressedLowerVf != lowerWrite.reg;
        const bool hasDistinctLowerWrite = hasLowerWrite && (!hasUpperWrite || lowerWrite.reg != upperWrite.reg);
        float oldUpperVf[4]{};
        float newUpperVf[4]{};
        float oldLowerVf[4]{};
        float newLowerVf[4]{};
        float oldAcc[4]{};
        float newAcc[4]{};
        if (hasUpperWrite)
            std::memcpy(oldUpperVf, m_state.vf[upperWrite.reg], sizeof(oldUpperVf));
        if (hasDistinctLowerWrite)
            std::memcpy(oldLowerVf, m_state.vf[lowerWrite.reg], sizeof(oldLowerVf));
        if (decoded.upperUsage.accWrite != 0u)
            std::memcpy(oldAcc, m_state.acc, sizeof(oldAcc));

        if (decoded.iBit)
        {
            execUpper(decoded.upper);
            float immediate = 0.0f;
            std::memcpy(&immediate, &decoded.lower, sizeof(immediate));
            m_state.i = normalizeOperand(immediate);
        }
        else if (decoded.upperVfShadowReg != 0u)
        {
            float oldVf[4]{};
            float upperVf[4]{};
            std::memcpy(oldVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(oldVf));
            execUpper(decoded.upper);
            std::memcpy(upperVf,
                        m_state.vf[decoded.upperVfShadowReg],
                        sizeof(upperVf));
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        oldVf,
                        sizeof(oldVf));
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
            std::memcpy(m_state.vf[decoded.upperVfShadowReg],
                        upperVf,
                        sizeof(upperVf));
        }
        else
        {
            execUpper(decoded.upper);
            execLower(decoded.lower, vuData, dataSize, gs, memory, decoded.upper);
        }

        if (xmenTraceVu1Program)
        {
            if (decoded.upperUsage.writesClip || decoded.lowerUsage.readsClip ||
                decoded.lowerUsage.writesClip)
            {
                std::fprintf(stderr,
                             "[xmen-vu1:clip-flags] cycle=%llu pc=0x%03x "
                             "upperWrites=%u lowerReads=%u lowerWrites=%u "
                             "committed=0x%x working=0x%x vi1=%d\n",
                             static_cast<unsigned long long>(m_cycle), m_state.pc,
                             decoded.upperUsage.writesClip ? 1u : 0u,
                             decoded.lowerUsage.readsClip ? 1u : 0u,
                             decoded.lowerUsage.writesClip ? 1u : 0u,
                             m_state.clip, m_workingClip, m_state.vi[1]);
            }
            const auto dumpVfWrite = [&](const char *slot, const VfAccess &access)
            {
                if (access.reg == 0u)
                    return;
                std::fprintf(stderr,
                             "[xmen-vu1:write] cycle=%llu pc=0x%03x slot=%s "
                             "vf%u mask=0x%x value=%g,%g,%g,%g\n",
                             static_cast<unsigned long long>(m_cycle), m_state.pc,
                             slot, access.reg, access.lanes,
                             m_state.vf[access.reg][0], m_state.vf[access.reg][1],
                             m_state.vf[access.reg][2], m_state.vf[access.reg][3]);
            };
            dumpVfWrite("upper", decoded.upperUsage.vfWrite);
            if (decoded.suppressedLowerVf != decoded.lowerUsage.vfWrite.reg)
                dumpVfWrite("lower", decoded.lowerUsage.vfWrite);
            if (decoded.upperUsage.accWrite != 0u)
            {
                std::fprintf(stderr,
                             "[xmen-vu1:acc-write] cycle=%llu pc=0x%03x mask=0x%x "
                             "value=%g,%g,%g,%g\n",
                             static_cast<unsigned long long>(m_cycle), m_state.pc,
                             decoded.upperUsage.accWrite,
                             m_state.acc[0], m_state.acc[1], m_state.acc[2], m_state.acc[3]);
            }
        }

        m_viBranchBackupValid = false;

        if (hasUpperWrite)
        {
            std::memcpy(newUpperVf, m_state.vf[upperWrite.reg], sizeof(newUpperVf));
            std::memcpy(m_state.vf[upperWrite.reg], oldUpperVf, sizeof(oldUpperVf));
            const uint32_t latency =
                decoded.upperUsage.vfLatency != 0u
                    ? decoded.upperUsage.vfLatency
                    : decoded.upperUsage.latency;
            queueVfWrite(upperWrite.reg, upperWrite.lanes, newUpperVf, latency);
        }
        if (hasDistinctLowerWrite)
        {
            std::memcpy(newLowerVf, m_state.vf[lowerWrite.reg], sizeof(newLowerVf));
            std::memcpy(m_state.vf[lowerWrite.reg], oldLowerVf, sizeof(oldLowerVf));
            const uint32_t latency = decoded.lowerUsage.vfLatency != 0u
                                         ? decoded.lowerUsage.vfLatency
                                         : decoded.lowerUsage.latency;
            queueVfWrite(lowerWrite.reg, lowerWrite.lanes, newLowerVf, latency);
        }
        if (decoded.upperUsage.accWrite != 0u)
        {
            std::memcpy(newAcc, m_state.acc, sizeof(newAcc));
            std::memcpy(m_state.acc, oldAcc, sizeof(oldAcc));
            // ACC is forwarded to the next upper instruction. Its arithmetic
            // flags still use the normal four-cycle FMAC timeline.
            queueAccWrite(decoded.upperUsage.accWrite, newAcc,
                          kAccForwardLatency);
        }
        if (writtenVi != 0u)
        {
            const int32_t newVi = m_state.vi[writtenVi];
            m_state.vi[writtenVi] = oldVi;
            const uint32_t latency =
                decoded.lowerUsage.viLatency != 0u
                    ? decoded.lowerUsage.viLatency
                    : decoded.lowerUsage.latency;
            queueViWrite(writtenVi, newVi, latency);
        }

        markPairWrites(decoded);
        if (writtenVi != 0u && decoded.lowerUsage.delaysNextBranchRead)
            recordViWriteForBranch(writtenVi, oldVi);

        m_state.vf[0][0] = 0.0f;
        m_state.vf[0][1] = 0.0f;
        m_state.vf[0][2] = 0.0f;
        m_state.vf[0][3] = 1.0f;
        m_state.vi[0] = 0;

        uint32_t nextPc = m_state.pc + 8u;
        if (nextPc >= codeSize)
            nextPc = 0u;
        m_state.pc = nextPc;

        if (m_state.branchPending)
        {
            if (m_state.branchDelay == 0u)
            {
                m_state.pc = m_state.branchTarget & microAddressMask();
                m_state.branchPending = false;
            }
            else
            {
                --m_state.branchDelay;
            }
        }

        const bool dHalt = decoded.dBit && m_state.dBitEnabled;
        const bool tHalt = decoded.tBit && m_state.tBitEnabled;
        const bool haltBit = dHalt || tHalt;
        const bool haltBranch = haltBit && decoded.lowerUsage.pipeline == PipelineBranch;

        if (m_state.haltAfterDelaySlot)
        {
            m_state.stoppedByD = m_pendingHaltD;
            m_state.stoppedByT = m_pendingHaltT;
            programEnded = true;
        }
        else if (m_state.ebit)
            programEnded = true;
        else if (haltBit && !haltBranch)
        {
            m_state.stoppedByD = dHalt;
            m_state.stoppedByT = tHalt;
            programEnded = true;
        }
        else if (decoded.eBit)
            m_state.ebit = true;
        else if (haltBranch)
        {
            m_state.haltAfterDelaySlot = true;
            m_pendingHaltD = dHalt;
            m_pendingHaltT = tHalt;
        }

        advanceOneCycle();
        if (programEnded)
            break;
    }

    static bool dumpedBudgetTrace = false;
    if (!dumpedBudgetTrace && m_cycle >= budgetEnd && !m_stopRequested)
    {
        dumpedBudgetTrace = true;
        const uint32_t retained = std::min(budgetTraceCount, 32u);
        const uint32_t first = budgetTraceCount - retained;
        std::fprintf(stderr, "[vu1:budget-trace] entries=%u\n", retained);
        for (uint32_t index = 0; index < retained; ++index)
        {
            const BudgetTraceEntry &entry = budgetTrace[(first + index) & 31u];
            std::fprintf(stderr,
                         "[vu1:budget-pair] pc=0x%04x lower=%08x upper=%08x "
                         "vi=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                         entry.pc, entry.lower, entry.upper,
                         entry.vi[1], entry.vi[2], entry.vi[3], entry.vi[4],
                         entry.vi[5], entry.vi[6], entry.vi[7], entry.vi[8],
                         entry.vi[9], entry.vi[10], entry.vi[11], entry.vi[12],
                         entry.vi[13], entry.vi[14], entry.vi[15]);
        }
        for (uint32_t pc = 0xB90u; pc <= 0xE98u && pc + 8u <= codeSize; pc += 8u)
        {
            uint32_t lower = 0;
            uint32_t upper = 0;
            std::memcpy(&lower, vuCode + pc, sizeof(lower));
            std::memcpy(&upper, vuCode + pc + 4u, sizeof(upper));
            std::fprintf(stderr, "[vu1:budget-code] pc=0x%04x lower=%08x upper=%08x\n",
                         pc, lower, upper);
        }
        for (uint32_t qword = 0x2E0u; qword <= 0x31Fu; ++qword)
        {
            uint32_t words[4]{};
            std::memcpy(words, vuData + qword * 16u, sizeof(words));
            std::fprintf(stderr,
                         "[vu1:budget-data] qword=0x%03x words=%08x,%08x,%08x,%08x\n",
                         qword, words[0], words[1], words[2], words[3]);
        }
    }

    if (programEnded)
    {
        flushPipelines();
        m_state.ebit = false;
        m_state.haltAfterDelaySlot = false;
        m_pendingHaltD = false;
        m_pendingHaltT = false;
    }
    m_running = !programEnded && !m_stopRequested;
    m_state.cycles = m_cycle;
    if (useVuRounding && previousRoundingMode != -1)
        std::fesetround(previousRoundingMode);
}
