// Based on Blackline Interactive implementation
#include "runtime/ps2_memory.h"
#include "ps2_vif_trace.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;
    thread_local std::unordered_map<
        const uint8_t *, std::vector<Vif1TraceProvenance>> vif1TraceProvenance;
    thread_local const std::vector<Vif1TraceProvenance> *currentVif1TraceProvenance = nullptr;

    struct XmenTargetVifWriteConfig
    {
        bool enabled = false;
        uint32_t firstQword = 0u;
        uint32_t lastQword = 0u;
        uint32_t maxWrites = 512u;
        uint64_t minTick = 0u;
        bool matchAfterWord0 = false;
        uint32_t afterWordIndex = 0u;
        uint32_t afterWord0 = 0u;
    };

    const XmenTargetVifWriteConfig &xmenTargetVifWriteConfig()
    {
        static const XmenTargetVifWriteConfig config = []()
        {
            XmenTargetVifWriteConfig result{};
            const char *firstValue = std::getenv("PS2X_TRACE_VIF_DEST_FIRST");
            const char *lastValue = std::getenv("PS2X_TRACE_VIF_DEST_LAST");
            if (!firstValue || !lastValue || firstValue[0] == '\0' || lastValue[0] == '\0')
                return result;

            char *firstEnd = nullptr;
            char *lastEnd = nullptr;
            result.firstQword = static_cast<uint32_t>(std::strtoul(firstValue, &firstEnd, 0));
            result.lastQword = static_cast<uint32_t>(std::strtoul(lastValue, &lastEnd, 0));
            if (!firstEnd || firstEnd == firstValue || *firstEnd != '\0' ||
                !lastEnd || lastEnd == lastValue || *lastEnd != '\0' ||
                result.firstQword > result.lastQword)
            {
                return XmenTargetVifWriteConfig{};
            }

            const char *tickValue = std::getenv("PS2X_TRACE_VIF_MIN_TICK");
            if (tickValue && tickValue[0] != '\0')
            {
                char *tickEnd = nullptr;
                const uint64_t parsedTick = std::strtoull(tickValue, &tickEnd, 0);
                if (tickEnd && tickEnd != tickValue && *tickEnd == '\0')
                    result.minTick = parsedTick;
            }

            const char *maxWritesValue = std::getenv("PS2X_TRACE_VIF_MAX_WRITES");
            if (maxWritesValue && maxWritesValue[0] != '\0')
            {
                char *maxWritesEnd = nullptr;
                const unsigned long parsedMaxWrites =
                    std::strtoul(maxWritesValue, &maxWritesEnd, 0);
                if (maxWritesEnd && maxWritesEnd != maxWritesValue &&
                    *maxWritesEnd == '\0' && parsedMaxWrites > 0u)
                {
                    result.maxWrites = static_cast<uint32_t>(parsedMaxWrites);
                }
            }

            const char *afterWord0Value =
                std::getenv("PS2X_TRACE_VIF_AFTER_WORD0");
            if (afterWord0Value && afterWord0Value[0] != '\0')
            {
                char *afterWord0End = nullptr;
                const unsigned long parsedAfterWord0 =
                    std::strtoul(afterWord0Value, &afterWord0End, 0);
                if (afterWord0End && afterWord0End != afterWord0Value &&
                    *afterWord0End == '\0')
                {
                    result.matchAfterWord0 = true;
                    result.afterWord0 = static_cast<uint32_t>(parsedAfterWord0);
                }
            }
            const char *afterWordIndexValue =
                std::getenv("PS2X_TRACE_VIF_AFTER_WORD_INDEX");
            if (afterWordIndexValue && afterWordIndexValue[0] != '\0')
            {
                char *afterWordIndexEnd = nullptr;
                const unsigned long parsedAfterWordIndex =
                    std::strtoul(afterWordIndexValue, &afterWordIndexEnd, 0);
                if (afterWordIndexEnd &&
                    afterWordIndexEnd != afterWordIndexValue &&
                    *afterWordIndexEnd == '\0' && parsedAfterWordIndex < 4u)
                {
                    result.afterWordIndex =
                        static_cast<uint32_t>(parsedAfterWordIndex);
                }
            }
            result.enabled = true;
            return result;
        }();
        return config;
    }

    void traceVuQwords(const char *tag, uint32_t transferIndex, const uint8_t *vuData,
                       uint32_t dataSize, uint32_t baseQword, uint32_t count)
    {
        if (!vuData || dataSize == 0u)
            return;

        std::fprintf(stderr, "[vif1:%s] transfer=%u base=0x%x", tag, transferIndex, baseQword);
        for (uint32_t i = 0u; i < count; ++i)
        {
            uint32_t words[4]{};
            const uint32_t address = ((baseQword + i) * 16u) % dataSize;
            if (address + sizeof(words) <= dataSize)
                std::memcpy(words, vuData + address, sizeof(words));
            std::fprintf(stderr, " q%u=%08x,%08x,%08x,%08x",
                         i, words[0], words[1], words[2], words[3]);
        }
        std::fprintf(stderr, "\n");
    }

    uint32_t gifImageQwcFromTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        if (flg != kGifFmtImage)
            return 0u;

        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }
}

void registerVif1TraceProvenance(
    const uint8_t *data,
    std::vector<Vif1TraceProvenance> &&ranges)
{
    if (data && !ranges.empty())
        vif1TraceProvenance[data] = std::move(ranges);
}

void beginVif1TraceProvenance(const uint8_t *data)
{
    currentVif1TraceProvenance = nullptr;
    const auto entry = vif1TraceProvenance.find(data);
    if (entry != vif1TraceProvenance.end())
        currentVif1TraceProvenance = &entry->second;
}

void endVif1TraceProvenance(const uint8_t *data)
{
    currentVif1TraceProvenance = nullptr;
    vif1TraceProvenance.erase(data);
}

bool resolveVif1TraceProvenance(
    size_t streamOffset,
    uint32_t &sourceAddr,
    uint32_t &tagAddr)
{
    if (!currentVif1TraceProvenance)
        return false;

    for (const Vif1TraceProvenance &range : *currentVif1TraceProvenance)
    {
        if (streamOffset >= range.flatStart && streamOffset < range.flatEnd)
        {
            sourceAddr = range.sourceAddr +
                static_cast<uint32_t>(streamOffset - range.flatStart);
            tagAddr = range.tagAddr;
            return true;
        }
    }
    return false;
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

bool PS2Memory::dispatchPendingVu1Mscal()
{
    if (!m_vif1MscalPending)
        return false;

    // VIF1 has a one-entry microprogram queue.  Do not reset an active VU1
    // program when a later MSCAL becomes eligible to start.
    if (m_vu1ServiceCallback && m_vu1ServiceCallback(false))
        return false;

    const uint32_t startPC = m_vif1PendingMscalPc;
    const uint32_t top = m_vif1PendingMscalTop;
    const uint32_t itop = m_vif1PendingMscalItop;
    m_vif1MscalPending = false;
    m_vif1PendingMscalUnpacks = 0u;
    if (m_vu1MscalCallback)
        m_vu1MscalCallback(startPC, top, itop);
    return true;
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    static std::atomic<uint32_t> vif1TransferTraceCount{0u};
    const uint32_t transferIndex = vif1TransferTraceCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
    if (transferIndex <= 12u || (transferIndex & (transferIndex - 1u)) == 0u)
    {
        std::fprintf(stderr, "[vif1:transfer] index=%u bytes=%u head=", transferIndex, sizeBytes);
        const uint32_t headBytes = std::min<uint32_t>(sizeBytes, 32u);
        for (uint32_t i = 0u; i < headBytes; ++i)
            std::fprintf(stderr, "%02x", data[i]);
        std::fprintf(stderr, "\n");
    }

    uint32_t pos = 0;

    const auto drainVu1Pipeline = [&]() -> bool
    {
        dispatchPendingVu1Mscal();
        if (m_vu1ServiceCallback && m_vu1ServiceCallback(true))
            return false;

        // If an older program was running, the first dispatch attempt left
        // the queued MSCAL intact.  Start and finish it now that VU1 is idle.
        if (m_vif1MscalPending)
        {
            if (!dispatchPendingVu1Mscal())
                return false;
            if (m_vu1ServiceCallback && m_vu1ServiceCallback(true))
                return false;
        }
        return !m_vif1MscalPending;
    };

    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        static std::atomic<uint32_t> vif1CommandTraceCount{0u};
        const uint32_t commandIndex = vif1CommandTraceCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (commandIndex <= 160u)
        {
            std::fprintf(stderr,
                         "[vif1:command] index=%u transfer=%u pos=%u raw=0x%08x opcode=0x%02x num=%u imm=0x%04x\n",
                         commandIndex,
                         transferIndex,
                         pos - 4u,
                         cmd,
                         opcode,
                         num,
                         imm);
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            dispatchPendingVu1Mscal();
            if (m_vu1ServiceCallback)
                m_vu1ServiceCallback(false);
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            drainVu1Pipeline();
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            if (!drainVu1Pipeline())
            {
                std::fprintf(stderr,
                             "[vif1:mscal-stall] transfer=%u pc=0x%x previous VU1 program did not stop\n",
                             transferIndex, static_cast<uint32_t>(imm) * 8u);
                continue;
            }
            uint32_t startPC = (uint32_t)imm * 8u;

            // Values visible to the VU program for this MSCAL.
            // DobieStation semantics: ITOP = ITOPS; TOP = current TOPS;
            // then TOPS/DBF are prepared for the next buffer.
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            static std::atomic<uint32_t> vif1MscalTraceCount{0u};
            const uint32_t mscalTraceIndex = vif1MscalTraceCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool traceMscal = mscalTraceIndex <= 96u;
            if (traceMscal)
            {
                std::fprintf(stderr,
                             "[vif1:mscal] transfer=%u pc=0x%x top=0x%x itop=0x%x base=0x%x ofst=0x%x dbf=%u\n",
                             transferIndex, startPC, runTop, runItop,
                             vif1_regs.base, vif1_regs.ofst,
                             (vif1_regs.stat >> 7) & 1u);
                if (startPC == 0x230u)
                    traceVuQwords("mscal-data", transferIndex, m_vu1Data, PS2_VU1_DATA_SIZE, runTop, 8u);
            }

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            m_vif1MscalPending = true;
            m_vif1PendingMscalPc = startPC;
            m_vif1PendingMscalTop = runTop;
            m_vif1PendingMscalItop = runItop;
            m_vif1PendingMscalUnpacks = 0u;
            const bool followedByUnpack =
                pos + sizeof(uint32_t) <= sizeBytes &&
                (data[pos + 3u] & 0x60u) == 0x60u;
            if (opcode == VIF_MSCALF || followedByUnpack)
                dispatchPendingVu1Mscal();
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            if (!drainVu1Pipeline())
            {
                std::fprintf(stderr,
                             "[vif1:mscnt-stall] transfer=%u previous VU1 program did not stop\n",
                             transferIndex);
                continue;
            }
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            drainVu1Pipeline();
            uint32_t destAddr = (uint32_t)imm * 8u;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                uint32_t chunkPos = pos;
                uint32_t remainingQw = qwCount;

                if (m_vif1PendingPath2ImageQwc != 0u)
                {
                    const uint32_t chunkQw =
                        std::min<uint32_t>(m_vif1PendingPath2ImageQwc, remainingQw);
                    std::vector<uint8_t> imagePacket(
                        16u + static_cast<size_t>(chunkQw) * 16u, 0u);
                    const uint64_t imageTag =
                        static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                        ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                        (static_cast<uint64_t>(kGifFmtImage) << 58);
                    std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
                    std::memcpy(imagePacket.data() + 16u,
                                data + chunkPos,
                                static_cast<size_t>(chunkQw) * 16u);
                    submitGifPacket(GifPathId::Path2,
                                    imagePacket.data(),
                                    static_cast<uint32_t>(imagePacket.size()),
                                    true,
                                    m_vif1PendingPath2DirectHl);

                    chunkPos += chunkQw * 16u;
                    remainingQw -= chunkQw;
                    m_vif1PendingPath2ImageQwc -= chunkQw;
                    if (m_vif1PendingPath2ImageQwc == 0u)
                    {
                        m_vif1PendingPath2DirectHl = false;
                    }
                }

                if (remainingQw > 0u)
                {
                    submitGifPacket(GifPathId::Path2,
                                    data + chunkPos,
                                    remainingQw * 16u,
                                    true,
                                    directHl);

                    const uint32_t imageQw =
                        gifImageQwcFromTag(data + chunkPos, remainingQw * 16u);
                    if (imageQw != 0u)
                    {
                        const uint32_t inlineImageQw = remainingQw - 1u;
                        if (imageQw > inlineImageQw)
                        {
                            m_vif1PendingPath2ImageQwc = imageQw - inlineImageQw;
                            m_vif1PendingPath2DirectHl = directHl;
                        }
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            static std::atomic<uint32_t> vif1UnpackTraceCount{0u};
            const uint32_t unpackTraceIndex = vif1UnpackTraceCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
            const bool topRelative = (imm & 0x8000u) != 0u;
            const bool traceUnpack = unpackTraceIndex <= 192u;

            if (traceUnpack)
            {
                std::fprintf(stderr,
                             "[vif1:unpack] transfer=%u opcode=0x%02x vn=%u vl=%u mask=%u mode=%u usn=%u num=%u imm=0x%04x addr=0x%x tops=0x%x cl=%u wl=%u write-vectors=%u source-vectors=%u bytes=%u data=",
                             transferIndex, opcode, vn, vl, maskEnable ? 1u : 0u,
                             vif1_regs.mode & 3u, zeroExtend ? 1u : 0u,
                             writeVectorCount, imm, vuAddr, vif1_regs.tops & 0x3FFu,
                             cl, wl, writeVectorCount, sourceVectorCount, totalBytes);
                const uint32_t previewBytes = std::min<uint32_t>(
                    totalBytes, 96u);
                for (uint32_t i = 0u; i < previewBytes && pos + i < sizeBytes; ++i)
                    std::fprintf(stderr, "%02x", data[pos + i]);
                std::fprintf(stderr, "\n");
            }

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t previousLanes[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;
                    uint32_t sourceVectorIndex = UINT32_MAX;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        sourceVectorIndex = srcIndex;
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5 expands 5-bit RGB and 1-bit alpha into the upper bits of each lane.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = (packed & 0x001Fu) << 3u;
                        decompressed[1] = (packed & 0x03E0u) >> 2u;
                        decompressed[2] = (packed & 0x7C00u) >> 7u;
                        decompressed[3] = (packed & 0x8000u) >> 8u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // The VIF expands V2 to XYXY. V3's otherwise-indeterminate W lane
                    // overlaps the next packed source component; games rely on both
                    // behaviors, so preserve them when the overlapping bytes are present.
                    if (handledFormat && components == 2)
                    {
                        decompressed[2] = decompressed[0];
                        decompressed[3] = decompressed[1];
                    }
                    else if (handledFormat && components == 3)
                    {
                        const size_t fourthComponentOffset =
                            static_cast<size_t>(srcVec - data) +
                            3u * static_cast<size_t>(bitsPerComponent / 8);
                        const size_t fourthComponentBytes =
                            static_cast<size_t>(bitsPerComponent / 8);
                        if (fourthComponentOffset + fourthComponentBytes <= sizeBytes)
                        {
                            if (vl == 0u)
                            {
                                std::memcpy(&decompressed[3],
                                            data + fourthComponentOffset,
                                            sizeof(decompressed[3]));
                            }
                            else if (vl == 1u)
                            {
                                uint16_t raw = 0u;
                                std::memcpy(&raw, data + fourthComponentOffset, sizeof(raw));
                                decompressed[3] = extend16(raw);
                            }
                            else if (vl == 2u)
                            {
                                decompressed[3] = extend8(data[fourthComponentOffset]);
                            }
                        }
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));

                    const XmenTargetVifWriteConfig &targetConfig =
                        xmenTargetVifWriteConfig();
                    const uint64_t targetTick =
                        gs_regs.vsyncTick.load(std::memory_order_relaxed);
                    static uint32_t targetWriteTraceCount = 0u;
                    if (targetConfig.enabled && targetTick >= targetConfig.minTick &&
                        destVec >= targetConfig.firstQword &&
                        destVec <= targetConfig.lastQword &&
                        (!targetConfig.matchAfterWord0 ||
                         lanes[targetConfig.afterWordIndex] ==
                             targetConfig.afterWord0) &&
                        targetWriteTraceCount++ < targetConfig.maxWrites)
                    {
                        const size_t sourceStreamOffset = srcVec
                            ? static_cast<size_t>(srcVec - data)
                            : static_cast<size_t>(-1);
                        uint32_t sourceGuestAddress = UINT32_MAX;
                        uint32_t sourceTagAddress = UINT32_MAX;
                        if (srcVec)
                            resolveVif1TraceProvenance(
                                sourceStreamOffset, sourceGuestAddress, sourceTagAddress);
                        std::fprintf(stderr,
                                     "[xmen-vif1:target-write] tick=%llu transfer=%u unpack=%u "
                                     "write=%u source=%u dest=0x%x opcode=0x%02x vn=%u vl=%u "
                                     "mask=%u mode=%u usn=%u cl=%u wl=%u cyclePos=%u decoded=%u "
                                     "before=%08x,%08x,%08x,%08x after=%08x,%08x,%08x,%08x "
                                     "stream=0x%zx guest=%08x tag=%08x src=",
                                     static_cast<unsigned long long>(targetTick),
                                     transferIndex,
                                     unpackTraceIndex,
                                     writeIndex,
                                     sourceVectorIndex,
                                     destVec,
                                     opcode,
                                     vn,
                                     vl,
                                     maskEnable ? 1u : 0u,
                                     vif1_regs.mode & 3u,
                                     zeroExtend ? 1u : 0u,
                                     cl,
                                     wl,
                                     cyclePos,
                                     decoded ? 1u : 0u,
                                     previousLanes[0], previousLanes[1],
                                     previousLanes[2], previousLanes[3],
                                     lanes[0], lanes[1], lanes[2], lanes[3],
                                     sourceStreamOffset, sourceGuestAddress,
                                     sourceTagAddress);
                        if (srcVec)
                        {
                            for (uint32_t sourceByte = 0u;
                                 sourceByte < std::min<uint32_t>(bytesPerVector, 16u);
                                 ++sourceByte)
                            {
                                std::fprintf(stderr, "%02x", srcVec[sourceByte]);
                            }
                        }
                        std::fprintf(stderr, "\n");
                    }
                }

                if (traceUnpack)
                {
                    const uint32_t dumpCount = std::min<uint32_t>(writeVectorCount, 8u);
                    traceVuQwords("unpack-dst", transferIndex, m_vu1Data, PS2_VU1_DATA_SIZE, vuAddr, dumpCount);
                    if (topRelative && vuAddr != (vif1_regs.tops & 0x3FFu))
                        traceVuQwords("unpack-tops", transferIndex, m_vu1Data, PS2_VU1_DATA_SIZE, vif1_regs.tops & 0x3FFu, 8u);
                }
                bool dispatchedMscal = false;
                if (m_vif1MscalPending && ++m_vif1PendingMscalUnpacks > 3u)
                    dispatchedMscal = dispatchPendingVu1Mscal();
                if (!dispatchedMscal && m_vu1ServiceCallback)
                    m_vu1ServiceCallback(false);
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
    dispatchPendingVu1Mscal();
}
