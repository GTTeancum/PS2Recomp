#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/gs_frontend.h"
#include "ps2_log.h"
#include "ps2_vif_trace.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <vector>

extern "C" void ps2xRecordXmenVif1Chain(
    uint64_t tick, uint32_t start, uint32_t end, uint32_t tags,
    uint32_t bytes, uint32_t chcr, uint32_t ended);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace
{
    uint64_t gifDmacCompletionCycles(uint64_t qwc, uint64_t tagCount)
    {
        constexpr uint64_t kBusCyclesPerQword = 2u;
        constexpr uint64_t kCyclesPerTag = 2u;
        constexpr uint64_t kTerminalCycles = 16u;
        // Generated guest code accounts time in coarse checkpoints. Keep short
        // GIF transfers asynchronous long enough for chained packet producers
        // to reach a checkpoint before their completion handler can run.
        static const uint64_t kMinimumCompletionCycles = []()
        {
            const char *value = std::getenv("PS2X_GIF_DMAC_MIN_COMPLETION_CYCLES");
            return value ? std::strtoull(value, nullptr, 0) : 1024ull;
        }();
        const uint64_t transferCycles =
            qwc * kBusCyclesPerQword + tagCount * kCyclesPerTag + kTerminalCycles;
        return std::max(transferCycles, kMinimumCompletionCycles);
    }

    bool traceGifDmaChains()
    {
        static const bool enabled = std::getenv("PS2X_TRACE_GIF_DMA") != nullptr;
        return enabled;
    }

    bool traceVifDmaProvenance()
    {
        static const bool enabled = std::getenv("PS2X_TRACE_VIF_PROVENANCE") != nullptr;
        return enabled;
    }

    bool xmenDiagnosticsEnabled()
    {
        static const bool enabled = std::getenv("PS2X_XMEN_DIAGNOSTICS") != nullptr;
        return enabled;
    }

    uint32_t tracedGifDestinationBlock()
    {
        static const uint32_t block = []()
        {
            const char *value = std::getenv("PS2X_TRACE_GIF_DESTINATION_BLOCK");
            return value && value[0] != '\0'
                ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
                : 0u;
        }();
        return block;
    }

#if defined(_WIN32)
    struct RdramPageWatch
    {
        std::atomic<bool> enabled{false};
        std::atomic<bool> ready{false};
        uint8_t *rdram = nullptr;
        uint32_t guestAddress = 0u;
        uint32_t size = 0u;
        bool match32Enabled = false;
        bool match32Any = false;
        bool reportAnyWrite = false;
        uint32_t match32 = 0u;
        uint64_t armTick = 0u;
        uint8_t *page = nullptr;
        size_t pageSize = 0u;
        DWORD originalProtection = PAGE_READWRITE;
        PVOID handler = nullptr;
        uintptr_t instruction = 0u;
        uintptr_t accessAddress = 0u;
        uintptr_t moduleBase = 0u;
        DWORD threadId = 0u;
        USHORT frameCount = 0u;
        std::array<void *, 24> frames{};
        std::array<uint8_t, 256> before{};
        std::array<uint8_t, 256> after{};
    };

    RdramPageWatch rdramPageWatch;
    thread_local bool rdramPageWatchSingleStep = false;
    thread_local uintptr_t rdramPageWatchInstruction = 0u;
    thread_local uintptr_t rdramPageWatchAccessAddress = 0u;
    thread_local DWORD rdramPageWatchThreadId = 0u;
    thread_local USHORT rdramPageWatchFrameCount = 0u;
    thread_local std::array<void *, 24> rdramPageWatchFrames{};
    thread_local std::array<uint8_t, 256> rdramPageWatchBefore{};

    LONG CALLBACK handleRdramPageWatch(PEXCEPTION_POINTERS exception)
    {
        if (!exception || !exception->ExceptionRecord || !exception->ContextRecord)
            return EXCEPTION_CONTINUE_SEARCH;

        const DWORD code = exception->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION && rdramPageWatch.enabled.load())
        {
            if (exception->ExceptionRecord->NumberParameters < 2u ||
                exception->ExceptionRecord->ExceptionInformation[0] != 1u)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            const uintptr_t accessAddress =
                static_cast<uintptr_t>(exception->ExceptionRecord->ExceptionInformation[1]);
            const uintptr_t pageStart = reinterpret_cast<uintptr_t>(rdramPageWatch.page);
            if (accessAddress < pageStart ||
                accessAddress >= pageStart + rdramPageWatch.pageSize)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            std::memcpy(rdramPageWatchBefore.data(),
                        rdramPageWatch.rdram + rdramPageWatch.guestAddress,
                        rdramPageWatch.size);
#if defined(_M_X64)
            rdramPageWatchInstruction =
                static_cast<uintptr_t>(exception->ContextRecord->Rip);
#else
            rdramPageWatchInstruction =
                static_cast<uintptr_t>(exception->ContextRecord->Eip);
#endif
            rdramPageWatchAccessAddress = accessAddress;
            rdramPageWatchThreadId = GetCurrentThreadId();
            rdramPageWatchFrameCount = RtlCaptureStackBackTrace(
                0u, static_cast<DWORD>(rdramPageWatchFrames.size()),
                rdramPageWatchFrames.data(), nullptr);

            DWORD ignored = 0u;
            if (!VirtualProtect(rdramPageWatch.page, rdramPageWatch.pageSize,
                                PAGE_READWRITE, &ignored))
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            rdramPageWatchSingleStep = true;
            exception->ContextRecord->EFlags |= 0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (code == EXCEPTION_SINGLE_STEP && rdramPageWatchSingleStep)
        {
            rdramPageWatchSingleStep = false;
            exception->ContextRecord->EFlags &= ~0x100u;

            std::array<uint8_t, 256> after{};
            std::memcpy(after.data(),
                        rdramPageWatch.rdram + rdramPageWatch.guestAddress,
                        rdramPageWatch.size);
            const bool changed =
                std::memcmp(rdramPageWatchBefore.data(), after.data(),
                            rdramPageWatch.size) != 0;
            const uintptr_t watchedStart = reinterpret_cast<uintptr_t>(
                rdramPageWatch.rdram + rdramPageWatch.guestAddress);
            const bool wroteWatchedRange =
                rdramPageWatchAccessAddress >= watchedStart &&
                rdramPageWatchAccessAddress < watchedStart + rdramPageWatch.size;
            bool matched = !rdramPageWatch.match32Enabled;
            if (rdramPageWatch.match32Enabled)
            {
                const uint32_t matchLimit = rdramPageWatch.match32Any
                    ? rdramPageWatch.size
                    : std::min<uint32_t>(rdramPageWatch.size, sizeof(uint32_t));
                for (uint32_t offset = 0u;
                     offset + sizeof(uint32_t) <= matchLimit;
                     ++offset)
                {
                    uint32_t candidate = 0u;
                    std::memcpy(&candidate, after.data() + offset, sizeof(candidate));
                    if (candidate == rdramPageWatch.match32)
                    {
                        matched = true;
                        break;
                    }
                }
            }
            if ((changed || (rdramPageWatch.reportAnyWrite && wroteWatchedRange)) &&
                matched)
            {
                rdramPageWatch.instruction = rdramPageWatchInstruction;
                rdramPageWatch.accessAddress = rdramPageWatchAccessAddress;
                rdramPageWatch.threadId = rdramPageWatchThreadId;
                rdramPageWatch.frameCount = rdramPageWatchFrameCount;
                rdramPageWatch.frames = rdramPageWatchFrames;
                rdramPageWatch.before = rdramPageWatchBefore;
                rdramPageWatch.after = after;
                rdramPageWatch.enabled.store(false);
                rdramPageWatch.ready.store(true);
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            DWORD ignored = 0u;
            if (!VirtualProtect(rdramPageWatch.page, rdramPageWatch.pageSize,
                                PAGE_READONLY, &ignored))
            {
                rdramPageWatch.enabled.store(false);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void disableRdramPageWatch()
    {
        rdramPageWatch.enabled.store(false);
        if (rdramPageWatch.page)
        {
            DWORD ignored = 0u;
            VirtualProtect(rdramPageWatch.page, rdramPageWatch.pageSize,
                           rdramPageWatch.originalProtection, &ignored);
        }
        if (rdramPageWatch.handler)
        {
            RemoveVectoredExceptionHandler(rdramPageWatch.handler);
            rdramPageWatch.handler = nullptr;
        }
        rdramPageWatch.page = nullptr;
        rdramPageWatch.ready.store(false);
    }

    void armRdramPageWatch(uint64_t tick)
    {
        if (!rdramPageWatch.page || !rdramPageWatch.handler ||
            rdramPageWatch.enabled.load() || rdramPageWatch.ready.load() ||
            tick < rdramPageWatch.armTick)
        {
            return;
        }

        if (!VirtualProtect(rdramPageWatch.page, rdramPageWatch.pageSize,
                            PAGE_READONLY, &rdramPageWatch.originalProtection))
        {
            disableRdramPageWatch();
            return;
        }
        rdramPageWatch.enabled.store(true);
        std::fprintf(stderr,
                     "[xmen-rdram-page-watch:armed] tick=%llu guest=0x%08x size=0x%x "
                     "host=%p page=%p module=0x%llx match32=%s0x%08x any=%u "
                     "reportAnyWrite=%u\n",
                     static_cast<unsigned long long>(tick),
                     rdramPageWatch.guestAddress, rdramPageWatch.size,
                     rdramPageWatch.rdram + rdramPageWatch.guestAddress,
                     rdramPageWatch.page,
                     static_cast<unsigned long long>(rdramPageWatch.moduleBase),
                     rdramPageWatch.match32Enabled ? "" : "disabled:",
                     rdramPageWatch.match32,
                     rdramPageWatch.match32Any ? 1u : 0u,
                     rdramPageWatch.reportAnyWrite ? 1u : 0u);
    }

    void initializeRdramPageWatch(uint8_t *rdram, size_t ramSize)
    {
        disableRdramPageWatch();
        const char *addressValue = std::getenv("PS2X_WATCH_RDRAM_PAGE_GUEST");
        if (!rdram || !addressValue || addressValue[0] == '\0')
            return;

        const uint32_t guestAddress =
            static_cast<uint32_t>(std::strtoul(addressValue, nullptr, 0)) & PS2_RAM_MASK;
        const char *sizeValue = std::getenv("PS2X_WATCH_RDRAM_PAGE_SIZE");
        const uint32_t size = sizeValue
            ? static_cast<uint32_t>(std::strtoul(sizeValue, nullptr, 0))
            : 16u;
        if (size == 0u || size > rdramPageWatch.before.size() ||
            static_cast<uint64_t>(guestAddress) + size > ramSize)
        {
            return;
        }

        SYSTEM_INFO systemInfo{};
        GetSystemInfo(&systemInfo);
        const size_t pageSize = systemInfo.dwPageSize;
        const uintptr_t target = reinterpret_cast<uintptr_t>(rdram + guestAddress);
        const uintptr_t pageStart = target & ~(static_cast<uintptr_t>(pageSize) - 1u);
        if (target + size > pageStart + pageSize)
            return;

        rdramPageWatch.rdram = rdram;
        rdramPageWatch.guestAddress = guestAddress;
        rdramPageWatch.size = size;
        const char *match32Value = std::getenv("PS2X_WATCH_RDRAM_PAGE_MATCH32");
        rdramPageWatch.match32Enabled = match32Value && match32Value[0] != '\0';
        rdramPageWatch.match32 = rdramPageWatch.match32Enabled
            ? static_cast<uint32_t>(std::strtoul(match32Value, nullptr, 0))
            : 0u;
        rdramPageWatch.match32Any =
            std::getenv("PS2X_WATCH_RDRAM_PAGE_MATCH32_ANY") != nullptr;
        rdramPageWatch.reportAnyWrite =
            std::getenv("PS2X_WATCH_RDRAM_PAGE_REPORT_ANY_WRITE") != nullptr;
        const char *armTickValue = std::getenv("PS2X_WATCH_RDRAM_PAGE_ARM_TICK");
        rdramPageWatch.armTick = armTickValue
            ? std::strtoull(armTickValue, nullptr, 0)
            : 0u;
        rdramPageWatch.page = reinterpret_cast<uint8_t *>(pageStart);
        rdramPageWatch.pageSize = pageSize;
        rdramPageWatch.moduleBase =
            reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        std::memcpy(rdramPageWatch.before.data(), rdram + guestAddress, size);
        rdramPageWatch.handler = AddVectoredExceptionHandler(1u, handleRdramPageWatch);
        if (!rdramPageWatch.handler)
        {
            disableRdramPageWatch();
            return;
        }
        armRdramPageWatch(0u);
    }

    void reportRdramPageWatch(uint64_t tick)
    {
        armRdramPageWatch(tick);
        if (!rdramPageWatch.ready.exchange(false))
            return;

        std::fprintf(stderr,
                     "[xmen-rdram-page-watch:changed] guest=0x%08x size=0x%x "
                     "accessGuest=0x%08x thread=%lu rip=0x%llx module=0x%llx "
                     "offset=0x%llx before=",
                     rdramPageWatch.guestAddress, rdramPageWatch.size,
                     static_cast<uint32_t>(rdramPageWatch.accessAddress -
                         reinterpret_cast<uintptr_t>(rdramPageWatch.rdram)),
                     static_cast<unsigned long>(rdramPageWatch.threadId),
                     static_cast<unsigned long long>(rdramPageWatch.instruction),
                     static_cast<unsigned long long>(rdramPageWatch.moduleBase),
                     static_cast<unsigned long long>(
                         rdramPageWatch.instruction - rdramPageWatch.moduleBase));
        for (uint32_t index = 0u; index < rdramPageWatch.size; ++index)
            std::fprintf(stderr, "%02x", rdramPageWatch.before[index]);
        std::fprintf(stderr, " after=");
        for (uint32_t index = 0u; index < rdramPageWatch.size; ++index)
            std::fprintf(stderr, "%02x", rdramPageWatch.after[index]);
        std::fprintf(stderr, " stack=");
        for (USHORT index = 0u; index < rdramPageWatch.frameCount; ++index)
        {
            std::fprintf(stderr, "%s0x%llx", index == 0u ? "" : ",",
                         static_cast<unsigned long long>(
                             reinterpret_cast<uintptr_t>(rdramPageWatch.frames[index])));
        }
        std::fprintf(stderr, "\n");
        disableRdramPageWatch();
    }
#else
    void disableRdramPageWatch() {}
    void initializeRdramPageWatch(uint8_t *, size_t) {}
    void reportRdramPageWatch(uint64_t) {}
#endif

    bool gifDmaPayloadInTraceRange(uint32_t address, uint32_t bytes)
    {
        static const uint32_t rangeStart = []()
        {
            const char *value = std::getenv("PS2X_TRACE_GIF_DMA_RANGE_START");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
        }();
        static const uint32_t rangeEnd = []()
        {
            const char *value = std::getenv("PS2X_TRACE_GIF_DMA_RANGE_END");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
        }();

        if (rangeEnd <= rangeStart || bytes == 0u)
            return false;

        const uint64_t payloadStart = address;
        const uint64_t payloadEnd = payloadStart + bytes;
        return payloadStart < rangeEnd && payloadEnd > rangeStart;
    }

    bool findXmenConsoleTextureBitblt(const uint8_t *data,
                                     size_t sizeBytes,
                                     uint32_t &dbpOut,
                                     size_t &offsetOut)
    {
        if (!data)
            return false;

        for (size_t offset = 8u; offset + 8u <= sizeBytes; offset += 8u)
        {
            uint64_t value = 0u;
            uint64_t reg = 0u;
            std::memcpy(&value, data + offset - 8u, sizeof(value));
            std::memcpy(&reg, data + offset, sizeof(reg));
            const uint32_t dbp = static_cast<uint32_t>((value >> 32u) & 0x3FFFu);
            if ((reg & 0xFFu) == 0x50u && (dbp == 11652u || dbp == 11656u))
            {
                dbpOut = dbp;
                offsetOut = offset - 8u;
                return true;
            }
        }
        return false;
    }

    inline void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (static_cast<uint64_t>(offset) + static_cast<uint64_t>(bytes) > static_cast<uint64_t>(regionSize))
        {
            throw std::runtime_error(std::string(op) + " out-of-bounds at address: 0x" + std::to_string(address));
        }
    }

    template <typename T>
    inline T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T value{};
        std::memcpy(&value, base + offset, sizeof(T));
        return value;
    }

    template <typename T>
    inline void storeScalar(uint8_t *base, uint32_t offset, size_t regionSize, T value, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        std::memcpy(base + offset, &value, sizeof(T));
    }

    inline bool isGsPrivReg(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_GS_PRIV_REG_BASE, PS2_GS_PRIV_REG_SIZE);
    }

    inline bool isIoRegister(uint32_t addr)
    {
        return Ps2AddressInRange(addr, PS2_IO_BASE, PS2_IO_SIZE);
    }

    inline uint32_t canonicalDmacAddress(uint32_t address)
    {
        if ((address & 0x80000000u) != 0u)
            return PS2_SCRATCHPAD_BASE + (address & (PS2_SCRATCHPAD_SIZE - 1u));
        return address;
    }

    inline uint64_t *gsRegPtr(GSRegisters &gs, uint32_t addr)
    {
        // Support both 64-bit base offsets and +4 dword aliases.
        uint32_t off = (addr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        switch (off)
        {
        case 0x0000:
            return &gs.pmode;
        case 0x0010:
            return &gs.smode1;
        case 0x0020:
            return &gs.smode2;
        case 0x0030:
            return &gs.srfsh;
        case 0x0040:
            return &gs.synch1;
        case 0x0050:
            return &gs.synch2;
        case 0x0060:
            return &gs.syncv;
        case 0x0070:
            return &gs.dispfb1;
        case 0x0080:
            return &gs.display1;
        case 0x0090:
            return &gs.dispfb2;
        case 0x00A0:
            return &gs.display2;
        case 0x00B0:
            return &gs.extbuf;
        case 0x00C0:
            return &gs.extdata;
        case 0x00D0:
            return &gs.extwrite;
        case 0x00E0:
            return &gs.bgcolor;
        // CSR (offset 0x1000) is intentionally not handled here: it is
        // std::atomic<uint64_t> and no longer converts to uint64_t*. Callers must
        // check for offset 0x1000 themselves and go through writeCsrHalf/
        // writeCsrFull/gs.csr.load() instead of gsRegPtr().
        case 0x1010:
            return &gs.imr;
        case 0x1040:
            return &gs.busdir;
        case 0x1080:
            return &gs.siglblid;
        default:
            return nullptr;
        }
    }

    constexpr uint32_t kGsCsrRegOffset = 0x1000u;
    constexpr uint32_t kGsImrRegOffset = 0x1010u;

    // Atomically apply a 32-bit write to one half (off=0 low dword, off=4 high
    // dword) of the GS CSR register. Event bits 0..4 are write-one-to-clear; the
    // remaining bits are either commands or read-only status. Uses compare_exchange
    // so the whole read-modify-write is a single atomic step -- this register is
    // also touched by the vsync worker (FIELD bit) and the GIF (SIGNAL/FINISH) on
    // other threads, so a load-then-store here would race with them.
    inline void writeCsrHalf(std::atomic<uint64_t> &csr, uint32_t off, uint32_t value)
    {
        constexpr uint32_t kW1cMask = 0x1Fu;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            if (off == 0u)
            {
                uint32_t oldLow = static_cast<uint32_t>(expected & 0xFFFFFFFFull);
                uint32_t nextLow = oldLow & ~(value & kW1cMask);
                desired = (expected & 0xFFFFFFFF00000000ull) | static_cast<uint64_t>(nextLow);
            }
            else
            {
                uint64_t mask = 0xFFFFFFFFull << (off * 8u);
                desired = (expected & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
            }
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    // Same as writeCsrHalf but for a full 64-bit CSR write.
    inline void writeCsrFull(std::atomic<uint64_t> &csr, uint64_t value)
    {
        constexpr uint64_t kW1cMask = 0x1Full;
        uint64_t expected = csr.load();
        uint64_t desired;
        do
        {
            desired = expected & ~(value & kW1cMask);
        } while (!csr.compare_exchange_weak(expected, desired));
    }

    constexpr std::array<uint32_t, 4> kEeTimerBases = {
        0x10000000u,
        0x10000800u,
        0x10001000u,
        0x10001800u,
    };
    constexpr uint32_t kEeTimerCountOffset = 0x00u;
    constexpr uint32_t kEeTimerModeOffset = 0x10u;
    constexpr uint32_t kEeTimerCompareOffset = 0x20u;
    constexpr uint32_t kEeTimerHoldOffset = 0x30u;
    constexpr uint32_t kEeTimerModeClksMask = 0x3u;
    constexpr uint32_t kEeTimerModeConfigMask = 0x3FFu;
    constexpr uint32_t kEeTimerModeStatusMask = 0xC00u;
    constexpr uint32_t kEeTimerModeZret = 1u << 6;
    constexpr uint32_t kEeTimerModeCue = 1u << 7;
    constexpr uint32_t kEeTimerModeCmpe = 1u << 8;
    constexpr uint32_t kEeTimerModeOvfe = 1u << 9;
    constexpr uint32_t kEeTimerModeEquf = 1u << 10;
    constexpr uint32_t kEeTimerModeOvff = 1u << 11;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kEeTimerClockHz = {
        147456000ull,
        9216000ull,
        576000ull,
        15734ull,
    };

    inline bool decodeEeTimerRegister(uint32_t address, size_t &timerIndex, uint32_t &offset)
    {
        for (size_t index = 0; index < kEeTimerBases.size(); ++index)
        {
            const uint32_t candidateOffset = address - kEeTimerBases[index];
            if (candidateOffset == kEeTimerCountOffset ||
                candidateOffset == kEeTimerModeOffset ||
                candidateOffset == kEeTimerCompareOffset ||
                (index < 2u && candidateOffset == kEeTimerHoldOffset))
            {
                timerIndex = index;
                offset = candidateOffset;
                return true;
            }
        }
        return false;
    }

    constexpr uint64_t ticksUntilMatch(uint32_t count, uint32_t target)
    {
        const uint32_t distance = (target - count) & 0xFFFFu;
        return distance == 0u ? 0x10000ull : static_cast<uint64_t>(distance);
    }

    struct DmaTagView
    {
        uint16_t qwc = 0;
        uint8_t id = 0;
        bool irq = false;
        uint32_t addr = 0;
        uint32_t upper = 0;
    };

    inline DmaTagView decodeDmaTag(uint64_t tag)
    {
        DmaTagView out{};
        out.qwc = static_cast<uint16_t>(tag & 0xFFFFu);
        out.id = static_cast<uint8_t>((tag >> 28u) & 0x7u);
        out.irq = ((tag >> 31u) & 0x1ull) != 0ull;
        out.addr = static_cast<uint32_t>((tag >> 32u) & 0x7FFFFFFFu);
        out.upper = static_cast<uint32_t>((tag >> 16u) & 0xFFFFu);
        return out;
    }

    inline uint32_t gifTagNloop(uint64_t tagLo)
    {
        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }

    inline uint8_t gifTagFlg(uint64_t tagLo)
    {
        return static_cast<uint8_t>((tagLo >> 58u) & 0x3u);
    }

    inline uint32_t gifTagNreg(uint64_t tagLo)
    {
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60u) & 0xFu);
        return nreg == 0u ? 16u : nreg;
    }

}

// Helpers for GS VRAM addressing (PSMCT32 path).
static inline uint32_t gs_vram_offset(uint32_t basePage, uint32_t x, uint32_t y, uint32_t fbw)
{
    // basePage is in 2048-byte units; fbw is in blocks of 64 pixels.
    uint32_t strideBytes = fbw * 64 * 4;
    return basePage * 2048 + y * strideBytes + x * 4;
}

PS2Memory::PS2Memory()
    : m_rdram(nullptr), m_scratchpad(nullptr), iop_ram(nullptr), m_seenGifCopy(false), m_gsVRAM(nullptr)
{
    ps2SetScratchpadHostPtr(nullptr);
}

PS2Memory::~PS2Memory()
{
    disableRdramPageWatch();
    if (m_rdram)
    {
        delete[] m_rdram;
        m_rdram = nullptr;
    }

    if (m_scratchpad)
    {
        ps2SetScratchpadHostPtr(nullptr);
        delete[] m_scratchpad;
        m_scratchpad = nullptr;
    }

    if (m_gsVRAM)
    {
        delete[] m_gsVRAM;
        m_gsVRAM = nullptr;
    }

    if (m_vu1Code)
    {
        delete[] m_vu1Code;
        m_vu1Code = nullptr;
    }
    if (m_vu1Data)
    {
        delete[] m_vu1Data;
        m_vu1Data = nullptr;
    }
    if (m_vu0Code)
    {
        delete[] m_vu0Code;
        m_vu0Code = nullptr;
    }
    if (m_vu0Data)
    {
        delete[] m_vu0Data;
        m_vu0Data = nullptr;
    }

    if (iop_ram)
    {
        delete[] iop_ram;
        iop_ram = nullptr;
    }
}

bool PS2Memory::initialize(size_t ramSize)
{
    auto cleanup = [this]()
    {
        disableRdramPageWatch();
        delete[] m_rdram;
        delete[] m_scratchpad;
        delete[] iop_ram;
        delete[] m_gsVRAM;
        delete[] m_vu0Code;
        delete[] m_vu0Data;
        delete[] m_vu1Code;
        delete[] m_vu1Data;
        m_rdram = nullptr;
        m_scratchpad = nullptr;
        ps2SetScratchpadHostPtr(nullptr);
        iop_ram = nullptr;
        m_gsVRAM = nullptr;
        m_vu0Code = nullptr;
        m_vu0Data = nullptr;
        m_vu1Code = nullptr;
        m_vu1Data = nullptr;
    };

    cleanup();
    m_seenGifCopy = false;
    m_dmaStartCount.store(0, std::memory_order_relaxed);
    for (auto &count : m_dmaChannelStartCounts)
        count.store(0, std::memory_order_relaxed);
    m_gifCopyCount.store(0, std::memory_order_relaxed);
    m_gsWriteCount.store(0, std::memory_order_relaxed);
    m_vifWriteCount.store(0, std::memory_order_relaxed);
    m_vif1TransferCount.store(0, std::memory_order_relaxed);
    m_vif1MscalDispatchCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_completedDmacMutex);
        m_completedDmacCauses.clear();
    }
    m_codeRegions.clear();
    m_path3Masked = false;
    m_path3MaskedFifo.clear();
    m_vif1PendingPath2ImageQwc = 0u;
    m_vif1PendingPath2DirectHl = false;
    resetEeTimers();

    try
    {
        // Allocate main RAM
        m_rdram = new uint8_t[ramSize];
        std::memset(m_rdram, 0, ramSize);
        initializeRdramPageWatch(m_rdram, ramSize);

        // Allocate scratchpad
        m_scratchpad = new uint8_t[PS2_SCRATCHPAD_SIZE];
        std::memset(m_scratchpad, 0, PS2_SCRATCHPAD_SIZE);
        ps2SetScratchpadHostPtr(m_scratchpad);

        // Initialize EE TLB entries (R5900 has 48 entries).
        m_tlbEntries.assign(48, TLBEntry{0, 0, 0, false});

        // Allocate IOP RAM
        iop_ram = new uint8_t[2 * 1024 * 1024]; // 2MB

        // Initialize IOP RAM with zeros
        std::memset(iop_ram, 0, 2 * 1024 * 1024);

        // Initialize I/O registers
        m_ioRegisters.clear();

        // Initialize GS registers
        memset(&gs_regs, 0, sizeof(gs_regs));
        // memset zero-fills std::atomic<uint64_t>::csr's bytes, which is not itself
        // a guaranteed-valid atomic store; make the zero-initialization explicit.
        gs_regs.csr.store(0);
        gs_regs.imr = PS2_GS_IMR_RESET_VALUE;
        gs_regs.dispfb1 = (0ULL << 0) | (10ULL << 9) | (0ULL << 15) | (0ULL << 32) | (0ULL << 43);
        gs_regs.display1 = (0ULL << 0) | (0ULL << 12) | (0ULL << 23) | (0ULL << 27) | (639ULL << 32) | (447ULL << 44);
        gs_regs.dispfb2 = gs_regs.dispfb1;
        gs_regs.display2 = gs_regs.display1;

        // Allocate GS VRAM (4MB)
        m_gsVRAM = new uint8_t[PS2_GS_VRAM_SIZE];
        std::memset(m_gsVRAM, 0, PS2_GS_VRAM_SIZE);

        m_vu0Code = new uint8_t[PS2_VU0_CODE_SIZE];
        m_vu0Data = new uint8_t[PS2_VU0_DATA_SIZE];
        std::memset(m_vu0Code, 0, PS2_VU0_CODE_SIZE);
        std::memset(m_vu0Data, 0, PS2_VU0_DATA_SIZE);

        m_vu1Code = new uint8_t[PS2_VU1_CODE_SIZE];
        m_vu1Data = new uint8_t[PS2_VU1_DATA_SIZE];
        std::memset(m_vu1Code, 0, PS2_VU1_CODE_SIZE);
        std::memset(m_vu1Data, 0, PS2_VU1_DATA_SIZE);
        markVU0CodeModified();
        markVU1CodeModified();

        // Initialize VIF registers
        memset(&vif0_regs, 0, sizeof(vif0_regs));
        memset(&vif1_regs, 0, sizeof(vif1_regs));
        m_vif1MscalPending = false;
        m_vif1PendingMscalUnpacks = 0u;

        // Initialize DMA registers
        memset(dma_regs, 0, sizeof(dma_regs));

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error initializing PS2 memory: " << e.what() << std::endl;
        cleanup();
        return false;
    }
}

void PS2Memory::resetEeTimers() noexcept
{
    m_eeTimers = {};
}

uint32_t PS2Memory::advanceEeTimers(uint64_t eeCycles) noexcept
{
    if (eeCycles == 0u)
    {
        return 0u;
    }

    uint32_t interruptMask = 0u;
    for (size_t index = 0; index < m_eeTimers.size(); ++index)
    {
        EeTimer &timer = m_eeTimers[index];
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t wholeSeconds = eeCycles / kEeClockHz;
        const uint64_t remainingCycles = eeCycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * clockHz + timer.clockRemainder;
        const uint64_t ticks = wholeSeconds * clockHz + scaled / kEeClockHz;
        timer.clockRemainder = scaled % kEeClockHz;
        if (ticks == 0u)
        {
            continue;
        }

        const uint32_t oldCount = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(oldCount, compare);
        const uint64_t overflowDistance = 0x10000ull - oldCount;
        const bool zeroReturn = (timer.mode & kEeTimerModeZret) != 0u;
        const bool compareReached = ticks >= compareDistance;
        bool overflowReached = false;

        if (zeroReturn)
        {
            overflowReached = ticks >= overflowDistance && overflowDistance <= compareDistance;
            if (compareReached)
            {
                const uint64_t remaining = ticks - compareDistance;
                timer.count = compare == 0u
                                  ? static_cast<uint32_t>(remaining & 0xFFFFu)
                                  : static_cast<uint32_t>(remaining % compare);
            }
            else
            {
                timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
            }
        }
        else
        {
            overflowReached = ticks >= overflowDistance;
            timer.count = static_cast<uint32_t>((oldCount + ticks) & 0xFFFFu);
        }

        if (compareReached && (timer.mode & kEeTimerModeCmpe) != 0u && (timer.mode & kEeTimerModeEquf) == 0u)
        {
            timer.mode |= kEeTimerModeEquf;
            interruptMask |= 1u << index;
            static std::atomic<uint32_t> compareTraceCount{0u};
            if (compareTraceCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
            {
                std::fprintf(stderr,
                             "[ee-timer:compare] timer=%zu cycles=%llu old=%u compare=%u ticks=%llu mode=0x%x\n",
                             index,
                             static_cast<unsigned long long>(eeCycles),
                             oldCount,
                             compare,
                             static_cast<unsigned long long>(ticks),
                             timer.mode);
            }
        }
        if (overflowReached && (timer.mode & kEeTimerModeOvfe) != 0u && (timer.mode & kEeTimerModeOvff) == 0u)
        {
            timer.mode |= kEeTimerModeOvff;
            interruptMask |= 1u << index;
        }
    }
    return interruptMask;
}

uint64_t PS2Memory::cyclesUntilNextEeTimerInterrupt() const noexcept
{
    uint64_t nearest = std::numeric_limits<uint64_t>::max();
    for (const EeTimer &timer : m_eeTimers)
    {
        if ((timer.mode & kEeTimerModeCue) == 0u)
        {
            continue;
        }

        const uint32_t count = timer.count & 0xFFFFu;
        const uint32_t compare = timer.compare & 0xFFFFu;
        const uint64_t compareDistance = ticksUntilMatch(count, compare);
        const uint64_t overflowDistance = 0x10000ull - count;
        uint64_t eventTicks = std::numeric_limits<uint64_t>::max();

        if ((timer.mode & kEeTimerModeCmpe) != 0u &&
            (timer.mode & kEeTimerModeEquf) == 0u)
        {
            eventTicks = compareDistance;
        }
        const bool overflowCanOccur = (timer.mode & kEeTimerModeZret) == 0u ||
                                      overflowDistance <= compareDistance;
        if (overflowCanOccur &&
            (timer.mode & kEeTimerModeOvfe) != 0u &&
            (timer.mode & kEeTimerModeOvff) == 0u)
        {
            eventTicks = std::min(eventTicks, overflowDistance);
        }
        if (eventTicks == std::numeric_limits<uint64_t>::max())
        {
            continue;
        }

        const uint64_t clockHz = kEeTimerClockHz[timer.mode & kEeTimerModeClksMask];
        const uint64_t numerator = eventTicks * kEeClockHz - timer.clockRemainder;
        const uint64_t cycles = (numerator + clockHz - 1u) / clockHz;
        nearest = std::min(nearest, std::max<uint64_t>(1u, cycles));
    }
    return nearest;
}

bool PS2Memory::isScratchpad(uint32_t address) const
{
    return ps2IsScratchpadAddress(address);
}

uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit)
{
    return const_cast<uint8_t *>(static_cast<const PS2Memory *>(this)->mapVuMemory(physAddr, size, offset, limit));
}

const uint8_t *PS2Memory::mapVuMemory(uint32_t physAddr, uint32_t size, uint32_t &offset, uint32_t &limit) const
{
    auto mapRange = [&](uint32_t base, uint32_t rangeSize, const uint8_t *ptr) -> const uint8_t *
    {
        if (!ptr || physAddr < base)
        {
            return nullptr;
        }
        const uint32_t local = physAddr - base;
        if (local >= rangeSize || size > (rangeSize - local))
        {
            return nullptr;
        }
        offset = local;
        limit = rangeSize;
        return ptr;
    };

    if (const uint8_t *ptr = mapRange(PS2_VU0_CODE_BASE, PS2_VU0_CODE_SIZE, m_vu0Code))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU0_DATA_BASE, PS2_VU0_DATA_SIZE, m_vu0Data))
    {
        return ptr;
    }
    if (const uint8_t *ptr = mapRange(PS2_VU1_CODE_BASE, PS2_VU1_CODE_SIZE, m_vu1Code))
    {
        return ptr;
    }
    return mapRange(PS2_VU1_DATA_BASE, PS2_VU1_DATA_SIZE, m_vu1Data);
}

uint32_t PS2Memory::translateAddress(uint32_t virtualAddress)
{
    if (isScratchpad(virtualAddress))
    {
        return ps2ScratchpadOffset(virtualAddress);
    }

    // EE uncached aliases of main RAM (per PS2 memory map):
    //   0x20000000-0x3FFFFFFF -> 32MB mirror of RDRAM
    // This includes the accelerated window rooted at 0x30100000.
    if (Ps2IsUncachedRamMirrorAddress(virtualAddress))
    {
        return virtualAddress & PS2_RAM_MASK;
    }

    // KSEG0/KSEG1 direct-mapped window.
    if (Ps2IsKseg01Address(virtualAddress))
    {
        return Ps2DirectMappedPhysicalAddress(virtualAddress);
    }

    // In this runtime, low segments are treated as physical-style addresses already.
    if (virtualAddress < 0x80000000)
    {
        return virtualAddress;
    }

    // KSEG2/KSEG3 are TLB mapped.
    if (Ps2IsKseg23Address(virtualAddress))
    {
        for (const auto &entry : m_tlbEntries)
        {
            if (entry.valid)
            {
                // PageMask uses bits [24:13]. Build an address-level mask (plus 4KB base page bits).
                const uint32_t mask = entry.mask & 0x01FFE000u;
                const uint32_t compareMask = ~(mask | 0xFFFu);
                if ((virtualAddress & compareMask) == (entry.vpn & compareMask))
                {
                    // TLB hit
                    const uint32_t pageOffsetMask = mask | 0xFFFu;
                    const uint32_t physBase = entry.pfn << 12;
                    return physBase | (virtualAddress & pageOffsetMask);
                }
            }
        }
        throw std::runtime_error("TLB miss for address: 0x" + std::to_string(virtualAddress));
    }

    return virtualAddress;
}

bool PS2Memory::tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    const TLBEntry &entry = m_tlbEntries[index];
    vpn = entry.vpn;
    pfn = entry.pfn;
    mask = entry.mask;
    valid = entry.valid;
    return true;
}

bool PS2Memory::tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid)
{
    if (index >= m_tlbEntries.size())
    {
        return false;
    }

    TLBEntry &entry = m_tlbEntries[index];
    entry.vpn = vpn & 0xFFFFF000u;
    entry.pfn = pfn & 0x000FFFFFu;
    entry.mask = mask & 0x01FFE000u;
    entry.valid = valid;
    return true;
}

int32_t PS2Memory::tlbProbe(uint32_t vpn) const
{
    const uint32_t normalizedVpn = vpn & 0xFFFFF000u;
    for (uint32_t i = 0; i < static_cast<uint32_t>(m_tlbEntries.size()); ++i)
    {
        const TLBEntry &entry = m_tlbEntries[i];
        if (!entry.valid)
        {
            continue;
        }

        const uint32_t mask = entry.mask & 0x01FFE000u;
        const uint32_t compareMask = ~(mask | 0xFFFu);
        if ((normalizedVpn & compareMask) == (entry.vpn & compareMask))
        {
            return static_cast<int32_t>(i);
        }
    }

    return -1;
}

uint8_t PS2Memory::read8(uint32_t address)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return m_scratchpad[physAddr];
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return m_rdram[physAddr];
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
    {
        (void)vuLimit;
        return vuMem[vuOffset];
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 3) * 8;
        return static_cast<uint8_t>((value >> shift) & 0xFF);
    }

    return 0;
}

uint16_t PS2Memory::read16(uint32_t address)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        return loadScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read16 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read16 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
    {
        return loadScalar<uint16_t>(vuMem, vuOffset, vuLimit, "read16 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t value = readIORegister(regAddr);
        uint32_t shift = (physAddr & 2) * 8;
        return static_cast<uint16_t>((value >> shift) & 0xFFFF);
    }

    return 0;
}

uint32_t PS2Memory::read32(uint32_t address)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit read at address: 0x" + std::to_string(address));
    }

    const uint32_t physAddr = translateAddress(address);
    if (isGsPrivReg(physAddr))
    {
        uint32_t off = physAddr & 7;
        const uint32_t regOff = (physAddr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            uint64_t val = gs_regs.csr.load();
            return (uint32_t)(val >> (off * 8));
        }
        uint64_t *reg = gsRegPtr(gs_regs, physAddr);
        if (!reg)
            return 0;
        uint64_t val = *reg;
        return (uint32_t)(val >> (off * 8));
    }

    const bool scratch = isScratchpad(address);
    if (scratch)
    {
        return loadScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read32 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read32 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
    {
        return loadScalar<uint32_t>(vuMem, vuOffset, vuLimit, "read32 vu", address);
    }
    else if (isIoRegister(physAddr))
    {
        return readIORegister(physAddr);
    }

    return 0;
}

uint64_t PS2Memory::read64(uint32_t address)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit read at address: 0x" + std::to_string(address));
    }

    const uint32_t physAddr = translateAddress(address);
    if (isGsPrivReg(physAddr))
    {
        const uint32_t regOff = (physAddr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return gs_regs.csr.load();
        }
        uint64_t *reg = gsRegPtr(gs_regs, physAddr);
        return reg ? *reg : 0;
    }

    const bool scratch = isScratchpad(address);
    if (scratch)
    {
        return loadScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, "read64 scratchpad", address);
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        return loadScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, "read64 rdram", address);
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
    {
        return loadScalar<uint64_t>(vuMem, vuOffset, vuLimit, "read64 vu", address);
    }

    // 64-bit IO read: compose from the two adjacent 32-bit IO register slots
    // to avoid any side-effects from read32 handlers.
    if (isIoRegister(address))
    {
        uint32_t lo = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t hi = m_ioRegisters.count(address + 4) ? m_ioRegisters[address + 4] : 0u;
        return static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    }
    return (uint64_t)read32(address) | ((uint64_t)read32(address + 4) << 32);
}

__m128i PS2Memory::read128(uint32_t address)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit read at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "read128 scratchpad", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]));
    }
    if (physAddr < PS2_RAM_SIZE)
    {
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "read128 rdram", address);
        return _mm_loadu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]));
    }
    uint32_t vuOffset = 0;
    uint32_t vuLimit = 0;
    if (const uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
    {
        inRange(vuOffset, sizeof(__m128i), vuLimit, "read128 vu", address);
        return _mm_loadu_si128(reinterpret_cast<const __m128i *>(vuMem + vuOffset));
    }

    // 128-bit reads are primarily for quad-word loads in the EE, which are only valid for RAM areas
    // Return zeroes for unsupported areas
    return _mm_setzero_si128();
}

void PS2Memory::write8(uint32_t address, uint8_t value)
{
    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        m_scratchpad[physAddr] = value;
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        m_rdram[physAddr] = value;
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint8_t), vuOffset, vuLimit))
        {
            (void)vuLimit;
            vuMem[vuOffset] = value;
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // IO registers - handle byte writes by modifying the appropriate byte in the word
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 3) * 8;
        uint32_t mask = ~(0xFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write16(uint32_t address, uint16_t value)
{
    if (address & 1)
    {
        throw std::runtime_error("Unaligned 16-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (scratch)
    {
        storeScalar<uint16_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write16 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        storeScalar<uint16_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write16 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint16_t), vuOffset, vuLimit))
        {
            storeScalar<uint16_t>(vuMem, vuOffset, vuLimit, value, "write16 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        uint32_t regAddr = physAddr & ~0x3;
        uint32_t shift = (physAddr & 2) * 8;
        uint32_t mask = ~(0xFFFF << shift);
        uint32_t newValue = (m_ioRegisters[regAddr] & mask) | ((uint32_t)value << shift);
        writeIORegister(regAddr, newValue);
    }
}

void PS2Memory::write32(uint32_t address, uint32_t value)
{
    if (address & 3)
    {
        throw std::runtime_error("Unaligned 32-bit write at address: 0x" + std::to_string(address));
    }

    const uint32_t physAddr = translateAddress(address);
    if (isGsPrivReg(physAddr))
    {
        uint32_t off = physAddr & 7;
        const uint32_t regOff = (physAddr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: event bits 0..4 of the low dword are write-one-to-clear.
            // Done as a single atomic RMW -- see writeCsrHalf's comment.
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (regOff == kGsImrRegOffset)
        {
            if (off == 0u)
                writeGsImr(value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, physAddr))
        {
            const uint64_t oldVal = *reg;
            uint64_t mask = 0xFFFFFFFFULL << (off * 8);
            uint64_t newVal = (*reg & ~mask) | ((uint64_t)value << (off * 8));
            *reg = newVal;
            if (regOff >= 0x70u && regOff <= 0xA0u)
            {
                static std::atomic<uint32_t> displayWriteTraceCount{0u};
                const uint32_t traceIndex = displayWriteTraceCount.fetch_add(1u, std::memory_order_relaxed);
                if (traceIndex < 128u)
                {
                    std::fprintf(stderr,
                                 "[gs:mmio-display-write32] index=%u address=0x%08x phys=0x%08x reg=0x%x old=0x%016llx new=0x%016llx\n",
                                 traceIndex,
                                 address,
                                 physAddr,
                                 regOff,
                                 static_cast<unsigned long long>(oldVal),
                                 static_cast<unsigned long long>(newVal));
                }
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    if (scratch)
    {
        storeScalar<uint32_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write32 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        // Check if this might be code modification
        markModified(address, 4);

        storeScalar<uint32_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write32 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint32_t), vuOffset, vuLimit))
        {
            storeScalar<uint32_t>(vuMem, vuOffset, vuLimit, value, "write32 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        writeIORegister(physAddr, value);
    }
}

void PS2Memory::write64(uint32_t address, uint64_t value)
{
    if (address & 7)
    {
        throw std::runtime_error("Unaligned 64-bit write at address: 0x" + std::to_string(address));
    }

    const uint32_t physAddr = translateAddress(address);
    if (isGsPrivReg(physAddr))
    {
        const uint32_t regOff = (physAddr - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            // CSR: event bits 0..4 are write-one-to-clear. Done as a single
            // atomic RMW -- see writeCsrFull's comment.
            writeCsrFull(gs_regs.csr, value);
        }
        else if (regOff == kGsImrRegOffset)
        {
            writeGsImr(value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, physAddr))
        {
            const uint64_t oldVal = *reg;
            *reg = value;
            if (regOff >= 0x70u && regOff <= 0xA0u)
            {
                static std::atomic<uint32_t> displayWriteTraceCount{0u};
                const uint32_t traceIndex = displayWriteTraceCount.fetch_add(1u, std::memory_order_relaxed);
                if (traceIndex < 128u)
                {
                    std::fprintf(stderr,
                                 "[gs:mmio-display-write64] index=%u address=0x%08x phys=0x%08x reg=0x%x old=0x%016llx new=0x%016llx\n",
                                 traceIndex,
                                 address,
                                 physAddr,
                                 regOff,
                                 static_cast<unsigned long long>(oldVal),
                                 static_cast<unsigned long long>(value));
                }
            }
        }
        return;
    }

    const bool scratch = isScratchpad(address);
    if (scratch)
    {
        storeScalar<uint64_t>(m_scratchpad, physAddr, PS2_SCRATCHPAD_SIZE, value, "write64 scratchpad", address);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 8);
        storeScalar<uint64_t>(m_rdram, physAddr, PS2_RAM_SIZE, value, "write64 rdram", address);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(uint64_t), vuOffset, vuLimit))
        {
            storeScalar<uint64_t>(vuMem, vuOffset, vuLimit, value, "write64 vu", address);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        write32(address, (uint32_t)value);
        write32(address + 4, (uint32_t)(value >> 32));
    }
}

uint64_t PS2Memory::writeGsImr(uint64_t value)
{
    const uint64_t oldImr = gs_regs.imr;
    const uint64_t newImr = Ps2NormalizeGsImr(value);
    gs_regs.imr = newImr;

    const uint32_t pendingEvents = static_cast<uint32_t>(gs_regs.csr.load(std::memory_order_acquire)) & 0x1Fu;
    const uint32_t newlyUnmasked =
        pendingEvents & static_cast<uint32_t>((oldImr & ~newImr) >> 8u) & 0x1Fu;
    if (newlyUnmasked != 0u && m_gsInterruptCallback)
        m_gsInterruptCallback(newlyUnmasked);

    return oldImr;
}

void PS2Memory::write128(uint32_t address, __m128i value)
{
    if (address & 15)
    {
        throw std::runtime_error("Unaligned 128-bit write at address: 0x" + std::to_string(address));
    }

    const bool scratch = isScratchpad(address);
    uint32_t physAddr = translateAddress(address);

    if (isGsPrivReg(physAddr))
    {
        // GS privileged registers occupy the low 64 bits of 16-byte EE bus
        // slots. SQ stores are used by some games for display register updates;
        // the upper lane addresses padding and must not spill into the next slot.
        write64(address, static_cast<uint64_t>(_mm_extract_epi64(value, 0)));
        return;
    }

    if (scratch)
    {
        inRange(physAddr, sizeof(__m128i), PS2_SCRATCHPAD_SIZE, "write128 scratchpad", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_scratchpad[physAddr]), value);
    }
    else if (physAddr < PS2_RAM_SIZE)
    {
        markModified(address, 16);
        inRange(physAddr, sizeof(__m128i), PS2_RAM_SIZE, "write128 rdram", address);
        _mm_storeu_si128(reinterpret_cast<__m128i *>(&m_rdram[physAddr]), value);
    }
    else
    {
        uint32_t vuOffset = 0;
        uint32_t vuLimit = 0;
        if (uint8_t *vuMem = mapVuMemory(physAddr, sizeof(__m128i), vuOffset, vuLimit))
        {
            inRange(vuOffset, sizeof(__m128i), vuLimit, "write128 vu", address);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(vuMem + vuOffset), value);
            if (vuMem == m_vu0Code)
                markVU0CodeModified();
            else if (vuMem == m_vu1Code)
                markVU1CodeModified();
            return;
        }
    }
    if (isIoRegister(physAddr))
    {
        // Non-RAM 128-bit stores are modeled as two 64-bit stores.
        uint64_t lo = _mm_extract_epi64(value, 0);
        uint64_t hi = _mm_extract_epi64(value, 1);

        write64(address, lo);
        write64(address + 8, hi);
    }
}

bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)
{
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        EeTimer &timer = m_eeTimers[timerIndex];
        const uint32_t previousCount = timer.count;
        const uint32_t previousMode = timer.mode;
        const uint32_t previousCompare = timer.compare;
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            timer.count = value & 0xFFFFu;
            timer.clockRemainder = 0u;
            break;
        case kEeTimerModeOffset:
        {
            const uint32_t previousMode = timer.mode;
            const uint32_t status = (previousMode & kEeTimerModeStatusMask) &~(value & kEeTimerModeStatusMask);
            timer.mode = (value & kEeTimerModeConfigMask) | status;
            if (((previousMode ^ timer.mode) & (kEeTimerModeClksMask | kEeTimerModeCue)) != 0u)
            {
                timer.clockRemainder = 0u;
            }
            break;
        }
        case kEeTimerCompareOffset:
            timer.compare = value & 0xFFFFu;
            break;
        case kEeTimerHoldOffset:
            timer.hold = value & 0xFFFFu;
            break;
        default:
            return false;
        }
        static std::atomic<uint32_t> timerWriteTraceCount{0u};
        if (xmenDiagnosticsEnabled() && timerIndex < 2u &&
            timerWriteTraceCount.fetch_add(1u, std::memory_order_relaxed) < 128u)
        {
            std::fprintf(stderr,
                         "[ee-timer:write] timer=%zu off=0x%x value=0x%x count=%u->%u mode=0x%x->0x%x compare=%u->%u\n",
                         timerIndex,
                         timerOffset,
                         value,
                         previousCount,
                         timer.count,
                         previousMode,
                         timer.mode,
                         previousCompare,
                         timer.compare);
        }
        return true;
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from write8/16/32/64 today since those all funnel IO
        // register writes through addresses in PS2_IO_BASE's range, which is
        // disjoint from PS2_GS_PRIV_REG_BASE; kept correct for direct callers.
        m_ioRegisters[address] = value;
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            writeCsrHalf(gs_regs.csr, off, value);
        }
        else if (regOff == kGsImrRegOffset)
        {
            if (off == 0u)
                writeGsImr(value);
        }
        else if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            const uint64_t mask = 0xFFFFFFFFull << (off * 8u);
            *reg = (*reg & ~mask) | (static_cast<uint64_t>(value) << (off * 8u));
        }
        m_gsWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        if (address == 0x10002010)
        {
            m_ioRegisters[address] = value & ~(1u << 31);
            if (value & (1u << 30))
            {
                m_ioRegisters[0x10002000] = 0;
                m_ioRegisters[0x10002020] = 0;
                m_ioRegisters[0x10002030] = 0;
            }
        }
        else
        {
            m_ioRegisters[address] = value;
        }
        return true;
    }

    if (address == 0x1000E010u)
    {
        const uint32_t current = m_ioRegisters.count(address) ? m_ioRegisters[address] : 0u;
        uint32_t status = current & 0x3FFu;
        uint32_t mask = (current >> 16) & 0x3FFu;

        // D_STAT low bits are W1C status, high bits [16..25] toggle masks on write-one.
        status &= ~(value & 0x3FFu);
        mask ^= ((value >> 16) & 0x3FFu);

        uint32_t next = (current & ~((0x3FFu) | (0x3FFu << 16) | (1u << 31)));
        next |= status | (mask << 16);
        if ((status & mask) != 0u)
            next |= (1u << 31);
        m_ioRegisters[address] = next;
        return true;
    }

    m_ioRegisters[address] = value;

    if (address >= 0x10003C00u && address < 0x10003E00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);

        switch (address)
        {
        case 0x10003C10u:     // VIF1_FBRST
            if (value & 0x1u) // RST
            {
                std::memset(&vif1_regs, 0, sizeof(vif1_regs));
                m_vif1PendingPath2ImageQwc = 0u;
                m_vif1PendingPath2DirectHl = false;
                m_vif1MscalPending = false;
                m_vif1PendingMscalUnpacks = 0u;
            }
            if (value & 0x8u) // STC
            {
                vif1_regs.stat &= ~((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13));
            }
            break;
        case 0x10003C30u:
            vif1_regs.mark = value & 0xFFFFu;
            vif1_regs.stat &= ~(1u << 6); // clear MRK flag on CPU write
            break;
        case 0x10003C40u:
            vif1_regs.cycle = value & 0xFFFFu;
            break;
        case 0x10003C50u:
            vif1_regs.mode = value & 0x3u;
            break;
        case 0x10003C60u:
            vif1_regs.num = value & 0xFFu;
            break;
        case 0x10003C70u:
            vif1_regs.mask = value;
            break;
        case 0x10003C80u:
            vif1_regs.code = value;
            break;
        case 0x10003C90u:
            vif1_regs.itops = value & 0x3FFu;
            break;
        case 0x10003CA0u:
            vif1_regs.base = value & 0x3FFu;
            break;
        case 0x10003CB0u:
            vif1_regs.ofst = value & 0x3FFu;
            break;
        case 0x10003CC0u:
            vif1_regs.tops = value & 0x3FFu;
            break;
        case 0x10003CD0u:
            vif1_regs.itop = value & 0x3FFu;
            break;
        case 0x10003CE0u:
            vif1_regs.top = value & 0x3FFu;
            break;
        default:
            break;
        }

        return true;
    }

    if (address >= 0x10003800u && address < 0x10003A00u)
    {
        m_vifWriteCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    if (address >= 0x10008000 && address < 0x1000F000)
    {
        if ((address & 0xFF) == 0x00 && (value & 0x100))
        {
            const auto dctrlIt = m_ioRegisters.find(0x1000E000u);
            const bool dmacEnabled = (dctrlIt == m_ioRegisters.end()) || ((dctrlIt->second & 0x1u) != 0u);
            if (!dmacEnabled)
            {
                return true;
            }

            const uint32_t channelBase = address & 0xFFFFFF00;
            const uint32_t madr = m_ioRegisters[channelBase + 0x10];
            const uint32_t qwc = m_ioRegisters[channelBase + 0x20];
            m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
            static constexpr std::array<uint32_t, 10> dmaChannelBases = {
                0x10008000u, 0x10009000u, 0x1000A000u, 0x1000B000u, 0x1000B400u,
                0x1000C000u, 0x1000C400u, 0x1000C800u, 0x1000D000u, 0x1000D400u,
            };
            const auto dmaChannelIt = std::find(dmaChannelBases.begin(),
                                                dmaChannelBases.end(),
                                                channelBase);
            if (dmaChannelIt != dmaChannelBases.end())
            {
                m_dmaChannelStartCounts[static_cast<size_t>(
                    dmaChannelIt - dmaChannelBases.begin())].fetch_add(
                        1u, std::memory_order_relaxed);
            }
            const uint32_t channelIndex = (channelBase - 0x10008000u) >> 12u;
            uint32_t dmaStartIndex = 0u;
            static std::atomic<uint32_t> dmaChannelTraceCounts[7]{};
            if (channelIndex < 7u)
            {
                dmaStartIndex =
                    dmaChannelTraceCounts[channelIndex].fetch_add(1u, std::memory_order_relaxed) + 1u;
                if (dmaStartIndex <= 8u || (dmaStartIndex & (dmaStartIndex - 1u)) == 0u)
                {
                    std::fprintf(stderr,
                                 "[ee-dma:start] channel=0x%x index=%u chcr=0x%x madr=0x%x qwc=0x%x tadr=0x%x\n",
                                 channelBase,
                                 dmaStartIndex,
                                 value,
                                 madr,
                                 qwc,
                                 m_ioRegisters[channelBase + 0x30]);
                }
            }

            if (channelBase == 0x1000B400u && dmaStartIndex <= 4u)
            {
                uint32_t tagAddr = canonicalDmacAddress(m_ioRegisters[channelBase + 0x30u]);
                std::fprintf(stderr,
                             "[ipu1-dma-chain] start=%08x chcr=%08x madr=%08x qwc=%u\n",
                             tagAddr, value, madr, qwc);
                for (uint32_t tagIndex = 0u; tagIndex < 16u; ++tagIndex)
                {
                    const uint32_t tagPhys = translateAddress(tagAddr);
                    if (tagPhys + 16u > PS2_RAM_SIZE)
                        break;

                    const uint8_t *tagPtr = m_rdram + tagPhys;
                    const uint64_t tag = loadScalar<uint64_t>(tagPtr, 0, 16, "IPU1 DMA tag", tagAddr);
                    const uint32_t tagQwc = static_cast<uint32_t>(tag & 0xFFFFu);
                    const uint32_t tagId = static_cast<uint32_t>((tag >> 28u) & 0x7u);
                    const uint32_t tagDataAddr = static_cast<uint32_t>(tag >> 32u) & 0x1FFFFFFFu;
                    std::fprintf(stderr,
                                 "[ipu1-dma-tag] index=%u addr=%08x raw=%016llx id=%u qwc=%u data=%08x irq=%u head=",
                                 tagIndex,
                                 tagAddr,
                                 static_cast<unsigned long long>(tag),
                                 tagId,
                                 tagQwc,
                                 tagDataAddr,
                                 static_cast<unsigned>((tag >> 31u) & 1u));
                    if (tagQwc != 0u && tagDataAddr + 16u <= PS2_RAM_SIZE)
                    {
                        for (uint32_t byte = 0u; byte < 16u; ++byte)
                            std::fprintf(stderr, "%02x", static_cast<unsigned>(m_rdram[tagDataAddr + byte]));
                    }
                    std::fprintf(stderr, "\n");

                    if (tagId == 2u)
                        tagAddr = tagDataAddr;
                    else
                        tagAddr += 16u + ((tagId == 1u || tagId >= 5u) ? tagQwc * 16u : 0u);
                }
            }

            if ((channelBase == 0x1000A000u || channelBase == 0x10009000u || channelBase == 0x10008000u) &&
                (m_gsVRAM || channelBase == 0x10008000u))
            {
                auto enqueueTransfer = [&](uint32_t srcAddr, uint32_t qwCount)
                {
                    if (qwCount == 0)
                        return;
                    srcAddr = canonicalDmacAddress(srcAddr);
                    const bool scratch = isScratchpad(srcAddr);
                    PendingTransfer pt;
                    pt.fromScratchpad = scratch;
                    pt.srcAddr = srcAddr;
                    pt.qwc = qwCount;
                    if (channelBase == 0x1000A000u)
                        m_pendingGifTransfers.push_back(pt);
                    else if (channelBase == 0x10009000u)
                        m_pendingVif1Transfers.push_back(pt);
                    else if (channelBase == 0x10008000u)
                        m_pendingVif0Transfers.push_back(pt);
                };

                uint32_t chcr = value;
                uint32_t mode = (chcr >> 2) & 0x3;

                if (mode == 0 && qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }
                else if (mode == 1)
                {
                    uint32_t tagAddr = m_ioRegisters[channelBase + 0x30];
                    const bool traceXmenVifChain =
                        channelBase == 0x10009000u &&
                        (tagAddr == 0x00970D80u || tagAddr == 0x00984E80u);
                    const bool traceGifChain =
                        channelBase == 0x1000A000u && traceGifDmaChains();
                    uint32_t asr0 = m_ioRegisters[channelBase + 0x40];
                    uint32_t asr1 = m_ioRegisters[channelBase + 0x50];
                    uint32_t asp = (chcr >> 4) & 0x3u;
                    const bool tagTransferEnabled = (chcr & (1u << 6)) != 0u;
                    const bool tieEnabled = (chcr & (1u << 7)) != 0u;
                    const int kMaxChainTags = 65536;
                    std::vector<uint8_t> chainBuf;
                    std::vector<Vif1TraceProvenance> chainProvenance;
                    bool chainEnded = false;
                    uint32_t tracedDestinationFollowups = 0u;
                    const uint32_t chainStartTagAddr = tagAddr;
                    const bool traceXmenCorruptChain =
                        channelBase == 0x10009000u &&
                        (chainStartTagAddr == 0x0098CF00u ||
                         chainStartTagAddr == 0x0099D000u ||
                         chainStartTagAddr == 0x00A05680u);
                    std::FILE *xmenVifChainMap = nullptr;
                    if (traceXmenVifChain && chainStartTagAddr == 0x00984E80u)
                    {
                        xmenVifChainMap = std::fopen("xmen-vif-chain-984e80-map.txt", "wb");
                        if (xmenVifChainMap)
                        {
                            std::fprintf(xmenVifChainMap,
                                         "flat_start flat_end tag id qwc data irq tte\n");
                        }
                    }

                    auto appendData = [&](uint32_t srcAddr, uint32_t qwCount,
                                          uint32_t sourceTagAddr)
                    {
                        const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
                        uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
                        srcAddr = canonicalDmacAddress(srcAddr);
                        const bool scratch = isScratchpad(srcAddr);
                        uint32_t src = 0;
                        src = translateAddress(srcAddr);
                        const uint8_t *base2;
                        uint32_t maxSz2;
                        if (scratch)
                        {
                            base2 = m_scratchpad;
                            maxSz2 = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            base2 = m_rdram;
                            maxSz2 = PS2_RAM_SIZE;
                        }

                        while (bytes > 0)
                        {
                            if (src >= maxSz2)
                                src = 0;
                            uint32_t chunk = bytes;
                            if (src + chunk > maxSz2)
                                chunk = maxSz2 - src;
                            if (chunk == 0)
                                break;
                            const uint32_t flatStart = static_cast<uint32_t>(chainBuf.size());
                            chainBuf.insert(chainBuf.end(), base2 + src, base2 + src + chunk);
                            if (channelBase == 0x10009000u)
                            {
                                chainProvenance.push_back({
                                    flatStart,
                                    static_cast<uint32_t>(chainBuf.size()),
                                    canonicalDmacAddress(srcAddr),
                                    canonicalDmacAddress(sourceTagAddr)});
                            }
                            bytes -= chunk;
                            src += chunk;
                            srcAddr += chunk;
                        }
                    };

                    auto appendVifChainTagData = [&](uint32_t localTagAddr)
                    {
                        localTagAddr = canonicalDmacAddress(localTagAddr);
                        uint32_t tagPhys = 0u;
                        const bool tagScratch = isScratchpad(localTagAddr);
                        tagPhys = translateAddress(localTagAddr);

                        const uint8_t *localBase = tagScratch ? m_scratchpad : m_rdram;
                        const uint32_t localMax = tagScratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                        if (tagPhys + 16u > localMax)
                            return;

                        // VIF packet helpers embed 8 bytes of VIF stream in the DMAtag's upper half.
                        chainBuf.insert(chainBuf.end(), localBase + tagPhys + 8u, localBase + tagPhys + 16u);
                    };

                    int tagsProcessed = 0;
                    uint32_t lastTagUpper = (chcr >> 16) & 0xFFFFu;

                    while (tagsProcessed < kMaxChainTags)
                    {
                        const uint32_t currentTagAddr = tagAddr;
                        const uint32_t canonicalTagAddr = canonicalDmacAddress(tagAddr);
                        const bool tagInSPR = isScratchpad(canonicalTagAddr);
                        uint32_t physTag = 0;
                        try
                        {
                            physTag = translateAddress(canonicalTagAddr);
                        }
                        catch (...)
                        {
                            break;
                        }
                        const uint8_t *tagBase;
                        uint32_t tagMax;
                        if (tagInSPR)
                        {
                            tagBase = m_scratchpad;
                            tagMax = PS2_SCRATCHPAD_SIZE;
                        }
                        else
                        {
                            tagBase = m_rdram;
                            tagMax = PS2_RAM_SIZE;
                        }
                        if (physTag + 16 > tagMax)
                            break;

                        const uint8_t *tp = tagBase + physTag;
                        uint64_t tag = loadScalar<uint64_t>(tp, 0, 16, "dma chain tag", tagAddr);
                        uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
                        uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
                        const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
                        // The upper address bit is the DMAtag SPR selector. Keep it
                        // attached so later tag/payload resolution uses scratchpad.
                        uint32_t addr = static_cast<uint32_t>(tag >> 32);
                        lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
                        ++tagsProcessed;

                        if (traceGifChain)
                        {
                            std::fprintf(stderr,
                                         "[gif-dma-chain-tag] start=%08x index=%d current=%08x "
                                         "raw=%016llx id=%u qwc=%u addr=%08x irq=%u tte=%u\n",
                                         chainStartTagAddr, tagsProcessed, currentTagAddr,
                                         static_cast<unsigned long long>(tag), id, tagQwc, addr,
                                         irq ? 1u : 0u, tagTransferEnabled ? 1u : 0u);
                        }

                        uint32_t dataAddr = 0;
                        bool hasPayload = (tagQwc > 0);
                        bool endChain = false;

                        switch (id)
                        {
                        case 0:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        case 1:
                            dataAddr = tagAddr + 16;
                            tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                            break;
                        case 2:
                            dataAddr = tagAddr + 16;
                            tagAddr = addr;
                            break;
                        case 3:
                        case 4:
                            dataAddr = addr;
                            tagAddr = tagAddr + 16;
                            break;
                        case 5:
                            dataAddr = tagAddr + 16;
                            {
                                const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                                if (asp == 0u)
                                {
                                    asr0 = retAddr;
                                    asp = 1u;
                                }
                                else if (asp == 1u)
                                {
                                    asr1 = retAddr;
                                    asp = 2u;
                                }
                            }
                            tagAddr = addr;
                            break;
                        case 6:
                            dataAddr = tagAddr + 16;
                            if (asp == 2u)
                            {
                                tagAddr = asr1;
                                asp = 1u;
                            }
                            else if (asp == 1u)
                            {
                                tagAddr = asr0;
                                asp = 0u;
                            }
                            else
                            {
                                endChain = true;
                            }
                            break;
                        case 7:
                            dataAddr = tagAddr + 16;
                            endChain = true;
                            break;
                        default:
                            hasPayload = false;
                            endChain = true;
                            break;
                        }

                        if (hasPayload &&
                            gifDmaPayloadInTraceRange(
                                canonicalDmacAddress(dataAddr),
                                static_cast<uint32_t>(tagQwc) * 16u))
                        {
                            static std::atomic<uint32_t> s_dmaRangeTraceCount{0u};
                            const uint32_t traceIndex = s_dmaRangeTraceCount.fetch_add(
                                1u, std::memory_order_relaxed);
                            if (traceIndex < 128u)
                            {
                                std::fprintf(stderr,
                                             "[dma-range] index=%u channel=%08x start=%08x tag=%08x id=%u "
                                             "qwc=%u data=%08x end=%08x\n",
                                             traceIndex, channelBase, chainStartTagAddr, currentTagAddr, id,
                                             tagQwc, canonicalDmacAddress(dataAddr),
                                             canonicalDmacAddress(dataAddr) +
                                                 static_cast<uint32_t>(tagQwc) * 16u);
                            }
                        }

                        static std::atomic<uint32_t> xmenVifChainTagLogCount{0u};
                        if (xmenDiagnosticsEnabled() && traceXmenCorruptChain &&
                            (tagsProcessed <= 256 || endChain ||
                             (tagsProcessed & (tagsProcessed - 1)) == 0) &&
                            xmenVifChainTagLogCount.fetch_add(1u, std::memory_order_relaxed) < 256u)
                        {
                            std::fprintf(stderr,
                                         "[xmen-vif-chain-tag] start=%08x index=%d current=%08x "
                                         "raw=%016llx id=%u qwc=%u addr=%08x spr=%u data=%08x "
                                         "next=%08x asp=%u asr0=%08x asr1=%08x end=%u flat=0x%zx\n",
                                         chainStartTagAddr, tagsProcessed, currentTagAddr,
                                         static_cast<unsigned long long>(tag), id, tagQwc, addr,
                                         (addr >> 31u) & 1u, dataAddr, tagAddr, asp, asr0, asr1,
                                         endChain ? 1u : 0u, chainBuf.size());
                        }

                        const bool vifChannel =
                            (channelBase == 0x10009000u || channelBase == 0x10008000u);
                        const size_t flattenedStart = chainBuf.size();
                        if (vifChannel && tagTransferEnabled)
                        {
                            const uint32_t tagFlatStart = static_cast<uint32_t>(chainBuf.size());
                            appendVifChainTagData(currentTagAddr);
                            if (channelBase == 0x10009000u &&
                                chainBuf.size() > tagFlatStart)
                            {
                                chainProvenance.push_back({
                                    tagFlatStart,
                                    static_cast<uint32_t>(chainBuf.size()),
                                    canonicalDmacAddress(currentTagAddr) + 8u,
                                    canonicalDmacAddress(currentTagAddr)});
                            }
                        }

                        if (hasPayload)
                            appendData(dataAddr, tagQwc, currentTagAddr);
                        if (channelBase == 0x1000A000u && tracedGifDestinationBlock() != 0u)
                        {
                            bool foundDestination = false;
                            for (size_t offset = flattenedStart + 8u;
                                 offset + 8u <= chainBuf.size(); offset += 8u)
                            {
                                uint64_t bitbltbuf = 0u;
                                uint64_t bitbltbufReg = 0u;
                                std::memcpy(&bitbltbuf, chainBuf.data() + offset - 8u,
                                            sizeof(bitbltbuf));
                                std::memcpy(&bitbltbufReg, chainBuf.data() + offset,
                                            sizeof(bitbltbufReg));
                                const uint32_t dbp =
                                    static_cast<uint32_t>((bitbltbuf >> 32u) & 0x3FFFu);
                                if ((bitbltbufReg & 0xFFu) == 0x50u &&
                                    dbp == tracedGifDestinationBlock())
                                {
                                    foundDestination = true;
                                    tracedDestinationFollowups = 4u;
                                    std::fprintf(stderr,
                                                 "[gif-destination-source] phase=setup start=%08x "
                                                 "tag=%08x id=%u qwc=%u payload=%08x source=%08x "
                                                 "flat=0x%zx bitblt=%016llx\n",
                                                 chainStartTagAddr, currentTagAddr, id, tagQwc,
                                                 dataAddr,
                                                 dataAddr + static_cast<uint32_t>(
                                                     offset - flattenedStart - 8u),
                                                 offset - 8u,
                                                 static_cast<unsigned long long>(bitbltbuf));
                                    break;
                                }
                            }

                            if (foundDestination || tracedDestinationFollowups != 0u)
                            {
                                std::fprintf(stderr,
                                             "[gif-destination-source] phase=payload start=%08x "
                                             "tag=%08x id=%u qwc=%u payload=%08x flat=0x%zx..0x%zx "
                                             "head=",
                                             chainStartTagAddr, currentTagAddr, id, tagQwc,
                                             dataAddr, flattenedStart, chainBuf.size());
                                const size_t dumpEnd = std::min<size_t>(
                                    chainBuf.size(), flattenedStart + 64u);
                                for (size_t index = flattenedStart; index < dumpEnd; ++index)
                                    std::fprintf(stderr, "%02x", chainBuf[index]);
                                std::fprintf(stderr, "\n");
                                if (!foundDestination)
                                    --tracedDestinationFollowups;
                            }
                        }
                        if (xmenVifChainMap)
                        {
                            std::fprintf(xmenVifChainMap,
                                         "0x%zx 0x%zx 0x%08x %u %u 0x%08x %u %u\n",
                                         flattenedStart, chainBuf.size(), currentTagAddr,
                                         id, tagQwc, dataAddr, irq ? 1u : 0u,
                                         tagTransferEnabled ? 1u : 0u);
                        }
                        if (channelBase == 0x1000A000u && chainBuf.size() >= flattenedStart + 80u)
                        {
                            static bool loggedXmenClutSetup = false;
                            for (size_t offset = flattenedStart + 8u;
                                 !loggedXmenClutSetup && offset + 8u <= chainBuf.size();
                                 offset += 8u)
                            {
                                uint64_t bitbltbuf = 0u;
                                uint64_t bitbltbufReg = 0u;
                                std::memcpy(&bitbltbuf, chainBuf.data() + offset - 8u, sizeof(bitbltbuf));
                                std::memcpy(&bitbltbufReg, chainBuf.data() + offset, sizeof(bitbltbufReg));
                                const uint32_t dbp = static_cast<uint32_t>((bitbltbuf >> 32u) & 0x3FFFu);
                                if ((bitbltbufReg & 0xFFu) == 0x50u && dbp == 12224u)
                                {
                                    const uint32_t sourceAddr = dataAddr +
                                        static_cast<uint32_t>(offset - flattenedStart - 8u);
                                    std::fprintf(stderr,
                                                 "[xmen-clut-setup-source] dmaTag=%08x id=%u qwc=%u "
                                                 "payload=%08x source=%08x flat=0x%zx bitblt=%016llx\n",
                                                 currentTagAddr, id, tagQwc, dataAddr, sourceAddr, offset - 8u,
                                                 static_cast<unsigned long long>(bitbltbuf));
                                    loggedXmenClutSetup = true;
                                }
                            }
                        }
                        if (xmenDiagnosticsEnabled() && channelBase == 0x10009000u &&
                            ((flattenedStart <= 9928u && chainBuf.size() > 9928u) ||
                             (flattenedStart <= 10404u && chainBuf.size() > 10404u)))
                        {
                            std::fprintf(stderr,
                                         "[xmen-vif-chain] flat=0x%zx..0x%zx tag=0x%x id=%u qwc=%u data=0x%x compact=%u\n",
                                         flattenedStart, chainBuf.size(), currentTagAddr,
                                         id, tagQwc, dataAddr, vifChannel ? 1u : 0u);
                        }
                        if (irq && tieEnabled)
                            endChain = true;
                        if (endChain)
                        {
                            chainEnded = true;
                            break;
                        }
                    }

                    if ((xmenDiagnosticsEnabled() || traceVifDmaProvenance()) &&
                        channelBase == 0x10009000u &&
                        (chainBuf.size() >= 0x10000u || tagsProcessed >= kMaxChainTags))
                    {
                        std::fprintf(stderr,
                            "[vif1:chain-summary] start=0x%x end=0x%x tags=%d/%d bytes=%zu "
                            "asp=%u tie=%u last-upper=0x%x\n",
                            chainStartTagAddr, tagAddr, tagsProcessed, kMaxChainTags,
                            chainBuf.size(), asp, tieEnabled ? 1u : 0u, lastTagUpper);
                    }
                    if (channelBase == 0x1000A000u &&
                        (traceGifChain ||
                         (xmenDiagnosticsEnabled() && dmaStartIndex >= 70u)))
                    {
                        uint64_t chainHash = 1469598103934665603ull;
                        for (const uint8_t byte : chainBuf)
                        {
                            chainHash ^= byte;
                            chainHash *= 1099511628211ull;
                        }
                        std::fprintf(stderr,
                                     "[xmen-gif-chain-complete] index=%u start=%08x end=%08x tags=%d bytes=%zu "
                                     "asp=%u tie=%u chcr=%08x queued=%u hash=%016llx head=",
                                     dmaStartIndex, chainStartTagAddr, tagAddr, tagsProcessed,
                                     chainBuf.size(), asp, tieEnabled ? 1u : 0u, chcr,
                                     chainBuf.empty() ? 0u : 1u,
                                     static_cast<unsigned long long>(chainHash));
                        for (size_t i = 0u; i < std::min<size_t>(chainBuf.size(), 32u); ++i)
                            std::fprintf(stderr, "%02x", static_cast<unsigned>(chainBuf[i]));
                        std::fprintf(stderr, "\n");
                    }

                    if (xmenDiagnosticsEnabled() && channelBase == 0x1000A000u)
                    {
                        uint32_t consoleDbp = 0u;
                        size_t consoleOffset = 0u;
                        if (findXmenConsoleTextureBitblt(chainBuf.data(), chainBuf.size(),
                                                        consoleDbp, consoleOffset))
                        {
                            std::fprintf(stderr,
                                         "[xmen-console-dma-chain] index=%u start=%08x end=%08x "
                                         "tags=%d bytes=%zu dbp=%u adOffset=0x%zx chcr=%08x\n",
                                         dmaStartIndex, chainStartTagAddr, tagAddr, tagsProcessed,
                                         chainBuf.size(), consoleDbp, consoleOffset, chcr);
                        }
                    }

                    if (xmenVifChainMap)
                    {
                        std::fclose(xmenVifChainMap);
                        xmenVifChainMap = nullptr;
                        if (std::FILE *chainDump =
                                std::fopen("xmen-vif-chain-984e80.bin", "wb"))
                        {
                            std::fwrite(chainBuf.data(), 1u, chainBuf.size(), chainDump);
                            std::fclose(chainDump);
                        }
                    }

                    m_ioRegisters[channelBase + 0x30] = tagAddr;
                    m_ioRegisters[channelBase + 0x40] = asr0;
                    m_ioRegisters[channelBase + 0x50] = asr1;
                    chcr = (chcr & ~(0x3u << 4)) | ((asp & 0x3u) << 4);
                    chcr = (chcr & 0x0000FFFFu) | (lastTagUpper << 16);
                    m_ioRegisters[channelBase + 0x00] = chcr;

                    if (channelBase == 0x10009000u)
                    {
                        ps2xRecordXmenVif1Chain(
                            gs().vsyncTick.load(std::memory_order_relaxed),
                            canonicalDmacAddress(chainStartTagAddr),
                            canonicalDmacAddress(tagAddr),
                            static_cast<uint32_t>(tagsProcessed),
                            static_cast<uint32_t>(chainBuf.size()), chcr,
                            chainEnded ? 1u : 0u);
                    }

                    if (!chainBuf.empty() || chainEnded)
                    {
                        PendingTransfer pt;
                        pt.fromScratchpad = false;
                        pt.srcAddr = 0;
                        pt.qwc = 0;
                        pt.dmaTagCount = static_cast<uint32_t>(tagsProcessed);
                        pt.chainData = std::move(chainBuf);
                        if (!chainProvenance.empty())
                        {
                            registerVif1TraceProvenance(
                                pt.chainData.data(), std::move(chainProvenance));
                        }
                        if (channelBase == 0x1000A000)
                        {
                            m_pendingGifTransfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10009000u)
                        {
                            m_pendingVif1Transfers.push_back(std::move(pt));
                        }
                        else if (channelBase == 0x10008000u)
                        {
                            m_pendingVif0Transfers.push_back(std::move(pt));
                        }
                    }
                    // else if (channelBase == 0x10009000u)
                    // {

                    // }
                }
                else if (qwc > 0)
                {
                    enqueueTransfer(madr, qwc);
                }

                const bool autoProcessTransfers =
                    (channelBase == 0x1000A000u) ? (m_gifPacketCallback || m_gifArbiter != nullptr) : true;
                if (autoProcessTransfers)
                {
                    processPendingTransfers();
                }
            }
            else if (channelBase == 0x1000D000u && ((value >> 2u) & 0x3u) == 0u && qwc > 0u)
            {
                uint32_t dst = madr & 0x1FFFFFFFu;
                uint32_t src = m_ioRegisters[channelBase + 0x80u] & (PS2_SCRATCHPAD_SIZE - 1u);
                uint32_t bytesLeft = qwc * 16u;

                while (bytesLeft > 0u)
                {
                    dst %= PS2_RAM_SIZE;
                    src &= PS2_SCRATCHPAD_SIZE - 1u;
                    const uint32_t chunk = std::min({bytesLeft, PS2_RAM_SIZE - dst, PS2_SCRATCHPAD_SIZE - src});
                    if (chunk == 0u)
                        break;
                    std::memcpy(m_rdram + dst, m_scratchpad + src, chunk);
                    dst += chunk;
                    src += chunk;
                    bytesLeft -= chunk;
                }

                const uint32_t transferredBytes = qwc * 16u - bytesLeft;
                m_ioRegisters[channelBase + 0x10u] = madr + transferredBytes;
                m_ioRegisters[channelBase + 0x20u] = bytesLeft / 16u;
                m_ioRegisters[channelBase + 0x80u] = src & (PS2_SCRATCHPAD_SIZE - 1u);
                m_ioRegisters[channelBase + 0x00u] &= ~0x100u;

                if (((m_ioRegisters[0x1000E000u] >> 4u) & 0x3u) == 3u)
                    m_ioRegisters[0x1000E060u] = madr + transferredBytes;

                uint32_t dstat = m_ioRegisters[0x1000E010u] | (1u << 8u);
                const uint32_t status = dstat & 0x3FFu;
                const uint32_t mask = (dstat >> 16u) & 0x3FFu;
                if ((status & mask) != 0u)
                    dstat |= 1u << 31u;
                else
                    dstat &= ~(1u << 31u);
                m_ioRegisters[0x1000E010u] = dstat;
                queueCompletedDmacCause(8u);
            }
            else if (channelBase == 0x1000D400u && ((value >> 2u) & 0x3u) == 0u && qwc > 0u)
            {
                uint32_t src = madr & 0x1FFFFFFFu;
                uint32_t dst = m_ioRegisters[channelBase + 0x80u] & (PS2_SCRATCHPAD_SIZE - 1u);
                uint32_t bytesLeft = qwc * 16u;

                while (bytesLeft > 0u)
                {
                    src %= PS2_RAM_SIZE;
                    dst &= PS2_SCRATCHPAD_SIZE - 1u;
                    const uint32_t chunk = std::min({bytesLeft, PS2_RAM_SIZE - src, PS2_SCRATCHPAD_SIZE - dst});
                    if (chunk == 0u)
                        break;
                    std::memcpy(m_scratchpad + dst, m_rdram + src, chunk);
                    src += chunk;
                    dst += chunk;
                    bytesLeft -= chunk;
                }

                const uint32_t transferredBytes = qwc * 16u - bytesLeft;
                m_ioRegisters[channelBase + 0x10u] = madr + transferredBytes;
                m_ioRegisters[channelBase + 0x20u] = bytesLeft / 16u;
                m_ioRegisters[channelBase + 0x80u] = dst & (PS2_SCRATCHPAD_SIZE - 1u);
                m_ioRegisters[channelBase + 0x00u] &= ~0x100u;

                uint32_t dstat = m_ioRegisters[0x1000E010u] | (1u << 9u);
                const uint32_t status = dstat & 0x3FFu;
                const uint32_t mask = (dstat >> 16u) & 0x3FFu;
                if ((status & mask) != 0u)
                    dstat |= 1u << 31u;
                else
                    dstat &= ~(1u << 31u);
                m_ioRegisters[0x1000E010u] = dstat;
                queueCompletedDmacCause(9u);
            }
        }
        return true;
    }

    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10000200 && address < 0x10000300)
        {
            return true;
        }
        if (address >= 0x10000000 && address < 0x10000100)
        {
            return true;
        }
    }

    return false;
}

void PS2Memory::processPendingTransfers()
{
    reportRdramPageWatch(gs().vsyncTick.load(std::memory_order_relaxed));
    const bool hadGif = !m_pendingGifTransfers.empty();
    uint64_t gifTransferQwc = 0u;
    uint64_t gifDmaTagCount = 0u;
    for (size_t idx = 0; idx < m_pendingGifTransfers.size(); ++idx)
    {
        auto &p = m_pendingGifTransfers[idx];
        gifDmaTagCount += p.dmaTagCount;
        if (!p.chainData.empty())
        {
            gifTransferQwc += (p.chainData.size() + 15u) / 16u;
            m_seenGifCopy = true;
            m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
            submitGifPacket(GifPathId::Path3, p.chainData.data(), static_cast<uint32_t>(p.chainData.size()), false);
        }
        else if (p.qwc > 0)
        {
            gifTransferQwc += p.qwc;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            uint32_t srcPhys = 0;
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_scratchpad + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft >= 16)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    m_seenGifCopy = true;
                    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
                    submitGifPacket(GifPathId::Path3, m_rdram + srcPhys, chunk, false);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingGifTransfers.clear();

    const bool hadVif0 = !m_pendingVif0Transfers.empty();
    for (auto &p : m_pendingVif0Transfers)
    {
        if (!p.chainData.empty())
        {
            processVIF0Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF0Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif0Transfers.clear();

    const bool hadVif1 = !m_pendingVif1Transfers.empty();
    for (auto &p : m_pendingVif1Transfers)
    {
        if (!p.chainData.empty())
        {
            beginVif1TraceProvenance(p.chainData.data());
            processVIF1Data(p.chainData.data(), static_cast<uint32_t>(p.chainData.size()));
            endVif1TraceProvenance(p.chainData.data());
        }
        else if (p.qwc > 0)
        {
            uint32_t srcPhys = 0;
            const uint64_t bytes64 = static_cast<uint64_t>(p.qwc) * 16ull;
            uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            try
            {
                srcPhys = translateAddress(p.srcAddr);
            }
            catch (const std::exception &)
            {
                continue;
            }
            if (p.fromScratchpad)
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_SCRATCHPAD_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_SCRATCHPAD_SIZE)
                        chunk = PS2_SCRATCHPAD_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(m_scratchpad + srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
            else
            {
                uint32_t bytesLeft = sizeBytes;
                while (bytesLeft > 0)
                {
                    if (srcPhys >= PS2_RAM_SIZE)
                        srcPhys = 0;
                    uint32_t chunk = bytesLeft;
                    if (srcPhys + chunk > PS2_RAM_SIZE)
                        chunk = PS2_RAM_SIZE - srcPhys;
                    if (chunk == 0)
                        break;
                    processVIF1Data(srcPhys, chunk);
                    bytesLeft -= chunk;
                    srcPhys += chunk;
                }
            }
        }
    }
    m_pendingVif1Transfers.clear();

    if (m_gifArbiter)
        m_gifArbiter->drain();

    constexpr uint32_t D_STAT = 0x1000E010u;
    auto raiseDStatChannel = [&](uint32_t channelBit)
    {
        uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
        dstat |= 1u << channelBit;
        const uint32_t status = dstat & 0x3FFu;
        const uint32_t mask = (dstat >> 16u) & 0x3FFu;
        if ((status & mask) != 0u)
            dstat |= 1u << 31u;
        else
            dstat &= ~(1u << 31u);
        m_ioRegisters[D_STAT] = dstat;
    };

    if (hadGif)
    {
        raiseDStatChannel(2u);
        queueCompletedDmacCause(2u, gifDmacCompletionCycles(gifTransferQwc, gifDmaTagCount));
        m_ioRegisters[0x1000A000u] &= ~0x100u;
        m_ioRegisters[0x1000A020u] = 0u;
    }
    if (hadVif0)
    {
        raiseDStatChannel(0u);
        queueCompletedDmacCause(0u);
        m_ioRegisters[0x10008000u] &= ~0x100u;
        m_ioRegisters[0x10008020u] = 0u;
    }
    if (hadVif1)
    {
        raiseDStatChannel(1u);
        queueCompletedDmacCause(1u);
        m_ioRegisters[0x10009000u] &= ~0x100u;
        m_ioRegisters[0x10009020u] = 0u;
    }
}

void PS2Memory::queueCompletedDmacCause(uint32_t cause, uint64_t delayCycles)
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    m_completedDmacCauses.push_back(DmacCompletion{cause, delayCycles});
}

std::vector<PS2Memory::DmacCompletion> PS2Memory::consumeCompletedDmacCauses()
{
    std::lock_guard<std::mutex> lock(m_completedDmacMutex);
    std::vector<DmacCompletion> causes;
    causes.swap(m_completedDmacCauses);
    return causes;
}

void PS2Memory::flushMaskedPath3Packets(bool drainImmediately)
{
    if (m_path3Masked || m_path3MaskedFifo.empty())
        return;

    auto emit = [&](const uint8_t *packetData, uint32_t packetSize)
    {
        if (m_gifArbiter)
            m_gifArbiter->submit(GifPathId::Path3, packetData, packetSize, false);
        else if (m_gifPacketCallback)
            m_gifPacketCallback(packetData, packetSize);
    };

    for (const auto &packet : m_path3MaskedFifo)
    {
        if (packet.size() >= 16u)
            emit(packet.data(), static_cast<uint32_t>(packet.size()));
    }
    m_path3MaskedFifo.clear();

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool drainImmediately, bool path2DirectHl)
{
    if (!data || sizeBytes < 16)
        return;

    uint64_t tagLo = 0u;
    uint64_t tagHi = 0u;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    std::memcpy(&tagHi, data + sizeof(tagLo), sizeof(tagHi));
    static std::atomic<uint32_t> submitTraceCount{0u};
    const uint32_t submitTraceIndex = submitTraceCount.fetch_add(1u, std::memory_order_relaxed);
    if (xmenDiagnosticsEnabled() && submitTraceIndex < 128u)
    {
        std::fprintf(stderr,
                     "[gif-submit] idx=%u path=%u bytes=%u drain=%u arbiter=%u callback=%u masked=%u directHl=%u tagLo=0x%016llx tagHi=0x%016llx\n",
                     submitTraceIndex,
                     static_cast<unsigned>(pathId),
                     sizeBytes,
                     drainImmediately ? 1u : 0u,
                     m_gifArbiter ? 1u : 0u,
                     m_gifPacketCallback ? 1u : 0u,
                     m_path3Masked ? 1u : 0u,
                     path2DirectHl ? 1u : 0u,
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi));
    }


    uint32_t consoleDbp = 0u;
    size_t consoleOffset = 0u;
    if (xmenDiagnosticsEnabled() &&
        findXmenConsoleTextureBitblt(data, sizeBytes, consoleDbp, consoleOffset))
    {
        std::fprintf(stderr,
                     "[xmen-console-submit] path=%u bytes=%u drain=%u directHl=%u "
                     "dbp=%u adOffset=0x%zx\n",
                     static_cast<unsigned>(pathId), sizeBytes,
                     drainImmediately ? 1u : 0u, path2DirectHl ? 1u : 0u,
                     consoleDbp, consoleOffset);
    }

    if (pathId == GifPathId::Path3 && sizeBytes >= 80u)
    {
        static bool loggedXmenClutSubmission = false;
        for (uint32_t offset = 8u;
             !loggedXmenClutSubmission && offset + 8u <= sizeBytes;
             offset += 8u)
        {
            uint64_t bitbltbuf = 0u;
            uint64_t bitbltbufReg = 0u;
            std::memcpy(&bitbltbuf, data + offset - 8u, sizeof(bitbltbuf));
            std::memcpy(&bitbltbufReg, data + offset, sizeof(bitbltbufReg));
            const uint32_t dbp = static_cast<uint32_t>((bitbltbuf >> 32u) & 0x3FFFu);
            if ((bitbltbufReg & 0xFFu) == 0x50u && dbp == 12224u)
            {
                const char *storage = "staged";
                uint32_t sourceAddr = 0u;
                if (m_rdram && data >= m_rdram && data < m_rdram + PS2_RAM_SIZE)
                {
                    storage = "rdram";
                    sourceAddr = static_cast<uint32_t>(data - m_rdram) + offset - 8u;
                }
                else if (m_scratchpad && data >= m_scratchpad && data < m_scratchpad + PS2_SCRATCHPAD_SIZE)
                {
                    storage = "scratchpad";
                    sourceAddr = 0x70000000u + static_cast<uint32_t>(data - m_scratchpad) + offset - 8u;
                }
                std::fprintf(stderr,
                             "[xmen-clut-submit-source] storage=%s source=%08x offset=0x%x "
                             "packetBytes=%u bitblt=%016llx\n",
                             storage, sourceAddr, offset - 8u, sizeBytes,
                             static_cast<unsigned long long>(bitbltbuf));
                loggedXmenClutSubmission = true;
            }
        }
    }

    if (pathId == GifPathId::Path3)
    {
        if (m_path3Masked)
        {
            m_path3MaskedFifo.emplace_back(data, data + sizeBytes);
            return;
        }
        flushMaskedPath3Packets(false);
    }

    if (m_gifArbiter)
        m_gifArbiter->submit(pathId, data, sizeBytes, path2DirectHl);
    else if (m_gifPacketCallback)
        m_gifPacketCallback(data, sizeBytes);

    if (m_gifArbiter && drainImmediately)
        m_gifArbiter->drain();
}

void PS2Memory::processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount)
{
    if (!m_rdram || qwCount == 0)
        return;
    const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
    uint32_t sizeBytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
    uint32_t bytesLeft = sizeBytes;
    while (bytesLeft >= 16)
    {
        if (srcPhysAddr >= PS2_RAM_SIZE)
            srcPhysAddr = 0;
        uint32_t chunk = bytesLeft;
        if (srcPhysAddr + chunk > PS2_RAM_SIZE)
            chunk = PS2_RAM_SIZE - srcPhysAddr;
        if (chunk == 0)
            break;

        m_seenGifCopy = true;
        m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
        submitGifPacket(GifPathId::Path3, m_rdram + srcPhysAddr, chunk);

        bytesLeft -= chunk;
        srcPhysAddr += chunk;
    }
}

void PS2Memory::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (m_gifArbiter)
        submitGifPacket(GifPathId::Path3, data, sizeBytes);
    else if (m_gifPacketCallback && data && sizeBytes >= 16)
        m_gifPacketCallback(data, sizeBytes);
}

bool PS2Memory::tryProcessNativeGifImageUploadChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    auto reject = [&](const char *reason) -> bool
    {
        static std::atomic<uint32_t> s_rejectTraceCount{0u};
        const uint32_t traceIndex = s_rejectTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 96u || traceGifDmaChains())
        {
            std::fprintf(stderr,
                         "[gif-native-image-reject] index=%u reason=%s tadr=%08x chcr=%08x "
                         "rdram=%u vram=%u masked=%u arbiter=%u\n",
                         traceIndex, reason, tadr, chcr,
                         m_rdram ? 1u : 0u,
                         m_gsVRAM ? 1u : 0u,
                         m_path3Masked ? 1u : 0u,
                         (m_gifArbiter && !m_gifArbiter->empty()) ? 1u : 0u);
        }
        return false;
    };

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return reject("state");
    if (m_gifArbiter && !m_gifArbiter->empty())
        return reject("arbiter-not-empty");
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return reject("chcr-mode");
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return reject("chcr-tie-asp");

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return reject("dmac-disabled");

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    auto loadDmaTagAt = [&](uint32_t guestAddr, DmaTagView &out) -> bool
    {
        const uint8_t *ptr = nullptr;
        if (!resolveContiguous(guestAddr, 16u, ptr))
            return false;
        out = decodeDmaTag(loadScalar<uint64_t>(ptr, 0u, 16u, "native gif dma tag", guestAddr));
        return true;
    };

    auto decodeSetupPayload = [&](const uint8_t *payload, uint64_t (&regs)[4]) -> bool
    {
        const uint64_t tagLo = loadScalar<uint64_t>(payload, 0u, 80u, "native gif setup tag", 0u);
        const uint64_t tagHi = loadScalar<uint64_t>(payload, 8u, 80u, "native gif setup regs", 0u);
        if (gifTagNloop(tagLo) != 4u ||
            gifTagFlg(tagLo) != GIF_FMT_PACKED ||
            gifTagNreg(tagLo) != 1u ||
            (tagHi & 0xFull) != 0x0Eull)
        {
            return false;
        }

        static constexpr uint8_t kExpectedRegs[4] = {
            GS_REG_BITBLTBUF,
            GS_REG_TRXPOS,
            GS_REG_TRXREG,
            GS_REG_TRXDIR,
        };

        uint32_t offset = 16u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            regs[i] = loadScalar<uint64_t>(payload, offset, 80u, "native gif setup value", 0u);
            const uint64_t reg = loadScalar<uint64_t>(payload, offset + 8u, 80u, "native gif setup register", 0u);
            if ((reg & 0xFFu) != kExpectedRegs[i])
                return false;
            offset += 16u;
        }

        const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
        const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
        const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
        return trxdirMode == 0u && rrw != 0u && rrh != 0u;
    };

    DmaTagView setupTag{};
    if (!loadDmaTagAt(tadr, setupTag) ||
        setupTag.id != 1u ||
        setupTag.qwc != 5u ||
        setupTag.irq)
    {
        return reject("setup-dma-tag");
    }

    const uint8_t *setupPayload = nullptr;
    const uint32_t setupPayloadAddr = tadr + 16u;
    if (!resolveContiguous(setupPayloadAddr, 5u * 16u, setupPayload))
        return reject("setup-payload");

    uint64_t setupRegs[4] = {};
    if (!decodeSetupPayload(setupPayload, setupRegs))
        return reject("setup-payload-shape");

    uint32_t imageTagDmaAddr = setupPayloadAddr + 5u * 16u;
    DmaTagView imageTagDma{};
    if (!loadDmaTagAt(imageTagDmaAddr, imageTagDma) ||
        imageTagDma.id != 1u ||
        imageTagDma.qwc != 1u ||
        imageTagDma.irq)
    {
        return reject("image-dma-tag");
    }

    const uint8_t *imageGifTag = nullptr;
    if (!resolveContiguous(imageTagDmaAddr + 16u, 16u, imageGifTag))
        return reject("image-gif-tag");

    const uint64_t imageTagLo = loadScalar<uint64_t>(imageGifTag, 0u, 16u, "native gif image tag", imageTagDmaAddr + 16u);
    const uint8_t imageFormat = gifTagFlg(imageTagLo);
    if (imageFormat != GIF_FMT_IMAGE && imageFormat != GIF_FMT_IMAGE2)
        return reject("image-giftag-flg");

    const uint32_t imageQwc = gifTagNloop(imageTagLo);
    if (imageQwc == 0u)
        return reject("image-empty");

    const uint64_t imageBytes64 = static_cast<uint64_t>(imageQwc) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return reject("image-too-large");
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);

    const uint32_t payloadTagAddr = imageTagDmaAddr + 32u;
    DmaTagView payloadTag{};
    if (!loadDmaTagAt(payloadTagAddr, payloadTag) ||
        payloadTag.qwc != imageQwc ||
        payloadTag.irq)
    {
        return reject("payload-dma-tag");
    }

    uint32_t imageDataAddr = 0u;
    uint32_t finalTadr = payloadTagAddr;
    uint32_t lastTagUpper = payloadTag.upper;
    if (payloadTag.id == 3u || payloadTag.id == 4u)
    {
        imageDataAddr = payloadTag.addr;
        const uint32_t terminalTagAddr = payloadTagAddr + 16u;
        DmaTagView terminalTag{};
        if (!loadDmaTagAt(terminalTagAddr, terminalTag) ||
            terminalTag.qwc != 0u ||
            terminalTag.irq ||
            (terminalTag.id != 0u && terminalTag.id != 7u))
        {
            return reject("terminal-dma-tag");
        }
        finalTadr = (terminalTag.id == 0u) ? (terminalTagAddr + 16u) : terminalTagAddr;
        lastTagUpper = terminalTag.upper;
    }
    else if (payloadTag.id == 7u)
    {
        imageDataAddr = payloadTagAddr + 16u;
        finalTadr = payloadTagAddr;
    }
    else
    {
        return reject("payload-dma-id");
    }

    const uint8_t *imageData = nullptr;
    if (!resolveContiguous(imageDataAddr, imageBytes, imageData))
        return reject("image-data");

    const uint32_t uploadDbp = static_cast<uint32_t>((setupRegs[0] >> 32u) & 0x3FFFu);
    static std::atomic<uint32_t> s_acceptTraceCount{0u};
    const uint32_t acceptIndex = s_acceptTraceCount.fetch_add(1u, std::memory_order_relaxed);
    if (acceptIndex < 96u || uploadDbp == 11200u || uploadDbp == 12224u ||
        traceGifDmaChains())
    {
        uint64_t imageHash = 1469598103934665603ull;
        for (uint32_t i = 0u; i < imageBytes; ++i)
        {
            imageHash ^= imageData[i];
            imageHash *= 1099511628211ull;
        }
        std::fprintf(stderr,
                     "[gif-native-image-accept] index=%u dbp=%u tadr=%08x setup=%08x image-tag=%08x payload-tag=%08x "
                     "data=%08x qwc=%u bytes=%u trxreg=%016llx bitblt=%016llx hash=%016llx head=",
                     acceptIndex, uploadDbp, tadr, setupPayloadAddr, imageTagDmaAddr, payloadTagAddr,
                     imageDataAddr, imageQwc, imageBytes,
                     static_cast<unsigned long long>(setupRegs[2]),
                     static_cast<unsigned long long>(setupRegs[0]),
                     static_cast<unsigned long long>(imageHash));
        for (uint32_t i = 0u; i < std::min<uint32_t>(imageBytes, 32u); ++i)
            std::fprintf(stderr, "%02x", static_cast<unsigned>(imageData[i]));
        std::fprintf(stderr, "\n");
    }

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);
    gs.uploadImageNative(setupRegs[0], setupRegs[1], setupRegs[2], setupRegs[3], imageData, imageBytes);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = finalTadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] =
        ((chcr & 0x0000FFFFu) | (lastTagUpper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= 1u << 2u;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    const uint64_t transferredQwc = 5u + 1u + imageQwc;
    const uint64_t dmaTagCount = (payloadTag.id == 7u) ? 3u : 4u;
    queueCompletedDmacCause(2u, gifDmacCompletionCycles(transferredQwc, dmaTagCount));
    return true;
}

bool PS2Memory::tryProcessNativeGifPackedChain(GS &gs, uint32_t tadr, uint32_t chcr)
{
    static constexpr uint32_t GIF_CHANNEL = 0x1000A000u;
    static constexpr uint32_t D_STAT = 0x1000E010u;
    static constexpr uint32_t D_CTRL = 0x1000E000u;

    if (!m_rdram || !m_gsVRAM || m_path3Masked)
        return false;
    if (m_gifArbiter && !m_gifArbiter->empty())
        return false;
    if ((chcr & 0x100u) == 0u || ((chcr >> 2u) & 0x3u) != 1u)
        return false;
    if ((chcr & (1u << 7u)) != 0u || ((chcr >> 4u) & 0x3u) != 0u)
        return false;

    const auto dctrlIt = m_ioRegisters.find(D_CTRL);
    if (dctrlIt != m_ioRegisters.end() && ((dctrlIt->second & 0x1u) == 0u))
        return false;

    auto resolveContiguous = [&](uint32_t guestAddr, uint32_t bytes, const uint8_t *&out) -> bool
    {
        try
        {
            const bool scratch = isScratchpad(guestAddr);
            const uint32_t phys = translateAddress(guestAddr);
            const uint8_t *base = scratch ? m_scratchpad : m_rdram;
            const uint32_t limit = scratch ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
            if (!base || phys > limit || bytes > limit - phys)
                return false;
            out = base + phys;
            return true;
        }
        catch (const std::exception &)
        {
            return false;
        }
    };

    const uint8_t *tagPtr = nullptr;
    if (!resolveContiguous(tadr, 16u, tagPtr))
        return false;

    const DmaTagView tag = decodeDmaTag(loadScalar<uint64_t>(tagPtr, 0u, 16u, "native packed gif dma tag", tadr));
    if (tag.id != 7u || tag.qwc == 0u || tag.irq)
        return false;

    const uint64_t payloadBytes64 = static_cast<uint64_t>(tag.qwc) * 16ull;
    if (payloadBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);

    const uint8_t *payload = nullptr;
    if (!resolveContiguous(tadr + 16u, payloadBytes, payload))
        return false;
    if (!gs.processNativePackedGIFPacket(payload, payloadBytes))
        return false;

    m_dmaStartCount.fetch_add(1, std::memory_order_relaxed);
    m_seenGifCopy = true;
    m_gifCopyCount.fetch_add(1, std::memory_order_relaxed);

    m_ioRegisters[GIF_CHANNEL + 0x30u] = tadr;
    m_ioRegisters[GIF_CHANNEL + 0x40u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x50u] = 0u;
    m_ioRegisters[GIF_CHANNEL + 0x00u] =
        ((chcr & 0x0000FFFFu) | (tag.upper << 16u)) & ~0x100u;
    m_ioRegisters[GIF_CHANNEL + 0x20u] = 0u;

    uint32_t dstat = m_ioRegisters.count(D_STAT) ? m_ioRegisters[D_STAT] : 0u;
    dstat |= 1u << 2u;
    const uint32_t status = dstat & 0x3FFu;
    const uint32_t mask = (dstat >> 16u) & 0x3FFu;
    if ((status & mask) != 0u)
        dstat |= 1u << 31u;
    else
        dstat &= ~(1u << 31u);
    m_ioRegisters[D_STAT] = dstat;
    queueCompletedDmacCause(2u, gifDmacCompletionCycles(tag.qwc, 1u));
    return true;
}

int PS2Memory::pollDmaRegisters()
{
    return 0;
}

uint32_t PS2Memory::readIORegister(uint32_t address)
{
    size_t timerIndex = 0u;
    uint32_t timerOffset = 0u;
    if (decodeEeTimerRegister(address, timerIndex, timerOffset))
    {
        const EeTimer &timer = m_eeTimers[timerIndex];
        switch (timerOffset)
        {
        case kEeTimerCountOffset:
            return timer.count & 0xFFFFu;
        case kEeTimerModeOffset:
            return timer.mode & (kEeTimerModeConfigMask | kEeTimerModeStatusMask);
        case kEeTimerCompareOffset:
            return timer.compare & 0xFFFFu;
        case kEeTimerHoldOffset:
            return timer.hold & 0xFFFFu;
        default:
            return 0u;
        }
    }

    if (isGsPrivReg(address))
    {
        // NB: unreachable from read8/16/32/64 today, same reasoning as the write
        // path above; kept correct for direct callers.
        const uint32_t off = address & 7u;
        const uint32_t regOff = (address - PS2_GS_PRIV_REG_BASE) & ~0x7u;
        if (regOff == kGsCsrRegOffset)
        {
            return static_cast<uint32_t>((gs_regs.csr.load() >> (off * 8u)) & 0xFFFFFFFFull);
        }
        if (uint64_t *reg = gsRegPtr(gs_regs, address))
        {
            return static_cast<uint32_t>((*reg >> (off * 8u)) & 0xFFFFFFFFull);
        }
        return 0u;
    }

    if (address >= 0x10002000 && address <= 0x10002030)
    {
        uint32_t val = 0;
        switch (address)
        {
        case 0x10002000:
            val = m_ioRegisters[address];
            break;
        case 0x10002010:
            val = m_ioRegisters[address] & ~(1u << 31);
            break;
        case 0x10002020:
        case 0x10002030:
            val = m_ioRegisters[address];
            break;
        default:
            val = 0;
            break;
        }
        return val;
    }
    if (address >= 0x10000000 && address < 0x10010000)
    {
        if (address >= 0x10008000 && address < 0x1000F000)
        {
            if ((address & 0xFF) == 0x00)
            {
                uint32_t channelStatus = m_ioRegisters[address] & ~0x100u;
                m_ioRegisters[address] = channelStatus;
                return channelStatus;
            }
        }

        if (address >= 0x10000200 && address < 0x10000300)
        {
            return 0;
        }

        if (address >= 0x1000F200 && address <= 0x1000F260)
        {
            if (address == 0x1000F230)
            {
                return 0x60000;
            }
            if (address == 0x1000F240)
            {
                return 0xF0000002;
            }
            return 0;
        }
    }

    auto it = m_ioRegisters.find(address);
    if (it != m_ioRegisters.end())
    {
        return it->second;
    }

    return 0;
}

void PS2Memory::registerCodeRegion(uint32_t start, uint32_t end)
{
    if (end <= start)
    {
        std::cerr << "Ignoring invalid code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    if ((end - start) > PS2_RAM_SIZE)
    {
        std::cerr << "Ignoring oversized code region: start=0x" << std::hex << start
                  << " end=0x" << end << std::dec << std::endl;
        return;
    }

    for (const auto &existing : m_codeRegions)
    {
        if (existing.start == start && existing.end == end)
        {
            return;
        }
    }

    CodeRegion region;
    region.start = start;
    region.end = end;

    size_t sizeInWords = (end - start + 3u) / 4u;
    region.modified.resize(sizeInWords, false);

    m_codeRegions.push_back(region);
    RUNTIME_LOG("Registered code region: " << std::hex << start << " - " << end << std::dec);
}

bool PS2Memory::isAddressInRegion(uint32_t address, const CodeRegion &region)
{
    return (address >= region.start && address < region.end);
}

bool PS2Memory::isCodeAddress(uint32_t address) const
{
    for (const auto &region : m_codeRegions)
    {
        if (address >= region.start && address < region.end)
        {
            return true;
        }
    }
    return false;
}

void PS2Memory::markModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = true;
                RUNTIME_LOG("Marked code at " << std::hex << addr << std::dec << " as modified");
            }
        }
    }
}

bool PS2Memory::isCodeModified(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return false;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (const auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size() && region.modified[bitIndex])
            {
                return true; // Found modified code
            }
        }
    }

    return false; // No modifications found
}

void PS2Memory::clearModifiedFlag(uint32_t address, uint32_t size)
{
    if (size == 0)
    {
        return;
    }

    const uint64_t writeEnd = static_cast<uint64_t>(address) + static_cast<uint64_t>(size);
    for (auto &region : m_codeRegions)
    {
        const uint64_t regionStart = region.start;
        const uint64_t regionEnd = region.end;
        if (writeEnd <= regionStart || static_cast<uint64_t>(address) >= regionEnd)
        {
            continue;
        }

        uint32_t overlapStart = static_cast<uint32_t>(std::max<uint64_t>(address, regionStart));
        uint32_t overlapEnd = static_cast<uint32_t>(std::min<uint64_t>(writeEnd, regionEnd));

        for (uint32_t addr = overlapStart; addr < overlapEnd; addr += 4)
        {
            size_t bitIndex = (addr - region.start) / 4;
            if (bitIndex < region.modified.size())
            {
                region.modified[bitIndex] = false;
            }
        }
    }
}
