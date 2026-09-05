#include "runtime/ps2_vu1.h"
#include "ps2_vu1_detail.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <tuple>
#include <utility>

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

#if defined(PS2X_ENABLE_VU_NATIVE_BLOCKS)
template <uint32_t Lower, uint32_t Upper>
struct VUNativePairWords
{
    static constexpr uint32_t lower = Lower;
    static constexpr uint32_t upper = Upper;
};

struct VUNativeUpperUsage
{
    std::array<uint8_t, 2> readRegs{};
    std::array<uint8_t, 2> readLanes{};
    uint8_t readCount = 0u;
    uint8_t writeReg = 0u;
    uint8_t writeLanes = 0u;
    uint8_t accRead = 0u;
    uint8_t accWrite = 0u;
    bool valid = false;
};

enum class VUNativeLowerKind : uint8_t
{
    Invalid,
    Nop,
    Immediate,
    Regular,
    Branch,
};

struct VUNativeLowerUsage
{
    VUNativeLowerKind kind = VUNativeLowerKind::Invalid;
    uint16_t viRead = 0u;
    uint8_t viWriteReg = 0u;
    uint8_t vfReadReg = 0u;
    uint8_t vfReadLanes = 0u;
    uint8_t vfWriteReg = 0u;
    uint8_t vfWriteLanes = 0u;
    bool delaysNextBranchRead = false;
    bool queuesStore = false;
    bool valid = false;
};

template <uint32_t Lower, uint32_t Upper>
consteval VUNativeLowerUsage nativeLowerUsage()
{
    VUNativeLowerUsage usage{};
    if constexpr ((Upper & 0x80000000u) != 0u)
    {
        usage.kind = VUNativeLowerKind::Immediate;
        usage.valid = true;
        return usage;
    }
    if constexpr (Lower == 0u || Lower == 0x8000033Cu)
    {
        usage.kind = VUNativeLowerKind::Nop;
        usage.valid = true;
        return usage;
    }

    constexpr uint8_t op = static_cast<uint8_t>((Lower >> 25u) & 0x7Fu);
    constexpr uint8_t vfT = static_cast<uint8_t>((Lower >> 16u) & 0x1Fu);
    constexpr uint8_t vfS = static_cast<uint8_t>((Lower >> 11u) & 0x1Fu);
    constexpr uint8_t viT = static_cast<uint8_t>((Lower >> 16u) & 0xFu);
    constexpr uint8_t viS = static_cast<uint8_t>((Lower >> 11u) & 0xFu);
    constexpr uint8_t viD = static_cast<uint8_t>((Lower >> 6u) & 0xFu);
    constexpr uint8_t dest = static_cast<uint8_t>((Lower >> 21u) & 0xFu);
    const auto readVi = [&usage](uint8_t reg)
    {
        if (reg != 0u)
            usage.viRead |= static_cast<uint16_t>(1u << reg);
    };
    const auto writeVi = [&usage](uint8_t reg)
    {
        if (reg != 0u)
            usage.viWriteReg = reg;
    };
    const auto readVf = [&usage](uint8_t reg, uint8_t lanes)
    {
        if (lanes != 0u)
        {
            usage.vfReadReg = reg;
            usage.vfReadLanes = lanes;
        }
    };
    const auto writeVf = [&usage](uint8_t reg, uint8_t lanes)
    {
        if (reg != 0u && lanes != 0u)
        {
            usage.vfWriteReg = reg;
            usage.vfWriteLanes = lanes;
        }
    };

    usage.kind = VUNativeLowerKind::Regular;
    usage.valid = true;
    if constexpr (op == 0x00u) // LQ
    {
        readVi(viS);
        writeVf(vfT, dest);
    }
    else if constexpr (op == 0x01u) // SQ
    {
        readVi(viT);
        readVf(vfS, dest);
        usage.queuesStore = true;
    }
    else if constexpr (op == 0x05u) // ISW
    {
        readVi(viS);
        readVi(viT);
        usage.queuesStore = true;
    }
    else if constexpr (op == 0x08u || op == 0x09u) // IADDIU / ISUBIU
    {
        readVi(viS);
        writeVi(viT);
        usage.delaysNextBranchRead = true;
    }
    else if constexpr (op == 0x20u || op == 0x21u || op == 0x24u ||
                       op == 0x25u || op == 0x28u || op == 0x29u ||
                       (op >= 0x2Cu && op <= 0x2Fu))
    {
        usage.kind = VUNativeLowerKind::Branch;
        if constexpr (op == 0x21u || op == 0x25u)
            writeVi(viT);
        if constexpr (op == 0x24u || op == 0x25u ||
                      (op >= 0x2Cu && op <= 0x2Fu))
            readVi(viS);
        if constexpr (op == 0x28u || op == 0x29u)
        {
            readVi(viS);
            readVi(viT);
        }
    }
    else if constexpr (op == 0x40u)
    {
        constexpr uint8_t direct = static_cast<uint8_t>(Lower & 0x3Fu);
        if constexpr (direct == 0x30u || direct == 0x31u ||
                      direct == 0x34u || direct == 0x35u)
        {
            readVi(viS);
            readVi(viT);
            writeVi(viD);
            usage.delaysNextBranchRead = true;
        }
        else if constexpr (direct == 0x32u)
        {
            readVi(viS);
            writeVi(viT);
            usage.delaysNextBranchRead = true;
        }
        else if constexpr (direct >= 0x3Cu)
        {
            constexpr uint8_t special = static_cast<uint8_t>(
                (Lower & 3u) | ((Lower >> 4u) & 0x7Cu));
            if constexpr (special == 0x30u || special == 0x31u) // MOVE / MR32
            {
                readVf(vfS, special == 0x31u ? 0xFu : dest);
                writeVf(vfT, dest);
            }
            else if constexpr (special == 0x34u || special == 0x36u) // LQI / LQD
            {
                readVi(viS);
                writeVi(viS);
                writeVf(vfT, dest);
                usage.delaysNextBranchRead = true;
            }
            else if constexpr (special == 0x35u || special == 0x37u) // SQI / SQD
            {
                readVi(viT);
                writeVi(viT);
                readVf(vfS, dest);
                usage.delaysNextBranchRead = true;
                usage.queuesStore = true;
            }
            else if constexpr (special == 0x3Cu) // MTIR
            {
                readVf(vfS, laneForComponent((Lower >> 21u) & 3u));
                writeVi(viT);
                usage.delaysNextBranchRead = true;
            }
            else if constexpr (special == 0x3Du) // MFIR
            {
                readVi(viS);
                writeVf(vfT, dest);
            }
            else if constexpr (special == 0x3Fu) // ISWR
            {
                readVi(viS);
                readVi(viT);
                usage.queuesStore = true;
            }
            else if constexpr (special == 0x40u || special == 0x41u) // RNEXT / RGET
            {
                writeVf(vfT, dest);
            }
            else if constexpr (special == 0x42u || special == 0x43u) // RINIT / RXOR
            {
                readVf(vfS, laneForComponent((Lower >> 21u) & 3u));
            }
            else if constexpr (special == 0x68u || special == 0x69u) // XTOP / XITOP
            {
                writeVi(viT);
            }
            else
            {
                usage.valid = false;
            }
        }
        else
        {
            usage.valid = false;
        }
    }
    else
    {
        usage.valid = false;
    }
    return usage;
}

template <uint32_t Upper>
consteval VUNativeUpperUsage nativeUpperUsage()
{
    VUNativeUpperUsage usage{};
    constexpr uint8_t op = static_cast<uint8_t>(Upper & 0x3Fu);
    constexpr uint8_t dest = static_cast<uint8_t>((Upper >> 21u) & 0xFu);
    constexpr uint8_t ft = static_cast<uint8_t>((Upper >> 16u) & 0x1Fu);
    constexpr uint8_t fs = static_cast<uint8_t>((Upper >> 11u) & 0x1Fu);
    constexpr uint8_t fd = static_cast<uint8_t>((Upper >> 6u) & 0x1Fu);
    const auto addRead = [&usage](uint8_t reg, uint8_t lanes)
    {
        if (lanes == 0u)
            return;
        usage.readRegs[usage.readCount] = reg;
        usage.readLanes[usage.readCount] = lanes;
        ++usage.readCount;
    };

    if constexpr (op <= 0x2Fu)
    {
        usage.valid = true;
        addRead(fs, dest);
        if constexpr (fd != 0u && dest != 0u)
        {
            usage.writeReg = fd;
            usage.writeLanes = dest;
        }
        if constexpr (op <= 0x1Bu)
            addRead(ft, laneForComponent(op & 3u));
        else if constexpr (op >= 0x28u)
            addRead(ft, op == 0x2Eu ? 0xEu : dest);
        if constexpr (op == 0x08u || op == 0x09u || op == 0x0Au || op == 0x0Bu ||
                      op == 0x0Cu || op == 0x0Du || op == 0x0Eu || op == 0x0Fu ||
                      op == 0x21u || op == 0x23u || op == 0x25u || op == 0x27u ||
                      op == 0x29u || op == 0x2Du || op == 0x2Eu)
        {
            usage.accRead = dest;
        }
        return usage;
    }

    if constexpr (op >= 0x3Cu)
    {
        constexpr uint8_t special =
            static_cast<uint8_t>((Upper & 3u) | ((Upper >> 4u) & 0x7Cu));
        constexpr bool writesAcc = special <= 0x0Fu ||
            (special >= 0x18u && special <= 0x1Cu) || special == 0x1Eu ||
            (special >= 0x20u && special <= 0x2Au) ||
            (special >= 0x2Cu && special <= 0x2Eu);
        constexpr bool writesVf =
            (special >= 0x10u && special <= 0x17u) || special == 0x1Du;
        constexpr bool writesClip = special == 0x1Fu;
        constexpr bool isNop = special == 0x2Fu || special == 0x30u;
        usage.valid = writesAcc || writesVf || writesClip || isNop;
        if constexpr (writesAcc)
        {
            addRead(fs, dest);
            if constexpr (special <= 0x1Bu)
                addRead(ft, laneForComponent(special & 3u));
            else if constexpr (special >= 0x28u && special <= 0x2Eu)
                addRead(ft, special == 0x2Eu ? 0xEu : dest);
            usage.accWrite = dest;
            if constexpr ((special >= 0x08u && special <= 0x0Fu) ||
                          special == 0x21u || special == 0x23u ||
                          special == 0x25u || special == 0x27u ||
                          special == 0x29u || special == 0x2Du)
            {
                usage.accRead = dest;
            }
        }
        else if constexpr (writesVf)
        {
            addRead(fs, dest);
            if constexpr (ft != 0u && dest != 0u)
            {
                usage.writeReg = ft;
                usage.writeLanes = dest;
            }
        }
        else if constexpr (writesClip)
        {
            addRead(fs, 0xEu);
            addRead(ft, 0x1u);
        }
    }
    return usage;
}

template <typename... Words>
consteval bool nativeBlockInternalHazardsSafe()
{
    constexpr size_t Count = sizeof...(Words);
    constexpr std::array<VUNativeUpperUsage, Count> upperUsages = {
        nativeUpperUsage<Words::upper>()...};
    constexpr std::array<VUNativeLowerUsage, Count> lowerUsages = {
        nativeLowerUsage<Words::lower, Words::upper>()...};
    std::array<std::array<uint8_t, 4>, 32> vfReady{};
    std::array<uint8_t, 4> accReady{};
    std::array<uint8_t, 16> viReady{};
    for (size_t index = 0u; index < Count; ++index)
    {
        const auto &upper = upperUsages[index];
        const auto &lower = lowerUsages[index];
        if (lower.kind == VUNativeLowerKind::Branch && index + 2u != Count)
            return false;
        for (uint32_t readIndex = 0u; readIndex < upper.readCount; ++readIndex)
        {
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((upper.readLanes[readIndex] & laneForComponent(component)) != 0u &&
                    vfReady[upper.readRegs[readIndex]][component] > index)
                {
                    return false;
                }
            }
        }
        for (uint32_t component = 0u; component < 4u; ++component)
        {
            if ((upper.accRead & laneForComponent(component)) != 0u &&
                accReady[component] > index)
            {
                return false;
            }
        }
        for (uint32_t component = 0u; component < 4u; ++component)
        {
            if ((lower.vfReadLanes & laneForComponent(component)) != 0u &&
                vfReady[lower.vfReadReg][component] > index)
            {
                return false;
            }
        }
        for (uint32_t regs = lower.viRead; regs != 0u; regs &= regs - 1u)
        {
            const uint32_t reg = static_cast<uint32_t>(std::countr_zero(regs));
            if (viReady[reg] > index)
                return false;
        }

        if (upper.writeReg != 0u)
        {
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((upper.writeLanes & laneForComponent(component)) != 0u)
                    vfReady[upper.writeReg][component] =
                        static_cast<uint8_t>(index + 4u);
            }
        }
        const bool lowerSuppressed = lower.vfWriteReg != 0u &&
            lower.vfWriteReg == upper.writeReg;
        if (lower.vfWriteReg != 0u && !lowerSuppressed)
        {
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((lower.vfWriteLanes & laneForComponent(component)) != 0u)
                    vfReady[lower.vfWriteReg][component] =
                        static_cast<uint8_t>(index + 4u);
            }
        }
        for (uint32_t component = 0u; component < 4u; ++component)
        {
            if ((upper.accWrite & laneForComponent(component)) != 0u)
                accReady[component] = static_cast<uint8_t>(index + 1u);
        }
        if (lower.viWriteReg != 0u)
            viReady[lower.viWriteReg] = static_cast<uint8_t>(index + 1u);
    }
    return true;
}

struct VUNativeBlockAccess
{
private:
    template <typename Words, size_t Index>
    static bool matchesPair(const uint8_t *code, uint32_t startPc)
    {
        uint32_t lower = 0u;
        uint32_t upper = 0u;
        const uint8_t *pair = code + startPc + static_cast<uint32_t>(Index) * 8u;
        std::memcpy(&lower, pair, sizeof(lower));
        std::memcpy(&upper, pair + sizeof(lower), sizeof(upper));
        return lower == Words::lower && upper == Words::upper;
    }

    template <typename... Words, size_t... Index>
    static bool matchesPairs(const uint8_t *code, uint32_t startPc,
                             std::index_sequence<Index...>)
    {
        return (matchesPair<Words, Index>(code, startPc) && ...);
    }

    static void retireReadyFlags(VU1Interpreter *vu)
    {
        for (uint32_t slots = vu->m_flagPipelineMask; slots != 0u; slots &= slots - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(slots));
            auto &entry = vu->m_flagPipeline[slot];
            if (entry.readyCycle > vu->m_cycle)
                continue;
            if (entry.writesMac)
                vu->m_state.mac = entry.mac;
            if (entry.writesStatus)
            {
                const uint32_t current = entry.status & 0xFu;
                vu->m_state.status = (vu->m_state.status & 0xFF0u) |
                    current | ((current | entry.extraSticky) << 6u);
            }
            if (entry.writesSticky)
                vu->m_state.status = (vu->m_state.status & 0x03Fu) | (entry.status & 0xFC0u);
            if (entry.writesClip)
                vu->m_state.clip = entry.clip;
            entry = {};
            vu->m_flagPipelineMask &= ~(1u << slot);
        }
    }

    static void retireReadyVf(VU1Interpreter *vu)
    {
        for (uint32_t slots = vu->m_vfWritePipelineMask; slots != 0u; slots &= slots - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(slots));
            auto &write = vu->m_vfWritePipeline[slot];
            if (write.readyCycle > vu->m_cycle)
                continue;
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((write.laneMask & laneForComponent(component)) != 0u &&
                    vu->m_vfLatestWrite[write.reg][component] == write.sequence)
                {
                    vu->m_state.vf[write.reg][component] = write.value[component];
                }
            }
            write = {};
            vu->m_vfWritePipelineMask &= ~(1u << slot);
        }
    }

    static void retireReadyStores(VU1Interpreter *vu)
    {
        for (uint32_t slots = vu->m_storePipelineMask; slots != 0u; slots &= slots - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(slots));
            auto &store = vu->m_storePipeline[slot];
            if (store.readyCycle > vu->m_cycle)
                continue;
            if (vu->m_activeVuData && store.address + 16u <= vu->m_activeVuDataSize)
            {
                uint32_t words[4]{};
                std::memcpy(words, vu->m_activeVuData + store.address, sizeof(words));
                for (uint32_t component = 0u; component < 4u; ++component)
                {
                    if ((store.laneMask & laneForComponent(component)) != 0u)
                        words[component] = store.words[component];
                }
                std::memcpy(vu->m_activeVuData + store.address, words, sizeof(words));
            }
            store = {};
            vu->m_storePipelineMask &= ~(1u << slot);
        }
    }

    template <size_t Index, typename Words>
    static void executeFastPair(
        VU1Interpreter *vu, uint64_t blockEndCycle,
        uint8_t upperVfSlot, uint8_t lowerVfSlot)
    {
        constexpr VUNativeUpperUsage upperUsage = nativeUpperUsage<Words::upper>();
        constexpr VUNativeLowerUsage lowerUsage =
            nativeLowerUsage<Words::lower, Words::upper>();
        constexpr uint32_t upperLatency = VU1Interpreter::kFmacLatency;
        constexpr bool lowerSuppressed = lowerUsage.vfWriteReg != 0u &&
            lowerUsage.vfWriteReg == upperUsage.writeReg;
        constexpr bool hasLowerWrite = lowerUsage.vfWriteReg != 0u && !lowerSuppressed;
        constexpr bool lowerReadsUpper = upperUsage.writeReg != 0u &&
            lowerUsage.vfReadLanes != 0u &&
            lowerUsage.vfReadReg == upperUsage.writeReg &&
            (lowerUsage.vfReadLanes & upperUsage.writeLanes) != 0u;
        constexpr bool upperShadowed = lowerSuppressed || lowerReadsUpper;
        const bool deferUpper = upperUsage.writeReg != 0u &&
            vu->m_cycle + upperLatency > blockEndCycle;
        const bool deferLower = hasLowerWrite &&
            vu->m_cycle + upperLatency > blockEndCycle;
        float oldUpper[4]{};
        float oldLower[4]{};
        if (upperUsage.writeReg != 0u && (deferUpper || upperShadowed))
            std::memcpy(oldUpper, vu->m_state.vf[upperUsage.writeReg], sizeof(oldUpper));
        if constexpr (hasLowerWrite)
        {
            if (deferLower)
                std::memcpy(oldLower, vu->m_state.vf[lowerUsage.vfWriteReg], sizeof(oldLower));
        }
        const int32_t oldVi = lowerUsage.viWriteReg != 0u
            ? vu->m_state.vi[lowerUsage.viWriteReg] : 0;

        ++vu->m_pairCounters.native;
        vu->template execUpperNative<Words::upper>();
        if constexpr ((Words::upper & 0x80000000u) != 0u)
        {
            float immediate = 0.0f;
            constexpr uint32_t immediateBits = Words::lower;
            std::memcpy(&immediate, &immediateBits, sizeof(immediate));
            vu->m_state.i = vu->normalizeOperand(immediate);
        }
        else if constexpr (upperShadowed)
        {
            float upperValue[4]{};
            std::memcpy(upperValue, vu->m_state.vf[upperUsage.writeReg], sizeof(upperValue));
            std::memcpy(vu->m_state.vf[upperUsage.writeReg], oldUpper, sizeof(oldUpper));
            vu->template execLowerNative<Words::lower>(
                vu->m_activeVuData, vu->m_activeVuDataSize,
                *vu->m_activeGs, vu->m_activeMemory, Words::upper);
            std::memcpy(vu->m_state.vf[upperUsage.writeReg], upperValue, sizeof(upperValue));
        }
        else
        {
            vu->template execLowerNative<Words::lower>(
                vu->m_activeVuData, vu->m_activeVuDataSize,
                *vu->m_activeGs, vu->m_activeMemory, Words::upper);
        }
        vu->m_viBranchBackupValid = false;

        if (upperUsage.writeReg != 0u)
        {
            const uint64_t sequence = ++vu->m_nextWriteSequence;
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((upperUsage.writeLanes & laneForComponent(component)) != 0u)
                {
                    vu->m_vfLatestWrite[upperUsage.writeReg][component] = sequence;
                    vu->m_vfReady[upperUsage.writeReg][component] = vu->m_cycle + upperLatency;
                }
            }
            if (deferUpper)
            {
                float value[4]{};
                std::memcpy(value, vu->m_state.vf[upperUsage.writeReg], sizeof(value));
                std::memcpy(vu->m_state.vf[upperUsage.writeReg], oldUpper, sizeof(oldUpper));
                auto &pending = vu->m_vfWritePipeline[upperVfSlot];
                pending = {};
                pending.valid = true;
                pending.readyCycle = vu->m_cycle + upperLatency;
                pending.sequence = sequence;
                pending.reg = upperUsage.writeReg;
                pending.laneMask = upperUsage.writeLanes;
                std::copy(value, value + 4, pending.value.begin());
                vu->m_vfWritePipelineMask |= 1u << upperVfSlot;
            }
        }
        if constexpr (hasLowerWrite)
        {
            const uint64_t sequence = ++vu->m_nextWriteSequence;
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((lowerUsage.vfWriteLanes & laneForComponent(component)) != 0u)
                {
                    vu->m_vfLatestWrite[lowerUsage.vfWriteReg][component] = sequence;
                    vu->m_vfReady[lowerUsage.vfWriteReg][component] =
                        vu->m_cycle + upperLatency;
                }
            }
            if (deferLower)
            {
                float value[4]{};
                std::memcpy(value, vu->m_state.vf[lowerUsage.vfWriteReg], sizeof(value));
                std::memcpy(vu->m_state.vf[lowerUsage.vfWriteReg], oldLower, sizeof(oldLower));
                auto &pending = vu->m_vfWritePipeline[lowerVfSlot];
                pending = {};
                pending.valid = true;
                pending.readyCycle = vu->m_cycle + upperLatency;
                pending.sequence = sequence;
                pending.reg = lowerUsage.vfWriteReg;
                pending.laneMask = lowerUsage.vfWriteLanes;
                std::copy(value, value + 4, pending.value.begin());
                vu->m_vfWritePipelineMask |= 1u << lowerVfSlot;
            }
        }
        if (upperUsage.accWrite != 0u)
        {
            const uint64_t sequence = ++vu->m_nextWriteSequence;
            for (uint32_t component = 0u; component < 4u; ++component)
            {
                if ((upperUsage.accWrite & laneForComponent(component)) != 0u)
                {
                    vu->m_accLatestWrite[component] = sequence;
                    vu->m_accReady[component] = vu->m_cycle + VU1Interpreter::kAccForwardLatency;
                }
            }
        }
        uint64_t viSequence = 0u;
        int32_t newVi = 0;
        if (lowerUsage.viWriteReg != 0u)
        {
            newVi = vu->m_state.vi[lowerUsage.viWriteReg];
            vu->m_state.vi[lowerUsage.viWriteReg] = oldVi;
            viSequence = ++vu->m_nextWriteSequence;
            vu->m_viLatestWrite[lowerUsage.viWriteReg] = viSequence;
            vu->m_viReady[lowerUsage.viWriteReg] = vu->m_cycle + 1u;
            if (lowerUsage.delaysNextBranchRead)
                vu->recordViWriteForBranch(lowerUsage.viWriteReg, oldVi);
        }

        vu->m_state.vf[0][0] = 0.0f;
        vu->m_state.vf[0][1] = 0.0f;
        vu->m_state.vf[0][2] = 0.0f;
        vu->m_state.vf[0][3] = 1.0f;
        vu->m_state.vi[0] = 0;
        vu->m_state.pc = (vu->m_state.pc + 8u) & vu->microAddressMask();
        if (vu->m_state.branchPending)
        {
            if (vu->m_state.branchDelay == 0u)
            {
                vu->m_state.pc = vu->m_state.branchTarget & vu->microAddressMask();
                vu->m_state.branchPending = false;
            }
            else
            {
                --vu->m_state.branchDelay;
            }
        }
        ++vu->m_cycle;
        vu->m_state.cycles = vu->m_cycle;
        retireReadyFlags(vu);
        if constexpr (lowerUsage.queuesStore)
            retireReadyStores(vu);
        retireReadyVf(vu);
        if (lowerUsage.viWriteReg != 0u &&
            vu->m_viLatestWrite[lowerUsage.viWriteReg] == viSequence)
        {
            vu->m_state.vi[lowerUsage.viWriteReg] = static_cast<int16_t>(newVi);
        }
    }

    template <typename PairTuple, size_t... Index>
    static void executeFastPairs(
        VU1Interpreter *vu, uint64_t blockEndCycle,
        const std::array<uint8_t, sizeof...(Index)> &upperVfSlots,
        const std::array<uint8_t, sizeof...(Index)> &lowerVfSlots,
                                 std::index_sequence<Index...>)
    {
        (executeFastPair<Index, std::tuple_element_t<Index, PairTuple>>(
             vu, blockEndCycle, upperVfSlots[Index], lowerVfSlots[Index]), ...);
    }

    template <size_t Index, typename Words, size_t Count>
    static bool prepareFastPair(
        VU1Interpreter *vu, uint64_t startCycle,
        uint32_t &virtualVfMask,
        std::array<uint64_t, VU1Interpreter::kMaxPendingVfWrites> &virtualVfReady,
        std::array<uint64_t, 16> &virtualViReady,
        std::array<uint8_t, Count> &upperVfSlots,
        std::array<uint8_t, Count> &lowerVfSlots)
    {
        constexpr VUNativeUpperUsage upperUsage = nativeUpperUsage<Words::upper>();
        constexpr VUNativeLowerUsage lowerUsage =
            nativeLowerUsage<Words::lower, Words::upper>();
        constexpr bool lowerSuppressed = lowerUsage.vfWriteReg != 0u &&
            lowerUsage.vfWriteReg == upperUsage.writeReg;
        const uint64_t issueCycle = startCycle + Index;

        for (uint32_t slots = virtualVfMask; slots != 0u; slots &= slots - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(slots));
            if (virtualVfReady[slot] <= issueCycle)
                virtualVfMask &= ~(1u << slot);
        }
        const auto reserveVfSlot = [&](uint8_t &result)
        {
            constexpr uint32_t validSlots =
                (1u << VU1Interpreter::kMaxPendingVfWrites) - 1u;
            const uint32_t available = (~virtualVfMask) & validSlots;
            if (available == 0u)
                return false;
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(available));
            result = static_cast<uint8_t>(slot);
            virtualVfMask |= 1u << slot;
            virtualVfReady[slot] = issueCycle + VU1Interpreter::kFmacLatency;
            return true;
        };
        if constexpr (upperUsage.writeReg != 0u)
        {
            if (!reserveVfSlot(upperVfSlots[Index]))
                return false;
        }
        if constexpr (lowerUsage.vfWriteReg != 0u && !lowerSuppressed)
        {
            if (!reserveVfSlot(lowerVfSlots[Index]))
                return false;
        }
        for (uint32_t readIndex = 0u; readIndex < upperUsage.readCount; ++readIndex)
        {
            constexpr uint32_t componentCount = 4u;
            const uint8_t readReg = upperUsage.readRegs[readIndex];
            const uint8_t readLanes = upperUsage.readLanes[readIndex];
            for (uint32_t component = 0u; component < componentCount; ++component)
            {
                if ((readLanes & laneForComponent(component)) != 0u &&
                    vu->m_vfReady[readReg][component] > issueCycle)
                {
                    return false;
                }
            }
        }
        for (uint32_t component = 0u; component < 4u; ++component)
        {
            if ((lowerUsage.vfReadLanes & laneForComponent(component)) != 0u &&
                vu->m_vfReady[lowerUsage.vfReadReg][component] > issueCycle)
            {
                return false;
            }
        }
        for (uint32_t component = 0u; component < 4u; ++component)
        {
            if ((upperUsage.accRead & laneForComponent(component)) != 0u &&
                vu->m_accReady[component] > issueCycle)
            {
                return false;
            }
        }
        for (uint32_t regs = lowerUsage.viRead; regs != 0u; regs &= regs - 1u)
        {
            const uint32_t reg = static_cast<uint32_t>(std::countr_zero(regs));
            if (virtualViReady[reg] > issueCycle)
                return false;
        }
        if constexpr (lowerUsage.viWriteReg != 0u)
            virtualViReady[lowerUsage.viWriteReg] = issueCycle + 1u;
        return true;
    }

    template <typename PairTuple, size_t Count, size_t... Index>
    static bool prepareFastPairs(
        VU1Interpreter *vu, uint64_t startCycle,
        uint32_t &virtualVfMask,
        std::array<uint64_t, VU1Interpreter::kMaxPendingVfWrites> &virtualVfReady,
        std::array<uint64_t, 16> &virtualViReady,
        std::array<uint8_t, Count> &upperVfSlots,
        std::array<uint8_t, Count> &lowerVfSlots,
        std::index_sequence<Index...>)
    {
        return (prepareFastPair<Index, std::tuple_element_t<Index, PairTuple>>(
                    vu, startCycle, virtualVfMask, virtualVfReady,
                    virtualViReady, upperVfSlots, lowerVfSlots) && ...);
    }

    template <typename... Words>
    static bool canExecuteFast(
        VU1Interpreter *vu, uint64_t budgetEnd,
        std::array<uint8_t, sizeof...(Words)> &upperVfSlots,
        std::array<uint8_t, sizeof...(Words)> &lowerVfSlots)
    {
        static constexpr size_t Count = sizeof...(Words);
        upperVfSlots.fill(0xFFu);
        lowerVfSlots.fill(0xFFu);
        static_assert((nativeUpperUsage<Words::upper>().valid && ...));
        static_assert((nativeLowerUsage<Words::lower, Words::upper>().valid && ...));
        if constexpr (!nativeBlockInternalHazardsSafe<Words...>())
            return false;
        if constexpr ((((Words::upper & 0x58000000u) != 0u) || ...))
            return false;
        if (budgetEnd - vu->m_cycle < Count || vu->m_accWritePipelineMask != 0u ||
            vu->m_storePipelineMask != 0u || vu->m_viWritePipelineMask != 0u ||
            vu->m_fdiv.valid || vu->m_efu[0].valid || vu->m_efu[1].valid ||
            vu->m_xgkick.active || vu->m_state.branchPending || vu->m_state.ebit ||
            vu->m_state.haltAfterDelaySlot)
        {
            return false;
        }

        uint32_t virtualVfMask = vu->m_vfWritePipelineMask;
        std::array<uint64_t, VU1Interpreter::kMaxPendingVfWrites> virtualVfReady{};
        for (uint32_t slots = virtualVfMask; slots != 0u; slots &= slots - 1u)
        {
            const uint32_t slot = static_cast<uint32_t>(std::countr_zero(slots));
            virtualVfReady[slot] = vu->m_vfWritePipeline[slot].readyCycle;
        }
        std::array<uint64_t, 16> virtualViReady = vu->m_viReady;
        using PairTuple = std::tuple<Words...>;
        return prepareFastPairs<PairTuple>(
            vu, vu->m_cycle, virtualVfMask, virtualVfReady, virtualViReady,
            upperVfSlots, lowerVfSlots, std::make_index_sequence<Count>{});
    }

public:
    template <uint32_t StartPc, typename... PairWords>
        requires (sizeof...(PairWords) > 0u && sizeof...(PairWords) <= 32u)
    static uint32_t execute(VU1Interpreter *vu, const uint8_t *code, uint32_t codeSize,
                            uint8_t *vuData, uint32_t dataSize, GS &gs,
                            PS2Memory *memory, uint64_t budgetEnd)
    {
        static constexpr size_t pairCount = sizeof...(PairWords);
        if (vu->m_state.pc != StartPc ||
            StartPc + pairCount * 8u > codeSize)
        {
            return 0u;
        }
        if (!matchesPairs<PairWords...>(code, StartPc,
                                        std::make_index_sequence<pairCount>{}))
            return 0u;

        using PairTuple = std::tuple<PairWords...>;
        uint32_t executed = 0u;
        do
        {
            std::array<uint8_t, pairCount> upperVfSlots{};
            std::array<uint8_t, pairCount> lowerVfSlots{};
            if (!canExecuteFast<PairWords...>(
                    vu, budgetEnd, upperVfSlots, lowerVfSlots))
            {
                break;
            }
            const uint64_t blockEndCycle = vu->m_cycle + pairCount;
            executeFastPairs<PairTuple>(vu, blockEndCycle, upperVfSlots,
                                        lowerVfSlots,
                                        std::make_index_sequence<pairCount>{});
            executed += static_cast<uint32_t>(pairCount);
        } while (vu->m_state.pc == StartPc &&
                 budgetEnd - vu->m_cycle >= pairCount);
        return executed;
    }

    template <uint32_t StartPc, size_t OriginalCount, typename... PairWords>
        requires (OriginalCount > 0u && OriginalCount <= sizeof...(PairWords))
    static uint32_t executeExtended(
        VU1Interpreter *vu, const uint8_t *code, uint32_t codeSize,
        uint8_t *vuData, uint32_t dataSize, GS &gs,
        PS2Memory *memory, uint64_t budgetEnd)
    {
        const uint32_t extended = execute<StartPc, PairWords...>(
            vu, code, codeSize, vuData, dataSize, gs, memory, budgetEnd);
        if (extended != 0u || OriginalCount == sizeof...(PairWords))
            return extended;

        using PairTuple = std::tuple<PairWords...>;
        return [&]<size_t... Index>(std::index_sequence<Index...>)
        {
            return execute<StartPc, std::tuple_element_t<Index, PairTuple>...>(
                vu, code, codeSize, vuData, dataSize, gs, memory, budgetEnd);
        }(std::make_index_sequence<OriginalCount>{});
    }

};
#endif
