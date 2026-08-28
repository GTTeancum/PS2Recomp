#include "ps2_runtime.h"
#include "ps2_log.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"
#include "game_overrides.h"
#include "ps2_runtime_macros.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ee_scheduler.h"
#include "ThreadNaming.h"
#include "Kernel/Stubs/Audio.h"
#include "Kernel/Stubs/GS.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_host_backend.h"
#include "ps2_iop_host.h"
#include "ps2x/iop/iop_subsystem.h"

#include <iostream>
#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <chrono>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <sstream>
#include <cstdio>
#include <zlib.h>

namespace ps2_stubs
{
    void resetSifState();
}

static std::atomic<bool> g_xmenTitleBranchTraceArmed{false};
static std::atomic<uint32_t> g_xmenTitleBranchTraceCount{0u};
static std::atomic<uint32_t> g_xmenMainBackIgbPackage{0u};
static std::atomic<uint32_t> g_xmenMainBackIgbObjectEntries{0u};
static std::atomic<bool> g_xmenVu1MailboxTraceArmed{false};
static std::atomic<uint32_t> g_xmenVu1MailboxTraceSlice{0u};
static thread_local uint32_t g_xmenLiveChainSavedReturnSlot = 0u;
static thread_local uint32_t g_xmenLiveChainExpectedReturn = 0u;
static thread_local uint32_t g_xmenLiveChainSequence = 0u;
static thread_local uint64_t g_xmenLiveChainLastObserved = 0u;

static void traceXmenLiveChainWrite(uint8_t *rdram,
                                    uint32_t guestAddr,
                                    uint32_t size,
                                    uint64_t valueLo,
                                    uint64_t valueHi,
                                    const char *op,
                                    const R5900Context *ctx)
{
    const uint32_t slot = g_xmenLiveChainSavedReturnSlot;
    if (slot == 0u)
        return;

    const uint32_t physical = guestAddr & PS2_RAM_MASK;
    const uint64_t end = static_cast<uint64_t>(physical) + size;
    if (physical >= slot + sizeof(uint64_t) || end <= slot)
        return;

    uint64_t previous = 0u;
    std::memcpy(&previous, rdram + slot, sizeof(previous));
    static std::atomic<uint32_t> s_logCount{0u};
    const uint32_t index = s_logCount.fetch_add(1u, std::memory_order_relaxed);
    if (index < 4096u)
    {
        std::cerr << "[xmen-live-chain-write] index=" << std::dec << index
                  << " sequence=" << g_xmenLiveChainSequence
                  << " op=" << op
                  << " addr=0x" << std::hex << guestAddr
                  << " size=0x" << size
                  << " slot=0x" << slot
                  << " expected=0x" << g_xmenLiveChainExpectedReturn
                  << " previous=0x" << previous
                  << " lo=0x" << valueLo
                  << " hi=0x" << valueHi
                  << " pc=0x" << (ctx ? ctx->pc : 0u)
                  << " ra=0x" << getRegU32(ctx, 31)
                  << " sp=0x" << getRegU32(ctx, 29)
                  << std::dec << std::endl;
    }
}

static void traceXmenVu1Mailbox(const char *phase, uint32_t slice,
                               const VU1Interpreter &vu1, const uint8_t *vuData)
{
    if (!vuData)
        return;

    uint32_t qword2b[4]{};
    uint32_t qword4e[4]{};
    std::memcpy(qword2b, vuData + 0x2Bu * 16u, sizeof(qword2b));
    std::memcpy(qword4e, vuData + 0x4Eu * 16u, sizeof(qword4e));
    const VU1State &state = vu1.state();
    std::fprintf(stderr,
                 "[xmen-vu1-mailbox] phase=%s slice=%u running=%u pc=0x%x "
                 "vi4=0x%x branch=%u target=0x%x delay=%u "
                 "q2b=%08x,%08x,%08x,%08x q4e=%08x,%08x,%08x,%08x\n",
                 phase, slice, vu1.isRunning() ? 1u : 0u, state.pc,
                 static_cast<uint32_t>(state.vi[4]) & 0xFFFFu,
                 state.branchPending ? 1u : 0u, state.branchTarget, state.branchDelay,
                 qword2b[0], qword2b[1], qword2b[2], qword2b[3],
                 qword4e[0], qword4e[1], qword4e[2], qword4e[3]);
}

static void traceXmenVu1MailboxCode(const uint8_t *vuCode)
{
    if (!vuCode)
        return;

    for (uint32_t pc = 0x80u; pc <= 0x128u; pc += 8u)
    {
        uint32_t lower = 0u;
        uint32_t upper = 0u;
        std::memcpy(&lower, vuCode + pc, sizeof(lower));
        std::memcpy(&upper, vuCode + pc + sizeof(lower), sizeof(upper));
        std::fprintf(stderr,
                     "[xmen-vu1-mailbox-code] pc=0x%03x lower=%08x upper=%08x\n",
                     pc, lower, upper);
    }
}

void ps2xArmXmenTitleBranchTrace()
{
    g_xmenTitleBranchTraceCount.store(0u, std::memory_order_relaxed);
    g_xmenTitleBranchTraceArmed.store(true, std::memory_order_release);
}

#define ELF_MAGIC 0x464C457F // "\x7FELF" in little endian
#define ET_EXEC 2            // Executable file
#define EM_MIPS 8            // MIPS architecture
#define PT_LOAD 1            // Loadable segment

static constexpr int FB_WIDTH = 640;
static constexpr int FB_HEIGHT = 512;
static constexpr int DEFAULT_DISPLAY_HEIGHT = 448;
static constexpr uint32_t DEFAULT_FB_SIZE = FB_WIDTH * FB_HEIGHT * 4;
static constexpr uint32_t DEFAULT_FB_ADDR = (PS2_RAM_SIZE - DEFAULT_FB_SIZE - 0x10000u);
#if defined(PLATFORM_VITA)
static constexpr int HOST_WINDOW_WIDTH = 960;
static constexpr int HOST_WINDOW_HEIGHT = 544;
#else
static constexpr int HOST_WINDOW_WIDTH = FB_WIDTH;
static constexpr int HOST_WINDOW_HEIGHT = DEFAULT_DISPLAY_HEIGHT;
#endif
struct ElfHeader
{
    uint32_t magic;
    uint8_t elf_class;
    uint8_t endianness;
    uint8_t version;
    uint8_t os_abi;
    uint8_t abi_version;
    uint8_t padding[7];
    uint16_t type;
    uint16_t machine;
    uint32_t version2;
    uint32_t entry;
    uint32_t phoff;
    uint32_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct ProgramHeader
{
    uint32_t type;
    uint32_t offset;
    uint32_t vaddr;
    uint32_t paddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t flags;
    uint32_t align;
};

namespace
{
    constexpr uint32_t kGuestHeapDefaultBase = 0x00100000u;
    constexpr uint32_t kGuestHeapDefaultAlignment = 16u;
    constexpr uint32_t kGuestHeapSafetyPad = 0x1000u;
    constexpr uint32_t kGuestHeapHardLimit = 0x01800000u;
    // Keep emergency HLE allocations below the game's custom heap at
    // 0x01800000. Sharing that arena corrupts live Alchemy objects as the two
    // allocators advance independently.
    constexpr uint32_t kGuestBumpAllocatorBase = 0x00900000u;
    constexpr uint32_t kGuestBumpAllocatorLimit = 0x01800000u;
    std::mutex g_guestBumpAllocationMutex;
    std::unordered_map<uint32_t, uint32_t> g_guestBumpAllocationSizes;
    std::vector<std::pair<uint32_t, uint32_t>> g_guestBumpFreeBlocks;

    struct XmenHostInflateState
    {
        z_stream stream{};
        bool initialized = false;
    };

    std::mutex g_xmenHostInflateMutex;
    std::unordered_map<uint32_t, XmenHostInflateState> g_xmenHostInflateStates;

    constexpr uint32_t COP0_CAUSE_EXCCODE_MASK = 0x0000007Cu;
    constexpr uint32_t COP0_CAUSE_BD = 0x80000000u;
    constexpr uint32_t COP0_STATUS_EXL = 0x00000002u;
    constexpr uint32_t COP0_STATUS_BEV = 0x00400000u;
    constexpr uint32_t EXCEPTION_VECTOR_GENERAL = 0x80000080u;
    constexpr uint32_t EXCEPTION_VECTOR_TLB_REFILL = 0x80000000u;
    constexpr uint32_t EXCEPTION_VECTOR_BOOT = 0xBFC00200u;

    struct DispatchHistory
    {
        std::array<uint32_t, 64> pcs{};
        uint32_t next = 0u;
        bool wrapped = false;
    };

    thread_local DispatchHistory g_dispatchHistory;
    thread_local uint64_t g_guestDispatchYieldGeneration = 0u;

    bool isIpuHardwareAddress(uint32_t vaddr)
    {
        const uint32_t physical = vaddr & 0x1FFFFFFFu;
        return (physical >= 0x10002000u && physical <= 0x10002030u) ||
               (physical >= 0x10007000u && physical <= 0x10007030u) ||
               (physical >= 0x1000B000u && physical <= 0x1000B0FFu) ||
               (physical >= 0x1000B400u && physical <= 0x1000B4FFu);
    }

    bool shouldTraceIpuHardwareAccess(std::atomic<uint32_t> &counter, uint32_t &index)
    {
        index = counter.fetch_add(1u, std::memory_order_relaxed);
        return index < 256u || (index != 0u && (index & (index - 1u)) == 0u);
    }

    bool computeFileCrc32(const std::string &path, uint32_t &crcOut)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        static const std::array<uint32_t, 256> table = []
        {
            std::array<uint32_t, 256> values{};
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                uint32_t value = i;
                for (uint32_t bit = 0; bit < 8; ++bit)
                {
                    value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
                }
                values[i] = value;
            }
            return values;
        }();

        uint32_t crc = 0xFFFFFFFFu;
        std::array<uint8_t, 16 * 1024> buffer{};
        while (file.good())
        {
            file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                crc = table[(crc ^ buffer[static_cast<size_t>(i)]) & 0xFFu] ^ (crc >> 8u);
            }
        }
        if (file.bad())
        {
            return false;
        }
        crcOut = ~crc;
        return true;
    }

    void pushDispatchPc(uint32_t pc)
    {
        DispatchHistory &h = g_dispatchHistory;
        h.pcs[h.next] = pc;
        h.next = (h.next + 1u) % static_cast<uint32_t>(h.pcs.size());
        if (h.next == 0u)
        {
            h.wrapped = true;
        }
    }

    std::string formatDispatchHistory()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        if (count == 0u)
        {
            return "(empty)";
        }

        std::ostringstream oss;
        bool first = true;
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            if (!first)
            {
                oss << " -> ";
            }
            first = false;
            oss << "0x" << std::hex << h.pcs[idx];
        }
        return oss.str();
    }

    uint32_t readRdramProbeU32(const uint8_t *rdram, uint32_t addr)
    {
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            return 0u;
        }
        uint32_t value = 0u;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    uint16_t readRdramProbeU16(const uint8_t *rdram, uint32_t addr)
    {
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint16_t))
        {
            return 0u;
        }
        uint16_t value = 0u;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    float readRdramProbeF32(const uint8_t *rdram, uint32_t addr)
    {
        const uint32_t bits = readRdramProbeU32(rdram, addr);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    uint64_t readRdramProbeU64(const uint8_t *rdram, uint32_t addr)
    {
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint64_t))
        {
            return 0u;
        }
        uint64_t value = 0u;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    void writeRdramProbeU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (rdram && addr <= PS2_RAM_SIZE - sizeof(value))
        {
            std::memcpy(rdram + addr, &value, sizeof(value));
        }
    }

    void writeRdramProbeU64(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (rdram && addr <= PS2_RAM_SIZE - sizeof(value))
        {
            std::memcpy(rdram + addr, &value, sizeof(value));
        }
    }

    void restoreXmenTitleVtable(uint8_t *rdram)
    {
        static constexpr uint32_t kVtable = 0x006F2B50u;
        static constexpr uint32_t kWords[] = {
            0x0068EF50u, 0x00000000u, 0x0014AE50u, 0x0014B240u,
            0x0014B600u, 0x0014B610u, 0x0014BE90u, 0x0014BFA0u,
            0x0014BFB0u, 0x0014AD80u, 0x0014AD90u, 0x00000000u,
        };
        if (!rdram)
        {
            return;
        }
        for (uint32_t i = 0u; i < (sizeof(kWords) / sizeof(kWords[0])); ++i)
        {
            std::memcpy(rdram + kVtable + (i * sizeof(uint32_t)), &kWords[i], sizeof(uint32_t));
        }
    }

    void logXmenVtableTripwire(uint8_t *rdram,
                               const R5900Context *ctx,
                               const char *phase,
                               uint32_t sourcePc,
                               uint32_t targetPc,
                               PS2Runtime::GuestBranchKind kind)
    {
        constexpr uint32_t kBootstrapAllocator = 0x00898EF0u;
        constexpr uint32_t kBootReceiver = 0x018523E0u;
        constexpr uint32_t kBootReceiverVtable = 0x006FE910u;
        constexpr uint32_t kModeSingletonReceiver = 0x0072091Cu;
        constexpr uint32_t kVtable = 0x006F2B50u;
        const uint32_t receiverWord0 = readRdramProbeU32(rdram, kBootReceiver);
        const uint32_t singletonReceiver = readRdramProbeU32(rdram, kModeSingletonReceiver);
        const uint32_t slot08 = readRdramProbeU32(rdram, kVtable + 0x08u);
        const uint32_t slot0c = readRdramProbeU32(rdram, kVtable + 0x0cu);
        const uint32_t slot10 = readRdramProbeU32(rdram, kVtable + 0x10u);
        const uint32_t slot14 = readRdramProbeU32(rdram, kVtable + 0x14u);
        static thread_local bool s_seenGood = false;
        static thread_local bool s_reportedBad = false;
        static thread_local bool s_seenBootReceiver = false;
        static thread_local bool s_reportedBootReceiverBad = false;
        static thread_local uint32_t s_lastSingletonReceiver = 0u;
        static thread_local uint32_t s_lastBootstrapAllocatorVtable = 0u;
        const uint32_t bootstrapAllocatorVtable = readRdramProbeU32(rdram, kBootstrapAllocator);
        if (bootstrapAllocatorVtable != s_lastBootstrapAllocatorVtable)
        {
            std::cerr << "[xmen-bootstrap-allocator-vtable-change] phase=" << (phase ? phase : "")
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=0x" << static_cast<uint32_t>(kind)
                      << " old=0x" << s_lastBootstrapAllocatorVtable
                      << " new=0x" << bootstrapAllocatorVtable
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << (ctx ? GPR_U32(ctx, 31) : 0u)
                      << " sp=0x" << (ctx ? GPR_U32(ctx, 29) : 0u)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
            s_lastBootstrapAllocatorVtable = bootstrapAllocatorVtable;
        }
        if (singletonReceiver != s_lastSingletonReceiver)
        {
            std::cerr << "[xmen-mode-receiver-change] phase=" << (phase ? phase : "")
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=0x" << static_cast<uint32_t>(kind)
                      << " old=0x" << s_lastSingletonReceiver
                      << " new=0x" << singletonReceiver
                      << " newWord0=0x" << readRdramProbeU32(rdram, singletonReceiver)
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << (ctx ? GPR_U32(ctx, 31) : 0u)
                      << " sp=0x" << (ctx ? GPR_U32(ctx, 29) : 0u)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
            s_lastSingletonReceiver = singletonReceiver;
        }
        if (receiverWord0 == kBootReceiverVtable)
        {
            s_seenBootReceiver = true;
        }
        if (!s_reportedBootReceiverBad && s_seenBootReceiver && receiverWord0 != kBootReceiverVtable)
        {
            s_reportedBootReceiverBad = true;
            std::cerr << "[xmen-boot-receiver-tripwire] phase=" << (phase ? phase : "")
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=0x" << static_cast<uint32_t>(kind)
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << (ctx ? GPR_U32(ctx, 31) : 0u)
                      << " sp=0x" << (ctx ? GPR_U32(ctx, 29) : 0u)
                      << " object=0x" << kBootReceiver
                      << " word0=0x" << receiverWord0
                      << " word4=0x" << readRdramProbeU32(rdram, kBootReceiver + 4u)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
        if (slot14 == 0x0014B610u)
        {
            s_seenGood = true;
        }
        if (!s_reportedBad && s_seenGood && slot14 == 0u)
        {
            s_reportedBad = true;
            std::cerr << "[xmen-vtable-tripwire] phase=" << (phase ? phase : "")
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=0x" << static_cast<uint32_t>(kind)
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << (ctx ? GPR_U32(ctx, 31) : 0u)
                      << " sp=0x" << (ctx ? GPR_U32(ctx, 29) : 0u)
                      << " slot08=0x" << slot08
                      << " slot0c=0x" << slot0c
                      << " slot10=0x" << slot10
                      << " slot14=0x" << slot14
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
            restoreXmenTitleVtable(rdram);
            std::cerr << "[xmen-vtable-tripwire:restored] phase=" << (phase ? phase : "")
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " slot14=0x" << readRdramProbeU32(rdram, kVtable + 0x14u)
                      << std::dec << std::endl;
        }
    }

    void dumpDispatchHistoryToStderr()
    {
        const DispatchHistory &h = g_dispatchHistory;
        const uint32_t count = h.wrapped ? static_cast<uint32_t>(h.pcs.size()) : h.next;
        std::fprintf(stderr, "[seh] dispatch_trace=");
        if (count == 0u)
        {
            std::fprintf(stderr, "(empty)");
        }
        for (uint32_t i = 0u; i < count; ++i)
        {
            const uint32_t idx = (h.next + h.pcs.size() - count + i) % static_cast<uint32_t>(h.pcs.size());
            std::fprintf(stderr, "%s0x%08x", i == 0u ? "" : " -> ", h.pcs[idx]);
        }
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
    }

    uint32_t selectExceptionVector(const R5900Context *ctx, bool tlbRefill)
    {
        if (ctx->cop0_status & COP0_STATUS_BEV)
        {
            return EXCEPTION_VECTOR_BOOT;
        }
        return tlbRefill ? EXCEPTION_VECTOR_TLB_REFILL : EXCEPTION_VECTOR_GENERAL;
    }

    void seedVu0IdleSuccess(R5900Context *ctx)
    {
        if (!ctx)
        {
            return;
        }

        ctx->vu0_clip_flags = 0;
        ctx->vu0_clip_flags2 = 0;
        ctx->vu0_mac_flags = 0;
        ctx->vu0_status = 0;
        ctx->vu0_q = 1.0f;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));
        ctx->vu0_vpu_stat = 0;
        ctx->vu0_vpu_stat2 = 0;
    }

    void copyVu0ContextToState(const R5900Context *ctx, VU1State &state)
    {
        std::memset(&state, 0, sizeof(state));

        for (uint32_t i = 0; i < 32u; ++i)
        {
            _mm_storeu_ps(state.vf[i], ctx->vu0_vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            state.vi[i] = static_cast<int16_t>(ctx->vi[i]);
        }

        _mm_storeu_ps(state.acc, ctx->vu0_acc);
        state.q = ctx->vu0_q;
        state.p = ctx->vu0_p;
        state.i = ctx->vu0_i;
        alignas(16) uint32_t rWords[4]{};
        _mm_storeu_si128(reinterpret_cast<__m128i *>(rWords), _mm_castps_si128(ctx->vu0_r));
        state.r = 0x3F800000u | (rWords[0] & 0x007FFFFFu);
        state.pc = ctx->vu0_pc;
        state.mac = ctx->vu0_mac_flags;
        state.clip = ctx->vu0_clip_flags;
        state.status = ctx->vu0_status;
        state.itop = ctx->vu0_itop;
        state.dBitEnabled = (ctx->vu0_fbrst & (1u << 2)) != 0u;
        state.tBitEnabled = (ctx->vu0_fbrst & (1u << 3)) != 0u;

        state.vf[0][0] = 0.0f;
        state.vf[0][1] = 0.0f;
        state.vf[0][2] = 0.0f;
        state.vf[0][3] = 1.0f;
        state.vi[0] = 0;
    }

    void copyVu0StateToContext(const VU1State &state, R5900Context *ctx)
    {
        for (uint32_t i = 0; i < 32u; ++i)
        {
            ctx->vu0_vf[i] = _mm_loadu_ps(state.vf[i]);
        }
        for (uint32_t i = 0; i < 16u; ++i)
        {
            ctx->vi[i] = static_cast<uint16_t>(state.vi[i]);
        }

        ctx->vu0_acc = _mm_loadu_ps(state.acc);
        ctx->vu0_q = state.q;
        ctx->vu0_p = state.p;
        ctx->vu0_i = state.i;
        ctx->vu0_r = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int32_t>(state.r)));
        ctx->vu0_mac_flags = state.mac;
        ctx->vu0_clip_flags = state.clip;
        ctx->vu0_clip_flags2 = state.clip;
        ctx->vu0_status = static_cast<uint16_t>(state.status);
        ctx->vu0_itop = state.itop;
        ctx->vu0_pc = state.pc;
        ctx->vu0_tpc = state.pc;
        ctx->vu0_vpu_stat = (ctx->vu0_vpu_stat & 0xFF00u) | (state.stoppedByD ? (1u << 1) : 0u) | (state.stoppedByT ? (1u << 2) : 0u);
        ctx->vu0_vpu_stat2 = 0;

        ctx->vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        ctx->vi[0] = 0;
    }

    void raiseCop0Exception(R5900Context *ctx, uint32_t exceptionCode, bool tlbRefill = false)
    {
        if (ctx->in_delay_slot)
        {
            ctx->cop0_epc = ctx->branch_pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~COP0_CAUSE_EXCCODE_MASK) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK) |
                              COP0_CAUSE_BD;
        }
        else
        {
            ctx->cop0_epc = ctx->pc;
            ctx->cop0_cause = (ctx->cop0_cause & ~(COP0_CAUSE_EXCCODE_MASK | COP0_CAUSE_BD)) |
                              ((exceptionCode << 2) & COP0_CAUSE_EXCCODE_MASK);
        }

        ctx->cop0_status |= COP0_STATUS_EXL;
        ctx->pc = selectExceptionVector(ctx, tlbRefill);
        ctx->in_delay_slot = false;
    }

    std::filesystem::path normalizeAbsolutePath(const std::filesystem::path &path)
    {
        if (path.empty())
        {
            return {};
        }

#if defined(PLATFORM_VITA)
        const std::string generic = path.generic_string();
        const std::size_t colon = generic.find(':');
        if (colon != std::string::npos && colon != 0u)
        {
            const std::size_t slash = generic.find_first_of("/\\");
            if (slash == std::string::npos || colon < slash)
            {
                return path.lexically_normal();
            }
        }
#endif

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return path.lexically_normal();
        }
        return absolute.lexically_normal();
    }

    PS2Runtime::IoPaths &runtimeIoPaths()
    {
        static PS2Runtime::IoPaths paths = []()
        {
            PS2Runtime::IoPaths defaults;
            std::error_code ec;
            const std::filesystem::path cwd = std::filesystem::current_path(ec);
            defaults.elfDirectory = ec ? std::filesystem::path(".") : cwd.lexically_normal();
            defaults.hostRoot = defaults.elfDirectory;
            defaults.cdRoot = defaults.elfDirectory;
            defaults.mcRoot = defaults.elfDirectory / "mc0";
            return defaults;
        }();

        return paths;
    }

    std::string readGuestPrintableString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string out;
        if (!rdram || maxLen == 0)
        {
            return out;
        }

        out.reserve(std::min<size_t>(maxLen, 64));
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char ch = static_cast<char>(rdram[(addr + static_cast<uint32_t>(i)) & PS2_RAM_MASK]);
            if (ch == '\0')
            {
                break;
            }
            if (ch >= 0x20 && ch < 0x7F)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('.');
            }
        }
        return out;
    }

    uint32_t readGuestWord(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        for (uint32_t i = 0; i < sizeof(value); ++i)
        {
            value |= static_cast<uint32_t>(rdram[(addr + i) & PS2_RAM_MASK]) << (i * 8u);
        }
        return value;
    }
}

extern "C" void ps2xDumpDispatchHistoryToStderr()
{
    dumpDispatchHistoryToStderr();
}

extern "C" uint32_t ps2xGuestBumpAlloc(uint8_t *rdram, uint32_t size, uint32_t alignment)
{
    static std::atomic<uint32_t> s_bump{kGuestBumpAllocatorBase};

    if (!rdram || size == 0u)
    {
        return 0u;
    }

    alignment = std::max<uint32_t>(alignment, 4u);
    const uint32_t mask = alignment - 1u;
    const uint32_t paddedSize = (size + 0xfu) & ~0xfu;

    const auto traceXmenOverlapAllocation = [&](uint32_t result, const char *route)
    {
        const uint32_t end = result + paddedSize;
        const bool touchesMenuAllocation =
            result < 0x0094AC00u && end > 0x0094AA00u;
        const bool touchesPreviousProbe =
            result < 0x00B70100u && end > 0x00B6FF00u;
        if (touchesMenuAllocation || touchesPreviousProbe)
        {
            std::cerr << "[xmen-overlap-allocation] route=" << route
                      << " size=0x" << std::hex << size
                      << " padded=0x" << paddedSize
                      << " alignment=0x" << alignment
                      << " result=0x" << result
                      << " end=0x" << end
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    };

    {
        std::lock_guard<std::mutex> lock(g_guestBumpAllocationMutex);
        for (size_t i = 0; i < g_guestBumpFreeBlocks.size(); ++i)
        {
            const auto [blockAddress, blockSize] = g_guestBumpFreeBlocks[i];
            const uint32_t aligned = (blockAddress + mask) & ~mask;
            const uint32_t prefix = aligned - blockAddress;
            if (prefix > blockSize || paddedSize > blockSize - prefix)
            {
                continue;
            }

            const uint32_t suffixAddress = aligned + paddedSize;
            const uint32_t suffixSize = blockSize - prefix - paddedSize;
            g_guestBumpFreeBlocks.erase(g_guestBumpFreeBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            if (prefix != 0u) g_guestBumpFreeBlocks.emplace_back(blockAddress, prefix);
            if (suffixSize != 0u) g_guestBumpFreeBlocks.emplace_back(suffixAddress, suffixSize);
            g_guestBumpAllocationSizes[aligned] = size;
            std::memset(rdram + aligned, 0, paddedSize);
            traceXmenOverlapAllocation(aligned, "free-list");
            return aligned;
        }
    }

    uint32_t current = s_bump.load(std::memory_order_relaxed);
    for (;;)
    {
        const uint32_t aligned = (current + mask) & ~mask;
        const uint32_t next = aligned + paddedSize;
        if (next >= kGuestBumpAllocatorLimit || next < aligned)
        {
            return 0u;
        }

        if (s_bump.compare_exchange_weak(current, next, std::memory_order_acq_rel, std::memory_order_relaxed))
        {
            std::memset(rdram + aligned, 0, paddedSize);
            {
                std::lock_guard<std::mutex> lock(g_guestBumpAllocationMutex);
                g_guestBumpAllocationSizes[aligned] = size;
            }
            traceXmenOverlapAllocation(aligned, "bump");
            return aligned;
        }
    }
}

extern "C" uint32_t ps2xGuestBumpAllocationSize(uint32_t address)
{
    std::lock_guard<std::mutex> lock(g_guestBumpAllocationMutex);
    const auto it = g_guestBumpAllocationSizes.find(address);
    return it != g_guestBumpAllocationSizes.end() ? it->second : 0u;
}

extern "C" bool ps2xGuestBumpFree(uint32_t address)
{
    std::lock_guard<std::mutex> lock(g_guestBumpAllocationMutex);
    const auto allocation = g_guestBumpAllocationSizes.find(address);
    if (allocation == g_guestBumpAllocationSizes.end())
    {
        return false;
    }

    const uint32_t paddedSize = (allocation->second + 0xfu) & ~0xfu;
    g_guestBumpAllocationSizes.erase(allocation);
    g_guestBumpFreeBlocks.emplace_back(address, paddedSize);
    std::sort(g_guestBumpFreeBlocks.begin(), g_guestBumpFreeBlocks.end());

    size_t write = 0u;
    for (const auto &[blockAddress, blockSize] : g_guestBumpFreeBlocks)
    {
        if (write != 0u) {
            auto &previous = g_guestBumpFreeBlocks[write - 1u];
            if (previous.first + previous.second == blockAddress) {
                previous.second += blockSize;
                continue;
            }
        }
        g_guestBumpFreeBlocks[write++] = {blockAddress, blockSize};
    }
    g_guestBumpFreeBlocks.resize(write);
    return true;
}

static void UploadFrame(Texture2D &tex, PS2Runtime *rt, uint32_t &outWidth, uint32_t &outHeight)
{
    static uint64_t s_lastPresentationTick = std::numeric_limits<uint64_t>::max();
    static bool s_hasLatchedInitialFrame = false;
    static uint32_t s_lastDisplayFbp = std::numeric_limits<uint32_t>::max();
    static uint32_t s_lastSourceFbp = std::numeric_limits<uint32_t>::max();
    static bool s_lastPreferred = false;
    static uint32_t s_lastWidth = 0u;
    static uint32_t s_lastHeight = 0u;
    static bool s_hasUploadedFrame = false;
    static std::vector<uint8_t> s_scratch;
    static std::vector<uint8_t> s_uploadBuffer(DEFAULT_FB_SIZE, 0u);

    const uint64_t currentTick = rt->eeScheduler().currentVSyncTick();
    const bool needsLatch = !s_hasLatchedInitialFrame || currentTick != s_lastPresentationTick;
    if (needsLatch)
    {
        rt->gs().latchHostPresentationFrame();
        s_lastPresentationTick = currentTick;
        s_hasLatchedInitialFrame = true;
    }
    else if (s_hasUploadedFrame)
    {
        outWidth = (s_lastWidth != 0u) ? s_lastWidth : FB_WIDTH;
        outHeight = (s_lastHeight != 0u) ? s_lastHeight : DEFAULT_DISPLAY_HEIGHT;
        return;
    }

    s_scratch.clear();
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t displayFbp = 0u;
    uint32_t sourceFbp = 0u;
    bool usedPreferredDisplaySource = false;
    if (!rt->gs().copyLatchedHostPresentationFrame(s_scratch,
                                                   width,
                                                   height,
                                                   &displayFbp,
                                                   &sourceFbp,
                                                   &usedPreferredDisplaySource))
    {
        Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, MAGENTA);
        UpdateTexture(tex, blank.data);
        UnloadImage(blank);
        outWidth = FB_WIDTH;
        outHeight = DEFAULT_DISPLAY_HEIGHT;
        s_lastWidth = outWidth;
        s_lastHeight = outHeight;
        s_hasUploadedFrame = true;
        return;
    }

    PS2_IF_AGRESSIVE_LOGS({
        static uint32_t s_uploadDebugCount = 0u;
        if (s_uploadDebugCount < 128u ||
            displayFbp != s_lastDisplayFbp ||
            sourceFbp != s_lastSourceFbp ||
            usedPreferredDisplaySource != s_lastPreferred ||
            width != s_lastWidth ||
            height != s_lastHeight)
        {
            std::cout << "[frame:upload] idx=" << s_uploadDebugCount
                      << " tick=" << currentTick
                      << " displayFbp=" << displayFbp
                      << " sourceFbp=" << sourceFbp
                      << " size=" << width << "x" << height
                      << " preferred=" << static_cast<uint32_t>(usedPreferredDisplaySource ? 1u : 0u)
                      << std::endl;
        }
        ++s_uploadDebugCount;
    });
    s_lastDisplayFbp = displayFbp;
    s_lastSourceFbp = sourceFbp;
    s_lastPreferred = usedPreferredDisplaySource;
    s_lastWidth = width;
    s_lastHeight = height;

    std::fill(s_uploadBuffer.begin(), s_uploadBuffer.end(), 0u);
    if (!s_scratch.empty() && width != 0u && height != 0u)
    {
        const uint32_t copyWidth = std::min<uint32_t>(width, FB_WIDTH);
        const uint32_t copyHeight = std::min<uint32_t>(height, FB_HEIGHT);
        const size_t srcRowBytes = static_cast<size_t>(width) * 4u;
        const size_t dstRowBytes = static_cast<size_t>(FB_WIDTH) * 4u;
        const size_t copyRowBytes = static_cast<size_t>(copyWidth) * 4u;
        for (uint32_t y = 0; y < copyHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * srcRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * dstRowBytes;
            if (srcOffset + copyRowBytes > s_scratch.size() ||
                dstOffset + copyRowBytes > s_uploadBuffer.size())
            {
                break;
            }
            std::memcpy(s_uploadBuffer.data() + dstOffset, s_scratch.data() + srcOffset, copyRowBytes);
        }
    }

    UpdateTexture(tex, s_uploadBuffer.data());
    outWidth = width;
    outHeight = height;
    s_hasUploadedFrame = true;
}

PS2Runtime::PS2Runtime()
{
    m_iopHost = std::make_unique<PS2IopHostAdapter>(*this);
    m_iopSubsystem = std::make_unique<ps2x::iop::IopSubsystem>(*m_iopHost);
    m_eeScheduler = std::make_unique<EeScheduler>(*this);
    m_memory.setGsInterruptCallback([this](uint32_t)
                                    {
                                        if (m_eeScheduler)
                                            m_eeScheduler->postEvent(EeEvent{EeEventType::Intc, 0u, 0u});
                                    });
#if defined(PS2X_IOP_ENABLE_PLUGINS) && PS2X_IOP_ENABLE_PLUGINS && \
    !defined(PLATFORM_VITA) && (defined(_WIN32) || defined(__linux__))
    if (const char *applicationDirectory = GetApplicationDirectory();
        applicationDirectory && applicationDirectory[0] != '\0')
    {
        m_iopSubsystem->setPluginSearchPaths({std::filesystem::path(applicationDirectory) / "iop_plugins"});
    }
#endif

    // Assign a default-constructed context rather than memset-ing this one.
    // R5900Context's constructor already zeroes itself and then applies the
    // architectural reset values on top -- COP0 Status, PRId, Random. A raw
    // memset here silently threw those away, leaving Status at 0 for the main
    // thread while every thread created later, which goes through
    // `target->context = R5900Context{}` in EeScheduler::startThread, got the
    // correct values. Guest code reads Status.IE to decide whether interrupts
    // are enabled, so the main thread believed they were permanently off.
    m_cpuContext = R5900Context{};

    // R0 is always zero in MIPS
    m_cpuContext.r[0] = _mm_set1_epi32(0);
    m_cpuContext.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
    m_cpuContext.vu0_q = 1.0f;
    m_cpuContext.vu0_r = _mm_castsi128_ps(_mm_set1_epi32(0x3F800000));

    // Stack pointer (SP) and global pointer (GP) will be set by the loaded ELF

    m_loadedModules.clear();
    m_guestHeapBlocks.clear();
    m_guestHeapBase = kGuestHeapDefaultBase;
    m_guestHeapEnd = kGuestHeapDefaultBase;
    m_guestHeapLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_guestHeapSuggestedBase = kGuestHeapDefaultBase;
    m_guestHeapConfigured = false;
    m_asyncCallbackStackFloor = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    m_asyncCallbackStackTop = PS2_RAM_SIZE;
}

void PS2Runtime::setDebugUiCallbacks(DebugUiCallback initCallback,
                                     DebugUiCallback drawCallback,
                                     DebugUiCallback shutdownCallback,
                                     void *userData)
{
    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }

    m_debugUiInitCallback = initCallback;
    m_debugUiDrawCallback = drawCallback;
    m_debugUiShutdownCallback = shutdownCallback;
    m_debugUiUserData = userData;
}

PS2Runtime::~PS2Runtime()
{
    try
    {
        requestStop();
        m_iopSubsystem.reset();
        m_iopHost.reset();
#if defined(PLATFORM_VITA)
        m_audioBackend.stopAll();
        m_audioBackend.setAudioReady(false);
#else
        if (IsAudioDeviceReady())
        {
            m_audioBackend.stopAll();
            m_audioBackend.setAudioReady(false);
            CloseAudioDevice();
        }
#endif
        if (m_debugUiInitialized && m_debugUiShutdownCallback)
        {
            m_debugUiShutdownCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = false;
        }

        if (IsWindowReady())
        {
            CloseWindow();
        }

        m_loadedModules.clear();
    }
    catch (const std::exception &e)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[~PS2Runtime] cleanup exception: unknown" << std::endl;
    }
}

void PS2Runtime::setIopPluginSearchPaths(std::vector<std::filesystem::path> paths)
{
    m_iopSubsystem->setPluginSearchPaths(std::move(paths));
}

ps2x::iop::RpcAbi PS2Runtime::selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const
{
    return m_iopSubsystem->selectRpcAbi(request);
}

ps2x::iop::RpcResult PS2Runtime::handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request)
{
    auto scope = m_iopHost->enterCall(ctx, rdram);
    request.callToken = scope.token();
    return m_iopSubsystem->handleRpc(request);
}

void PS2Runtime::notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer)
{
    auto scope = m_iopHost->enterCall(nullptr, rdram);
    m_iopSubsystem->onSifTransfer(transfer);
}

void PS2Runtime::resetIop()
{
    m_iopSubsystem->reset();
}

ps2x::iop::DebugSnapshot PS2Runtime::iopDebugSnapshot() const
{
    return m_iopSubsystem->debugSnapshot();
}

bool PS2Runtime::syncCoreSubsystems()
{
    uint8_t *const rdram = m_memory.getRDRAM();
    uint8_t *const gsVram = m_memory.getGSVRAM();
    if (!rdram || !gsVram)
    {
        return false;
    }

    if (m_boundRdram == rdram && m_boundGSVram == gsVram)
    {
        return true;
    }

    m_gs.init(gsVram, static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &m_memory.gs());
    m_gs.setInterruptCallback([this](uint32_t cause)
                              {
                                  if (m_eeScheduler)
                                  {
                                      const uint64_t csr = m_memory.gs().csr.load(std::memory_order_relaxed);
                                      if ((csr & 0x2u) != 0u)
                                      {
                                          static std::atomic<uint32_t> finishPostTraceCount{0u};
                                          const uint32_t index = finishPostTraceCount.fetch_add(1u, std::memory_order_relaxed);
                                          std::fprintf(stderr,
                                                       "[xmen-gs-finish-post] index=%u cause=%u csr=0x%llx imr=0x%llx\n",
                                                       index,
                                                       cause,
                                                       static_cast<unsigned long long>(csr),
                                                       static_cast<unsigned long long>(m_memory.gs().imr));
                                      }
                                      m_eeScheduler->postEvent(EeEvent{EeEventType::Intc, cause, 0u});
                                  }
                              });
    m_gifArbiter.setProcessPacketFn([this](const uint8_t *data, uint32_t size)
                                    { m_gs.processGIFPacket(data, size); });
    m_memory.setGifArbiter(&m_gifArbiter);
    m_memory.setVu1ServiceCallback([this](bool drain)
                                   {
                                       if (!m_vu1.isRunning())
                                           return false;

                                       const uint32_t traceSlice =
                                           g_xmenVu1MailboxTraceSlice.load(std::memory_order_relaxed);
                                       const bool traceMailbox =
                                           g_xmenVu1MailboxTraceArmed.load(std::memory_order_acquire) &&
                                           traceSlice < 12u;
                                       if (traceMailbox)
                                           traceXmenVu1Mailbox("resume-before", traceSlice, m_vu1, m_memory.getVU1Data());

                                       R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                       if (!cpuContext)
                                           cpuContext = &m_cpuContext;

                                       m_vu1.state().dBitEnabled =
                                           (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                       m_vu1.state().tBitEnabled =
                                           (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                       m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                    m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                    m_gs, &m_memory,
                                                    m_vu1.state().top, m_vu1.state().itop,
                                                    drain ? (1u << 20) : 64u);
                                       if (traceMailbox)
                                       {
                                           traceXmenVu1Mailbox("resume-after", traceSlice, m_vu1, m_memory.getVU1Data());
                                           g_xmenVu1MailboxTraceSlice.fetch_add(1u, std::memory_order_relaxed);
                                       }
                                       if (!m_vu1.isRunning())
                                           g_xmenVu1MailboxTraceArmed.store(false, std::memory_order_release);
                                       cpuContext->vu0_vpu_stat =
                                           (cpuContext->vu0_vpu_stat & ~0x0700u) |
                                           (m_vu1.isRunning() ? 0x0100u : 0u) |
                                           (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                           (m_vu1.state().stoppedByT ? 0x0400u : 0u);
                                       return m_vu1.isRunning();
                                   });
    m_memory.setVu1MscalCallback([this](uint32_t startPC, uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     const bool traceMailbox =
                                         startPC == 0x80u && top == 0xA8u && itop == 0x11Au;
                                     if (traceMailbox)
                                     {
                                         g_xmenVu1MailboxTraceSlice.store(0u, std::memory_order_relaxed);
                                         g_xmenVu1MailboxTraceArmed.store(true, std::memory_order_release);
                                         traceXmenVu1Mailbox("execute-before", 0u, m_vu1, m_memory.getVU1Data());
                                         traceXmenVu1MailboxCode(m_memory.getVU1Code());
                                     }
                                     m_vu1.execute(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                   m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                   m_gs, &m_memory, startPC, top, itop, 64u);
                                     if (traceMailbox)
                                     {
                                         traceXmenVu1Mailbox("execute-after", 0u, m_vu1, m_memory.getVU1Data());
                                         if (!m_vu1.isRunning())
                                             g_xmenVu1MailboxTraceArmed.store(false, std::memory_order_release);
                                     }
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0700u) |
                                         (m_vu1.isRunning() ? 0x0100u : 0u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    m_memory.setVu1MscntCallback([this](uint32_t top, uint32_t itop)
                                 {
                                     R5900Context *cpuContext = m_eeScheduler ? m_eeScheduler->currentContext() : nullptr;
                                     if (!cpuContext)
                                     {
                                         cpuContext = &m_cpuContext;
                                     }
                                     m_vu1.state().dBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 10)) != 0u;
                                     m_vu1.state().tBitEnabled =
                                         (cpuContext->vu0_fbrst & (1u << 11)) != 0u;
                                     m_vu1.resume(m_memory.getVU1Code(), PS2_VU1_CODE_SIZE,
                                                  m_memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                                                  m_gs, &m_memory, top, itop, 1u << 20);
                                     cpuContext->vu0_vpu_stat =
                                         (cpuContext->vu0_vpu_stat & ~0x0700u) |
                                         (m_vu1.isRunning() ? 0x0100u : 0u) |
                                         (m_vu1.state().stoppedByD ? 0x0200u : 0u) |
                                         (m_vu1.state().stoppedByT ? 0x0400u : 0u); });
    resetIop();
    m_vu0.reset();
    m_vu1.reset();

    m_boundRdram = rdram;
    m_boundGSVram = gsVram;
    return true;
}

bool PS2Runtime::initialize(const char *title)
{
    try
    {
        if (!m_memory.initialize())
        {
            std::cerr << "Failed to initialize PS2 memory" << std::endl;
            return false;
        }

        if (!syncCoreSubsystems())
        {
            std::cerr << "Failed to bind runtime core subsystems" << std::endl;
            return false;
        }
#if defined(PS2X_IOP_ENABLE_PLUGINS) && PS2X_IOP_ENABLE_PLUGINS && \
    !defined(PLATFORM_VITA) && (defined(_WIN32) || defined(__linux__))
        std::string pluginError;
        if (!m_iopSubsystem->loadPlugins(&pluginError))
        {
            std::cerr << "Failed to load IOP plugins: " << pluginError << std::endl;
            return false;
        }
#endif
#if defined(PLATFORM_VITA)
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title); // raylib vita does not support audio
#else
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
        InitWindow(HOST_WINDOW_WIDTH, HOST_WINDOW_HEIGHT, title);
        InitAudioDevice();
        m_audioBackend.setAudioReady(IsAudioDeviceReady());
#endif
        SetTargetFPS(60);
        if (m_debugUiInitCallback)
        {
            m_debugUiInitCallback(*this, m_debugUiUserData);
            m_debugUiInitialized = true;
        }

        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Failed to initialize PS2 runtime: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "Failed to initialize PS2 runtime: unknown exception" << std::endl;
    }

    return false;
}

bool PS2Runtime::loadELF(const std::string &elfPath)
{
    configureIoPathsFromElf(elfPath);

    std::ifstream file(elfPath, std::ios::binary);
    if (!file)
    {
        std::cerr << "Failed to open ELF file: " << elfPath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff fileSize = file.tellg();
    if (fileSize < static_cast<std::streamoff>(sizeof(ElfHeader)))
    {
        std::cerr << "ELF file is too small: " << elfPath << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    ElfHeader header{};
    if (!file.read(reinterpret_cast<char *>(&header), sizeof(header)))
    {
        std::cerr << "Failed to read ELF header from: " << elfPath << std::endl;
        return false;
    }

    if (header.magic != ELF_MAGIC)
    {
        std::cerr << "Invalid ELF magic number" << std::endl;
        return false;
    }

    if (header.elf_class != 1u || header.endianness != 1u)
    {
        std::cerr << "Unsupported ELF format (expected 32-bit little-endian)." << std::endl;
        return false;
    }

    if (header.machine != EM_MIPS || header.type != ET_EXEC)
    {
        std::cerr << "Not a MIPS executable ELF file" << std::endl;
        return false;
    }

    if (header.phnum != 0u && header.phentsize < sizeof(ProgramHeader))
    {
        std::cerr << "Unsupported ELF program-header entry size: " << header.phentsize << std::endl;
        return false;
    }

    const uint64_t programHeaderTableEnd =
        static_cast<uint64_t>(header.phoff) +
        static_cast<uint64_t>(header.phnum) * static_cast<uint64_t>(header.phentsize);
    if (programHeaderTableEnd > static_cast<uint64_t>(fileSize))
    {
        std::cerr << "ELF program-header table is out of range." << std::endl;
        return false;
    }

    m_cpuContext.pc = header.entry;
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);

    uint32_t maxLoadedRdramEnd = kGuestHeapDefaultBase;
    uint32_t moduleBase = std::numeric_limits<uint32_t>::max();
    uint32_t moduleEnd = 0u;
    bool loadedAnySegment = false;

    for (uint16_t i = 0; i < header.phnum; i++)
    {
        const uint64_t phOffset =
            static_cast<uint64_t>(header.phoff) +
            static_cast<uint64_t>(i) * static_cast<uint64_t>(header.phentsize);
        if (phOffset + sizeof(ProgramHeader) > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF program header " << i << " is out of range." << std::endl;
            return false;
        }

        ProgramHeader ph{};
        file.seekg(static_cast<std::streamoff>(phOffset), std::ios::beg);
        if (!file.read(reinterpret_cast<char *>(&ph), sizeof(ph)))
        {
            std::cerr << "Failed to read ELF program header " << i << std::endl;
            return false;
        }

        if (ph.type != PT_LOAD || ph.memsz == 0u)
        {
            continue;
        }

        if (ph.filesz > ph.memsz)
        {
            std::cerr << "ELF segment " << i << " has filesz > memsz." << std::endl;
            return false;
        }

        const uint64_t segmentFileEnd = static_cast<uint64_t>(ph.offset) + static_cast<uint64_t>(ph.filesz);
        if (segmentFileEnd > static_cast<uint64_t>(fileSize))
        {
            std::cerr << "ELF segment " << i << " exceeds file bounds." << std::endl;
            return false;
        }

        const bool scratch =
            ph.vaddr >= PS2_SCRATCHPAD_BASE &&
            ph.vaddr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);

        uint32_t physAddr = 0u;
        try
        {
            physAddr = m_memory.translateAddress(ph.vaddr);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Failed to translate ELF segment " << i
                      << " virtual address 0x" << std::hex << ph.vaddr
                      << std::dec << ": " << e.what() << std::endl;
            return false;
        }
        const uint64_t regionSize = scratch ? static_cast<uint64_t>(PS2_SCRATCHPAD_SIZE)
                                            : static_cast<uint64_t>(PS2_RAM_SIZE);
        const uint64_t segmentMemEnd = static_cast<uint64_t>(physAddr) + static_cast<uint64_t>(ph.memsz);
        if (segmentMemEnd > regionSize)
        {
            std::cerr << "ELF segment " << i << " exceeds "
                      << (scratch ? "scratchpad" : "RDRAM")
                      << " bounds (vaddr=0x" << std::hex << ph.vaddr
                      << " memsz=0x" << ph.memsz << std::dec << ")." << std::endl;
            return false;
        }

        uint8_t *destBase = scratch ? m_memory.getScratchpad() : m_memory.getRDRAM();
        if (!destBase)
        {
            std::cerr << "ELF segment " << i << " has no destination memory backing." << std::endl;
            return false;
        }

        uint8_t *dest = destBase + physAddr;
        if (ph.filesz > 0u)
        {
            file.seekg(static_cast<std::streamoff>(ph.offset), std::ios::beg);
            if (!file.read(reinterpret_cast<char *>(dest), ph.filesz))
            {
                std::cerr << "Failed to read ELF segment " << i << " payload." << std::endl;
                return false;
            }
            constexpr uint32_t kXmenProbeVtable = 0x006F2B50u;
            constexpr uint32_t kXmenProbeBytes = 0x30u;
            const uint64_t fileBackedStart = ph.vaddr;
            const uint64_t fileBackedEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (fileBackedStart <= kXmenProbeVtable &&
                static_cast<uint64_t>(kXmenProbeVtable) + kXmenProbeBytes <= fileBackedEnd)
            {
                const uint32_t probePhys = m_memory.translateAddress(kXmenProbeVtable);
                const uint8_t *probe = destBase + probePhys;
                std::cerr << "[xmen-loader-probe:6f2b50]";
                for (uint32_t probeOffset = 0u; probeOffset < kXmenProbeBytes; probeOffset += 4u)
                {
                    uint32_t word = 0u;
                    std::memcpy(&word, probe + probeOffset, sizeof(word));
                    std::cerr << " +0x" << std::hex << probeOffset << "=0x" << word;
                }
                std::cerr << std::dec << std::endl;
            }
        }

        if (ph.memsz > ph.filesz)
        {
            std::memset(dest + ph.filesz, 0, ph.memsz - ph.filesz);
        }

        RUNTIME_LOG("Loading segment: 0x" << std::hex << ph.vaddr
                                          << " - 0x" << (static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz))
                                          << " (filesz: 0x" << ph.filesz
                                          << ", memsz: 0x" << ph.memsz << ")"
                                          << std::dec << std::endl);

        if (!scratch)
        {
            maxLoadedRdramEnd = std::max(maxLoadedRdramEnd, static_cast<uint32_t>(segmentMemEnd));
        }

        if (ph.flags & 0x1u) // PF_X
        {
            const uint64_t execEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.filesz);
            if (execEnd <= std::numeric_limits<uint32_t>::max())
            {
                m_memory.registerCodeRegion(ph.vaddr, static_cast<uint32_t>(execEnd));
            }
        }

        loadedAnySegment = true;
        moduleBase = std::min(moduleBase, ph.vaddr);
        const uint64_t segmentVirtualEnd = static_cast<uint64_t>(ph.vaddr) + static_cast<uint64_t>(ph.memsz);
        const uint32_t clampedVirtualEnd =
            (segmentVirtualEnd > std::numeric_limits<uint32_t>::max())
                ? std::numeric_limits<uint32_t>::max()
                : static_cast<uint32_t>(segmentVirtualEnd);
        moduleEnd = std::max(moduleEnd, clampedVirtualEnd);
    }

    if (!loadedAnySegment)
    {
        std::cerr << "ELF contains no loadable PT_LOAD segments." << std::endl;
        return false;
    }

    if (maxLoadedRdramEnd > PS2_RAM_SIZE)
    {
        maxLoadedRdramEnd = PS2_RAM_SIZE;
    }

    const uint32_t paddedEnd = (maxLoadedRdramEnd > (PS2_RAM_SIZE - kGuestHeapSafetyPad))
                                   ? PS2_RAM_SIZE
                                   : (maxLoadedRdramEnd + kGuestHeapSafetyPad);
    const uint32_t suggestedHeapBase = alignGuestHeapValue(paddedEnd, kGuestHeapDefaultAlignment);
    {
        std::lock_guard<std::mutex> lock(m_guestHeapMutex);
        if (!m_guestHeapConfigured)
        {
            const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
            m_guestHeapSuggestedBase = std::min(suggestedHeapBase, hardLimit);
            m_guestHeapBase = m_guestHeapSuggestedBase;
            m_guestHeapEnd = m_guestHeapSuggestedBase;
            m_guestHeapLimit = hardLimit;
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
        const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
        m_asyncCallbackStackFloor = std::min(std::max(hardLimit, suggestedHeapBase), PS2_RAM_SIZE);
        m_asyncCallbackStackTop = PS2_RAM_SIZE;
    }

    LoadedModule module;
    module.name = elfPath.substr(elfPath.find_last_of("/\\") + 1);
    module.baseAddress = (moduleBase == std::numeric_limits<uint32_t>::max()) ? 0x00100000u : moduleBase;
    module.size = (moduleEnd > module.baseAddress) ? static_cast<size_t>(moduleEnd - module.baseAddress) : 0u;
    module.active = true;

    m_loadedModules.push_back(module);

    uint32_t elfCrc32 = 0u;
    const bool elfCrc32Valid = computeFileCrc32(elfPath, elfCrc32);
    if (!elfCrc32Valid)
    {
        std::cerr << "[ps2xIOP] failed to compute ELF CRC32 for '" << elfPath << "'" << std::endl;
    }
    ps2x::iop::GameIdentity identity;
    identity.elfName = module.name;
    identity.entryPoint = m_cpuContext.pc;
    identity.crc32 = elfCrc32;
    std::string iopError;
    if (!m_iopSubsystem->configure(identity, &iopError))
    {
        std::cerr << "[ps2xIOP] failed to configure profile: " << iopError << std::endl;
        return false;
    }

    ps2_game_overrides::applyMatching(*this,
                                      elfPath,
                                      m_cpuContext.pc,
                                      elfCrc32,
                                      elfCrc32Valid);

    RUNTIME_LOG("ELF file loaded successfully. Entry point: 0x" << std::hex << m_cpuContext.pc << std::dec);
    return true;
}

const PS2Runtime::IoPaths &PS2Runtime::getIoPaths()
{
    return runtimeIoPaths();
}

void PS2Runtime::setIoPaths(const IoPaths &paths)
{
    IoPaths normalized = paths;
    normalized.elfPath = normalizeAbsolutePath(normalized.elfPath);
    normalized.elfDirectory = normalizeAbsolutePath(normalized.elfDirectory);
    normalized.hostRoot = normalizeAbsolutePath(normalized.hostRoot);
    normalized.cdRoot = normalizeAbsolutePath(normalized.cdRoot);
    normalized.mcRoot = normalizeAbsolutePath(normalized.mcRoot);
    normalized.cdImage = normalizeAbsolutePath(normalized.cdImage);

    if (normalized.elfDirectory.empty() && !normalized.elfPath.empty())
    {
        normalized.elfDirectory = normalized.elfPath.parent_path();
    }

    if (normalized.hostRoot.empty())
    {
        normalized.hostRoot = normalized.elfDirectory;
    }
    if (normalized.cdRoot.empty())
    {
        normalized.cdRoot = normalized.elfDirectory;
    }
    if (normalized.mcRoot.empty())
    {
        normalized.mcRoot = normalized.elfDirectory / "mc0";
    }

    runtimeIoPaths() = normalized;
}

void PS2Runtime::configureIoPathsFromElf(const std::string &elfPath)
{
    IoPaths paths = runtimeIoPaths();
    paths.elfPath = normalizeAbsolutePath(std::filesystem::path(elfPath));
    if (!paths.elfPath.empty())
    {
        paths.elfDirectory = paths.elfPath.parent_path();
    }

    if (!paths.elfDirectory.empty())
    {
        paths.hostRoot = paths.elfDirectory;
        paths.cdRoot = paths.elfDirectory;
        paths.mcRoot = paths.elfDirectory / "mc0";
    }

    setIoPaths(paths);
}

namespace
{
    bool generatedFunctionTableSlot(uint32_t address, uint32_t &slot)
    {
        if ((address & 3u) != 0u || g_ps2RecompiledFunctionTableSlotCount == 0u)
        {
            return false;
        }

        if (address < g_ps2RecompiledFunctionTableBase || address >= g_ps2RecompiledFunctionTableEnd)
        {
            return false;
        }

        const uint32_t offset = address - g_ps2RecompiledFunctionTableBase;
        slot = offset >> 2;
        return slot < g_ps2RecompiledFunctionTableSlotCount;
    }
}

bool PS2Runtime::replaceFunction(uint32_t address, RecompiledFunction func)
{
    uint32_t slot = 0u;
    if (!generatedFunctionTableSlot(address, slot))
    {
        std::cerr << "[function-table] cannot replace guest PC 0x" << std::hex << address
                  << ": outside generated dense table [0x" << g_ps2RecompiledFunctionTableBase
                  << ", 0x" << g_ps2RecompiledFunctionTableEnd << ")"
                  << std::dec << std::endl;
        return false;
    }

    g_ps2RecompiledFunctionTable[slot] = func;
    return true;
}

bool PS2Runtime::registerFunction(uint32_t address, RecompiledFunction func)
{
    return replaceFunction(address, func);
}

bool PS2Runtime::hasFunction(uint32_t address) const
{
    uint32_t slot = 0u;
    return generatedFunctionTableSlot(address, slot) && g_ps2RecompiledFunctionTable[slot] != nullptr;
}

const char *describeGuestBranchKind(PS2Runtime::GuestBranchKind kind)
{
    switch (kind)
    {
    case PS2Runtime::GuestBranchKind::DirectJump:
        return "DirectJump";
    case PS2Runtime::GuestBranchKind::DirectCall:
        return "DirectCall";
    case PS2Runtime::GuestBranchKind::IndirectJump:
        return "IndirectJump";
    case PS2Runtime::GuestBranchKind::IndirectCall:
        return "IndirectCall";
    case PS2Runtime::GuestBranchKind::Return:
        return "Return";
    default:
        return "Unknown";
    }
}

PS2Runtime::RecompiledFunction PS2Runtime::lookupFunction(uint32_t address)
{
    pushDispatchPc(address);

    uint32_t slot = 0u;
    if (generatedFunctionTableSlot(address, slot))
    {
        RecompiledFunction fn = g_ps2RecompiledFunctionTable[slot];
        if (fn != nullptr)
        {
            return fn;
        }
    }

    std::cerr << "Error: No exact recompiled function for guest PC 0x" << std::hex << address
              << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
              << " tableEnd=0x" << g_ps2RecompiledFunctionTableEnd
              << " codeRegion=" << (m_memory.isCodeAddress(address) ? "yes" : "no")
              << " trace=" << formatDispatchHistory()
              << std::dec << std::endl;

    static RecompiledFunction missingFunction = [](uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t badPc = ctx->pc;
        runtime->reportMissingFunction(rdram,
                                       ctx,
                                       badPc,
                                       0u,
                                       PS2Runtime::GuestBranchKind::IndirectJump,
                                       "dispatch");
    };

    return missingFunction;
}

void PS2Runtime::setMissingFunctionPolicy(MissingFunctionPolicy policy)
{
    m_missingFunctionPolicy.store(static_cast<uint32_t>(policy), std::memory_order_release);
}

PS2Runtime::MissingFunctionPolicy PS2Runtime::missingFunctionPolicy() const
{
    return static_cast<MissingFunctionPolicy>(m_missingFunctionPolicy.load(std::memory_order_acquire));
}

void PS2Runtime::resetMissingFunctionReportOnce()
{
    m_missingFunctionReported.store(false, std::memory_order_release);
}

void PS2Runtime::reportMissingFunction(uint8_t *rdram,
                                       R5900Context *ctx,
                                       uint32_t targetPc,
                                       uint32_t sourcePc,
                                       GuestBranchKind kind,
                                       const char *debugName)
{
    const MissingFunctionPolicy policy = missingFunctionPolicy();
    const bool firstReport = !m_missingFunctionReported.exchange(true, std::memory_order_acq_rel);

    const uint32_t pc = ctx->pc;
    const uint32_t ra = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0));
    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0));
    const uint32_t gp = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[28], 0));
    const uint32_t a0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[4], 0));
    const uint32_t a1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[5], 0));
    const uint32_t a2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[6], 0));
    const uint32_t a3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[7], 0));
    const uint32_t s0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[16], 0));
    const uint32_t s1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[17], 0));
    const uint32_t s2 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[18], 0));
    const uint32_t s3 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[19], 0));
    const uint32_t s4 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[20], 0));
    const uint32_t v0 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[2], 0));
    const uint32_t v1 = static_cast<uint32_t>(_mm_extract_epi32(ctx->r[3], 0));

    auto readGuestU32At = [rdram](uint32_t addr, uint32_t &out) -> bool
    {
        // TODO this !rdram exist only because of test fix those test later
        if (!rdram || addr > PS2_RAM_SIZE - sizeof(uint32_t))
        {
            out = 0u;
            return false;
        }

        std::memcpy(&out, rdram + addr, sizeof(uint32_t));
        return true;
    };

    auto readGuestU32Offset = [&readGuestU32At](uint32_t base, uint32_t offset, uint32_t &out) -> bool
    {
        if (base > PS2_RAM_SIZE - sizeof(uint32_t) || offset > PS2_RAM_SIZE - sizeof(uint32_t) - base)
        {
            out = 0u;
            return false;
        }

        return readGuestU32At(base + offset, out);
    };

    uint32_t a0Word0 = 0u;
    uint32_t a0Word4 = 0u;
    uint32_t a0Word8 = 0u;
    uint32_t a0WordC = 0u;
    const bool a0Readable =
        readGuestU32Offset(a0, 0x00u, a0Word0) &&
        readGuestU32Offset(a0, 0x04u, a0Word4) &&
        readGuestU32Offset(a0, 0x08u, a0Word8) &&
        readGuestU32Offset(a0, 0x0cu, a0WordC);

    uint32_t s0Word0 = 0u;
    uint32_t s0Word4 = 0u;
    uint32_t s0Word8 = 0u;
    uint32_t s0WordC = 0u;
    const bool s0Readable =
        readGuestU32Offset(s0, 0x00u, s0Word0) &&
        readGuestU32Offset(s0, 0x04u, s0Word4) &&
        readGuestU32Offset(s0, 0x08u, s0Word8) &&
        readGuestU32Offset(s0, 0x0cu, s0WordC);

    uint32_t s1Word0 = 0u;
    uint32_t s1Word4 = 0u;
    uint32_t s1Word8 = 0u;
    uint32_t s1WordC = 0u;
    const bool s1Readable =
        readGuestU32Offset(s1, 0x00u, s1Word0) &&
        readGuestU32Offset(s1, 0x04u, s1Word4) &&
        readGuestU32Offset(s1, 0x08u, s1Word8) &&
        readGuestU32Offset(s1, 0x0cu, s1WordC);

    uint32_t s2Word0 = 0u;
    uint32_t s2Word4 = 0u;
    uint32_t s2Word8 = 0u;
    uint32_t s2WordC = 0u;
    const bool s2Readable =
        readGuestU32Offset(s2, 0x00u, s2Word0) &&
        readGuestU32Offset(s2, 0x04u, s2Word4) &&
        readGuestU32Offset(s2, 0x08u, s2Word8) &&
        readGuestU32Offset(s2, 0x0cu, s2WordC);

    uint32_t spWord0 = 0u;
    uint32_t spWord10 = 0u;
    uint32_t spWord20 = 0u;
    uint32_t spWord6C = 0u;
    const bool spReadable =
        readGuestU32Offset(sp, 0x00u, spWord0) &&
        readGuestU32Offset(sp, 0x10u, spWord10) &&
        readGuestU32Offset(sp, 0x20u, spWord20) &&
        readGuestU32Offset(sp, 0x6cu, spWord6C);

    uint32_t recordWord0 = 0u;
    uint32_t recordWord4 = 0u;
    uint32_t recordWord8 = 0u;
    uint32_t recordWordC = 0u;
    const bool recordReadable =
        s0Readable && s0Word4 != 0u &&
        readGuestU32Offset(s0Word4, 0x00u, recordWord0) &&
        readGuestU32Offset(s0Word4, 0x04u, recordWord4) &&
        readGuestU32Offset(s0Word4, 0x08u, recordWord8) &&
        readGuestU32Offset(s0Word4, 0x0cu, recordWordC);

    uint32_t vtableSlot0 = 0u;
    uint32_t vtableSlot4 = 0u;
    uint32_t vtableSlot8 = 0u;
    uint32_t vtableSlotC = 0u;
    const bool vtableReadable =
        a0Readable && a0Word0 != 0u &&
        readGuestU32Offset(a0Word0, 0x00u, vtableSlot0) &&
        readGuestU32Offset(a0Word0, 0x04u, vtableSlot4) &&
        readGuestU32Offset(a0Word0, 0x08u, vtableSlot8) &&
        readGuestU32Offset(a0Word0, 0x0cu, vtableSlotC);

    if (firstReport)
    {
        std::ostringstream oss;
        oss << "[guest-branch:missing-target] kind=" << describeGuestBranchKind(kind)
            << " op=" << (debugName ? debugName : "<unknown>")
            << " source=0x" << std::hex << sourcePc
            << " target=0x" << targetPc
            << " pc=0x" << pc
            << " ra=0x" << ra
            << " sp=0x" << sp
            << " gp=0x" << gp
            << " a0=0x" << a0
            << " a1=0x" << a1
            << " a2=0x" << a2
            << " a3=0x" << a3
            << " s0=0x" << s0
            << " s1=0x" << s1
            << " s2=0x" << s2
            << " s3=0x" << s3
            << " s4=0x" << s4
            << " v0=0x" << v0
            << " v1=0x" << v1
            << " a0Readable=" << (a0Readable ? "yes" : "no")
            << " a0[0]=0x" << a0Word0
            << " a0[4]=0x" << a0Word4
            << " a0[8]=0x" << a0Word8
            << " a0[c]=0x" << a0WordC
            << " s0Readable=" << (s0Readable ? "yes" : "no")
            << " s0[0]=0x" << s0Word0
            << " s0[4]=0x" << s0Word4
            << " s0[8]=0x" << s0Word8
            << " s0[c]=0x" << s0WordC
            << " s1Readable=" << (s1Readable ? "yes" : "no")
            << " s1[0]=0x" << s1Word0
            << " s1[4]=0x" << s1Word4
            << " s1[8]=0x" << s1Word8
            << " s1[c]=0x" << s1WordC
            << " s2Readable=" << (s2Readable ? "yes" : "no")
            << " s2[0]=0x" << s2Word0
            << " s2[4]=0x" << s2Word4
            << " s2[8]=0x" << s2Word8
            << " s2[c]=0x" << s2WordC
            << " spReadable=" << (spReadable ? "yes" : "no")
            << " sp[0]=0x" << spWord0
            << " sp[10]=0x" << spWord10
            << " sp[20]=0x" << spWord20
            << " sp[6c]=0x" << spWord6C
            << " recordReadable=" << (recordReadable ? "yes" : "no")
            << " record[0]=0x" << recordWord0
            << " record[4]=0x" << recordWord4
            << " record[8]=0x" << recordWord8
            << " record[c]=0x" << recordWordC
            << " vtableReadable=" << (vtableReadable ? "yes" : "no")
            << " vtbl[0]=0x" << vtableSlot0
            << " vtbl[4]=0x" << vtableSlot4
            << " vtbl[8]=0x" << vtableSlot8
            << " vtbl[c]=0x" << vtableSlotC
            << " codeRegion=" << (m_memory.isCodeAddress(targetPc) ? "yes" : "no")
            << " policy=" << static_cast<uint32_t>(policy)
            << " trace=" << formatDispatchHistory()
            << std::dec;

        static std::mutex s_missingFunctionLogMutex;
        {
            std::lock_guard<std::mutex> lock(s_missingFunctionLogMutex);
            std::cerr << oss.str() << std::endl;
        }
    }

    if (firstReport && policy == MissingFunctionPolicy::BreakOnce)
    {
#if defined(_MSC_VER)
        __debugbreak();
#endif // TODO others breakpoints
    }

    if (ctx)
    {
        ctx->pc = targetPc;
    }

    if (policy == MissingFunctionPolicy::Stop)
    {
        requestStop();
    }
}

bool PS2Runtime::dispatchGuestBranch(uint8_t *rdram,
                                     R5900Context *ctx,
                                     uint32_t targetPc,
                                     uint32_t sourcePc,
                                     uint32_t fallthroughPc,
                                     GuestBranchKind kind,
                                     const char *debugName)
{
    ctx->pc = targetPc;
    const bool isCall = (kind == GuestBranchKind::DirectCall || kind == GuestBranchKind::IndirectCall);
    constexpr uint32_t kXmenDispatchTable = 0x006AC600u;
    constexpr std::array<uint32_t, 14> kXmenExpectedDispatchTable = {
        0x002F3400u, 0x002F3400u, 0x002F3400u, 0x002F313Cu,
        0x002F3584u, 0x002F3584u, 0x002F3584u, 0x002F3258u,
        0x002F32E8u, 0x002F3370u, 0x002F31B8u, 0x002F30B4u,
        0x002F3000u, 0x002F3038u,
    };
    static std::atomic<bool> s_xmenDispatchTableClobberLogged{false};
    static std::atomic<bool> s_xmenPacketCursorOverrunLogged{false};
    static thread_local uint32_t s_xmenDispatchTableSampleCounter = 0u;
    static thread_local bool s_xmenSlotBaseInitialized = false;
    static thread_local uint32_t s_xmenLastSlotManager = 0u;
    static thread_local uint32_t s_xmenLastSlotBase = 0u;
    static thread_local bool s_xmenSlotGrowActive = false;
    static thread_local uint32_t s_xmenSlotGrowOldAddress = 0u;
    static thread_local uint32_t s_xmenSlotGrowBytes = 0u;
    if (sourcePc == 0x0024B2ACu && targetPc == 0u)
    {
        static std::atomic<uint32_t> s_xmenNullListenerTraceCount{0u};
        const uint32_t count = s_xmenNullListenerTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t container = GPR_U32(ctx, 19);
            const uint32_t descriptor = readRdramProbeU32(rdram, container + 8u);
            const uint32_t storage = readRdramProbeU32(rdram, descriptor + 0x10u);
            std::cerr << "[xmen-null-listener] index=" << std::dec << count
                      << " container=0x" << std::hex << container
                      << " event=0x" << GPR_U32(ctx, 18)
                      << " listenerIndex=0x" << GPR_U32(ctx, 16)
                      << " listener=0x" << GPR_U32(ctx, 4)
                      << " descriptor=0x" << descriptor
                      << " descriptor0=0x" << readRdramProbeU32(rdram, descriptor)
                      << " descriptor4=0x" << readRdramProbeU32(rdram, descriptor + 4u)
                      << " descriptor8=0x" << readRdramProbeU32(rdram, descriptor + 8u)
                      << " descriptorC=0x" << readRdramProbeU32(rdram, descriptor + 0x0Cu)
                      << " storage=0x" << storage
                      << " storage0=0x" << readRdramProbeU32(rdram, storage)
                      << " storage4=0x" << readRdramProbeU32(rdram, storage + 4u)
                      << " storage8=0x" << readRdramProbeU32(rdram, storage + 8u)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x00271BA0u && targetPc == 0x00200F30u)
    {
        const uint32_t manager = GPR_U32(ctx, 17);
        s_xmenSlotGrowActive = true;
        s_xmenSlotGrowOldAddress = GPR_U32(ctx, 4);
        s_xmenSlotGrowBytes = GPR_U32(ctx, 5);
        std::cerr << "[xmen-render-slot-grow-enter] manager=0x" << std::hex << manager
                  << " used=0x" << readRdramProbeU32(rdram, manager)
                  << " free=0x" << readRdramProbeU32(rdram, manager + 4u)
                  << " oldBase=0x" << GPR_U32(ctx, 4)
                  << " bytes=0x" << GPR_U32(ctx, 5)
                  << " alignment=0x" << GPR_U32(ctx, 6)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << std::dec << std::endl;
    }
    if (isCall && sourcePc == 0x00271BC8u && targetPc == 0x001240CCu)
    {
        const uint32_t manager = GPR_U32(ctx, 17);
        std::cerr << "[xmen-render-slot-grow-return] manager=0x" << std::hex << manager
                  << " used=0x" << readRdramProbeU32(rdram, manager)
                  << " free=0x" << readRdramProbeU32(rdram, manager + 4u)
                  << " newBase=0x" << readRdramProbeU32(rdram, manager + 0x10u)
                  << " allocationResult=0x" << GPR_U32(ctx, 2)
                  << " clearAddress=0x" << GPR_U32(ctx, 4)
                  << " clearBytes=0x" << GPR_U32(ctx, 5)
                  << std::dec << std::endl;
        s_xmenSlotGrowActive = false;
    }
    if (s_xmenSlotGrowActive && isCall &&
        (sourcePc == 0x00203FE0u || sourcePc == 0x00203FFCu ||
         sourcePc == 0x00204058u || sourcePc == 0x00204074u))
    {
        std::cerr << "[xmen-render-slot-owner-check] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " candidate=0x" << GPR_U32(ctx, 4)
                  << " pointer=0x" << s_xmenSlotGrowOldAddress
                  << " argument=0x" << GPR_U32(ctx, 5)
                  << " bytes=0x" << s_xmenSlotGrowBytes
                  << std::dec << std::endl;
    }
    if (s_xmenSlotGrowActive &&
        ((sourcePc == 0x00232048u && kind == GuestBranchKind::IndirectJump) ||
         (sourcePc == 0x00231EE8u && kind == GuestBranchKind::IndirectJump)))
    {
        std::cerr << "[xmen-render-slot-backend] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " heap=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " a3=0x" << GPR_U32(ctx, 7)
                  << std::dec << std::endl;
    }
    if (isCall && sourcePc == 0x00200EECu)
    {
        std::cerr << "[xmen-realloc-method-enter] target=0x" << std::hex << targetPc
                  << " heap=0x" << GPR_U32(ctx, 4)
                  << " oldAddress=0x" << GPR_U32(ctx, 5)
                  << " bytes=0x" << GPR_U32(ctx, 6)
                  << " alignment=0x" << GPR_U32(ctx, 7)
                  << std::dec << std::endl;
    }
    if (isCall && sourcePc == 0x00200D6Cu)
    {
        std::cerr << "[xmen-alloc-method-enter] target=0x" << std::hex << targetPc
                  << " heap=0x" << GPR_U32(ctx, 4)
                  << " bytes=0x" << GPR_U32(ctx, 5)
                  << " alignment=0x" << GPR_U32(ctx, 6)
                  << std::dec << std::endl;
    }
    if (rdram)
    {
        const uint32_t slotManager = readRdramProbeU32(rdram, 0x009727C8u);
        const uint32_t slotBase = readRdramProbeU32(rdram, slotManager + 0x10u);
        if (!s_xmenSlotBaseInitialized || slotManager != s_xmenLastSlotManager ||
            slotBase != s_xmenLastSlotBase)
        {
            std::cerr << "[xmen-render-slot-base-change] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=0x" << static_cast<uint32_t>(kind)
                      << " managerOld=0x" << s_xmenLastSlotManager
                      << " managerNew=0x" << slotManager
                      << " baseOld=0x" << s_xmenLastSlotBase
                      << " baseNew=0x" << slotBase
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
            s_xmenSlotBaseInitialized = true;
            s_xmenLastSlotManager = slotManager;
            s_xmenLastSlotBase = slotBase;
        }
    }
    if (rdram && targetPc == 0x002E6C60u &&
        !s_xmenPacketCursorOverrunLogged.load(std::memory_order_relaxed))
    {
        const uint32_t dmaDescriptor = readRdramProbeU32(rdram, 0x00750778u);
        const uint32_t descriptorBase = readRdramProbeU32(rdram, dmaDescriptor + 0x20u);
        const uint32_t descriptorEnd = readRdramProbeU32(rdram, dmaDescriptor + 0x28u);
        const uint32_t descriptorCurrent = readRdramProbeU32(rdram, dmaDescriptor + 0x30u);
        if (descriptorCurrent >= descriptorEnd &&
            !s_xmenPacketCursorOverrunLogged.exchange(true, std::memory_order_relaxed))
        {
            const uint32_t renderRecord = readRdramProbeU32(rdram, GPR_U32(ctx, 22));
            const uint32_t recordData = readRdramProbeU32(rdram, renderRecord);
            const uint32_t recordStride = readRdramProbeU16(rdram, renderRecord + 0x3Cu);
            const uint32_t recordRows = readRdramProbeU16(rdram, renderRecord + 0x3Eu);
            const uint32_t recordFormat = readRdramProbeU16(rdram, renderRecord + 0x42u);
            const uint32_t strideScale = readRdramProbeU32(
                rdram, 0x0065A520u + recordFormat * sizeof(uint32_t));
            const uint32_t divisor = recordStride * strideScale;
            std::cerr << "[xmen-packet-cursor-overrun] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " descriptor=0x" << dmaDescriptor
                      << " base=0x" << descriptorBase
                      << " end=0x" << descriptorEnd
                      << " current=0x" << descriptorCurrent
                      << " at=0x" << GPR_U32(ctx, 1)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " v1=0x" << GPR_U32(ctx, 3)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << " s5=0x" << GPR_U32(ctx, 21)
                      << " s6=0x" << GPR_U32(ctx, 22)
                      << " s7=0x" << GPR_U32(ctx, 23)
                      << " record=0x" << renderRecord
                      << " recordData=0x" << recordData
                      << " recordStride=0x" << recordStride
                      << " recordRows=0x" << recordRows
                      << " recordFormat=0x" << recordFormat
                      << " strideScale=0x" << strideScale
                      << " divisor=0x" << divisor
                      << " quotient=0x" << (divisor != 0u ? 0x000FFFE0u / divisor : 0xFFFFFFFFu)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
            requestStop();
            return false;
        }
    }
    const bool sampleXmenDispatchTable =
        (++s_xmenDispatchTableSampleCounter & 0xFFu) == 0u;
    if (rdram && sampleXmenDispatchTable &&
        !s_xmenDispatchTableClobberLogged.load(std::memory_order_relaxed))
    {
        std::array<uint32_t, kXmenExpectedDispatchTable.size()> observed{};
        std::memcpy(observed.data(), rdram + kXmenDispatchTable, sizeof(observed));
        if (observed != kXmenExpectedDispatchTable &&
            !s_xmenDispatchTableClobberLogged.exchange(true, std::memory_order_relaxed))
        {
            const uint32_t dmaDescriptor = readRdramProbeU32(rdram, 0x00750778u);
            std::cerr << "[xmen-dispatch-table-clobber] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " descriptor=0x" << dmaDescriptor
                      << " descriptorBase=0x" << readRdramProbeU32(rdram, dmaDescriptor + 0x20u)
                      << " descriptorEnd=0x" << readRdramProbeU32(rdram, dmaDescriptor + 0x28u)
                      << " descriptorCurrent=0x" << readRdramProbeU32(rdram, dmaDescriptor + 0x30u)
                      << " descriptorNext=0x" << readRdramProbeU32(rdram, dmaDescriptor + 0x34u)
                      << " table=";
            for (size_t index = 0u; index < observed.size(); ++index)
            {
                std::cerr << (index == 0u ? "" : ",") << "0x" << observed[index];
            }
            std::cerr << " trace=" << formatDispatchHistory() << std::dec << std::endl;
        }
    }
    if (g_xmenLiveChainSavedReturnSlot != 0u)
    {
        const uint64_t observed =
            readRdramProbeU64(rdram, g_xmenLiveChainSavedReturnSlot);
        if (observed != g_xmenLiveChainExpectedReturn &&
            observed != g_xmenLiveChainLastObserved)
        {
            static std::atomic<uint32_t> s_xmenLiveChainMismatchCount{0u};
            const uint32_t index =
                s_xmenLiveChainMismatchCount.fetch_add(1u, std::memory_order_relaxed);
            if (index < 2048u)
            {
                std::cerr << "[xmen-live-chain-mismatch] index=" << std::dec << index
                          << " sequence=" << g_xmenLiveChainSequence
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " fallthrough=0x" << fallthroughPc
                          << " kind=" << describeGuestBranchKind(kind)
                          << " slot=0x" << g_xmenLiveChainSavedReturnSlot
                          << " expected=0x" << g_xmenLiveChainExpectedReturn
                          << " observed=0x" << observed
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " sp=0x" << GPR_U32(ctx, 29)
                          << std::dec << std::endl;
            }
        }
        g_xmenLiveChainLastObserved = observed;
    }
    const uint64_t xmenBranchTick = m_memory.gs().vsyncTick.load(std::memory_order_relaxed);
    if (targetPc == 0x001FD030u || targetPc == 0x001FD740u)
    {
        const uint32_t path = GPR_U32(ctx, 5);
        const std::string pathText = readGuestPrintableString(rdram, path, 128u);
        if (pathText.find("maps/menu/main_back.igb") != std::string::npos)
        {
            static std::atomic<uint32_t> s_xmenMainBackLoaderEntryCount{0u};
            const uint32_t traceIndex =
                s_xmenMainBackLoaderEntryCount.fetch_add(1u, std::memory_order_relaxed);
            if (traceIndex < 8u)
            {
                const uint32_t output = GPR_U32(ctx, 4);
                std::cout << "[xmen-main-back-loader-entry] index=" << traceIndex
                          << " tick=" << xmenBranchTick
                          << " thread=" << eeScheduler().currentThreadId()
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " fallthrough=0x" << fallthroughPc
                          << " kind=" << describeGuestBranchKind(kind)
                          << " output=0x" << output
                          << " outputValue=0x" << readRdramProbeU32(rdram, output)
                          << " path=0x" << path
                          << " a2=0x" << GPR_U32(ctx, 6)
                          << " a3=0x" << GPR_U32(ctx, 7)
                          << " sp=0x" << GPR_U32(ctx, 29)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " pathText=\"" << pathText << "\""
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            }
        }
    }
    constexpr std::array<uint32_t, 16> sceneObjectIndices = {
        179u, 31u, 116u, 210u, 390u, 172u, 251u, 92u,
        318u, 213u, 119u, 389u, 319u, 334u, 341u, 345u,
    };
    constexpr std::array<uint32_t, 16> sceneObjectVtables = {
        0x006F4870u, 0x006F4870u, 0x006F5440u, 0x006F3BA0u,
        0x006F4910u, 0x006F4910u, 0x006F4910u, 0x006F4910u,
        0x006F4910u, 0x006F4910u, 0x006F4910u, 0x006F4910u,
        0x006F4910u, 0x006F32D0u, 0x006F32D0u, 0x006F32D0u,
    };
    static std::array<std::atomic<uint32_t>, 16> s_xmenSceneObjects{};
    static std::array<std::atomic<bool>, 16> s_xmenSceneObjectLifetimeBreaks{};
    static std::atomic<uint32_t> s_xmenSceneTrackedPackage{0u};
    static std::atomic<uint32_t> s_xmenSceneGeneration{0u};
    static std::atomic<uint32_t> s_xmenSceneObjectCallCount{0u};
    static std::atomic<uint32_t> s_xmenSceneObjectLiveCallCount{0u};
    static std::atomic<uint64_t> s_xmenSceneCaptureTick{0u};
    const uint32_t scenePackage =
        g_xmenMainBackIgbPackage.load(std::memory_order_relaxed);
    const uint32_t trackedScenePackage =
        s_xmenSceneTrackedPackage.load(std::memory_order_relaxed);
    if (scenePackage != 0u && scenePackage != trackedScenePackage)
    {
        s_xmenSceneTrackedPackage.store(scenePackage, std::memory_order_relaxed);
        s_xmenSceneGeneration.fetch_add(1u, std::memory_order_relaxed);
        s_xmenSceneObjectCallCount.store(0u, std::memory_order_relaxed);
        s_xmenSceneObjectLiveCallCount.store(0u, std::memory_order_relaxed);
        s_xmenSceneCaptureTick.store(xmenBranchTick, std::memory_order_relaxed);
        for (size_t slot = 0u; slot < sceneObjectIndices.size(); ++slot)
        {
            s_xmenSceneObjects[slot].store(0u, std::memory_order_relaxed);
            s_xmenSceneObjectLifetimeBreaks[slot].store(false, std::memory_order_relaxed);
        }
    }
    const uint32_t sceneGeneration =
        s_xmenSceneGeneration.load(std::memory_order_relaxed);
    const uint32_t objectEntries =
        g_xmenMainBackIgbObjectEntries.load(std::memory_order_relaxed);
    if (xmenBranchTick >= 1800u && objectEntries != 0u)
    {
        for (size_t slot = 0u; slot < sceneObjectIndices.size(); ++slot)
        {
            if (s_xmenSceneObjects[slot].load(std::memory_order_relaxed) != 0u)
            {
                continue;
            }
            const uint32_t object = readRdramProbeU32(
                rdram, objectEntries + sceneObjectIndices[slot] * 4u);
            if (readRdramProbeU32(rdram, object) == sceneObjectVtables[slot])
            {
                uint32_t expected = 0u;
                s_xmenSceneObjects[slot].compare_exchange_strong(
                    expected, object, std::memory_order_relaxed);
            }
        }
    }
    const uint64_t sceneCaptureTick =
        s_xmenSceneCaptureTick.load(std::memory_order_relaxed);
    if (sceneGeneration != 0u && xmenBranchTick > sceneCaptureTick + 5u)
    {
        const uint32_t a0 = GPR_U32(ctx, 4);
        for (size_t slot = 0u; slot < sceneObjectIndices.size(); ++slot)
        {
            const uint32_t object = s_xmenSceneObjects[slot].load(std::memory_order_relaxed);
            if (object == 0u)
            {
                continue;
            }
            const uint32_t currentVtable = readRdramProbeU32(rdram, object);
            if (currentVtable != sceneObjectVtables[slot] &&
                !s_xmenSceneObjectLifetimeBreaks[slot].exchange(true, std::memory_order_relaxed))
            {
                std::cout << "[xmen-scene-object-lifetime-break] tick=" << xmenBranchTick
                          << " generation=" << sceneGeneration
                          << " package=0x" << std::hex << scenePackage
                          << std::dec << " packageIndex=" << sceneObjectIndices[slot]
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " object=0x" << object
                          << " expectedVtable=0x" << sceneObjectVtables[slot]
                          << " currentVtable=0x" << currentVtable
                          << " word4=0x" << readRdramProbeU32(rdram, object + 4u)
                          << " word8=0x" << readRdramProbeU32(rdram, object + 8u)
                          << " wordc=0x" << readRdramProbeU32(rdram, object + 12u)
                          << std::dec << std::endl;
            }
            if ((kind == GuestBranchKind::DirectCall ||
                 kind == GuestBranchKind::IndirectCall) &&
                currentVtable == sceneObjectVtables[slot])
            {
                constexpr std::array<const char *, 15> liveRegisterNames = {
                    "a0", "a1", "a2", "a3", "v0", "v1", "s0", "s1",
                    "s2", "s3", "s4", "t0", "t1", "t8", "t9",
                };
                const std::array<uint32_t, liveRegisterNames.size()> liveRegisterValues = {
                    GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7),
                    GPR_U32(ctx, 2), GPR_U32(ctx, 3), GPR_U32(ctx, 16), GPR_U32(ctx, 17),
                    GPR_U32(ctx, 18), GPR_U32(ctx, 19), GPR_U32(ctx, 20), GPR_U32(ctx, 8),
                    GPR_U32(ctx, 9), GPR_U32(ctx, 24), GPR_U32(ctx, 25),
                };
                for (size_t registerIndex = 0u;
                     registerIndex < liveRegisterValues.size();
                     ++registerIndex)
                {
                    if (liveRegisterValues[registerIndex] != object)
                    {
                        continue;
                    }
                    const uint32_t traceIndex =
                        s_xmenSceneObjectLiveCallCount.fetch_add(1u, std::memory_order_relaxed);
                    if (traceIndex < 4096u)
                    {
                        std::cout << "[xmen-scene-object-live] index=" << traceIndex
                                  << " tick=" << xmenBranchTick
                                  << " generation=" << sceneGeneration
                                  << " package=0x" << std::hex << scenePackage
                                  << std::dec << " packageIndex=" << sceneObjectIndices[slot]
                                  << " reg=" << liveRegisterNames[registerIndex]
                                  << " kind=" << describeGuestBranchKind(kind)
                                  << " source=0x" << std::hex << sourcePc
                                  << " target=0x" << targetPc
                                  << " fallthrough=0x" << fallthroughPc
                                  << " object=0x" << object
                                  << " vtable=0x" << currentVtable
                                  << " ra=0x" << GPR_U32(ctx, 31)
                                  << std::dec << std::endl;
                    }
                    break;
                }
            }
            if (a0 != object)
            {
                continue;
            }

            const uint32_t traceIndex =
                s_xmenSceneObjectCallCount.fetch_add(1u, std::memory_order_relaxed);
            if (traceIndex < 1024u)
            {
                std::cout << "[xmen-scene-object-call] index=" << traceIndex
                          << " tick=" << xmenBranchTick
                          << " generation=" << sceneGeneration
                          << " package=0x" << std::hex << scenePackage
                          << std::dec << " packageIndex=" << sceneObjectIndices[slot]
                          << std::dec << " thread=" << eeScheduler().currentThreadId()
                          << " kind=" << describeGuestBranchKind(kind)
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " fallthrough=0x" << fallthroughPc
                          << " a0=0x" << a0
                          << " vtable=0x" << currentVtable
                          << " a1=0x" << GPR_U32(ctx, 5)
                          << " a2=0x" << GPR_U32(ctx, 6)
                          << " a3=0x" << GPR_U32(ctx, 7)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << std::dec << std::endl;
            }
            break;
        }
    }
    static std::array<std::atomic<uint32_t>, 32> s_xmenTrackedTextureObjects{};
    static std::array<std::atomic<uint32_t>, 32> s_xmenTrackedTextureVtables{};
    static std::array<std::atomic<bool>, 32> s_xmenTrackedTextureMutations{};
    static std::atomic<uint32_t> s_xmenTrackedTextureCount{0u};

    if (xmenBranchTick >= 1800u &&
        sourcePc == 0x002BD504u && targetPc == 0x00215FA0u)
    {
        const uint32_t object = GPR_U32(ctx, 5);
        const uint32_t vtable = readRdramProbeU32(rdram, object);
        uint32_t slot = 0u;
        for (; slot < s_xmenTrackedTextureObjects.size(); ++slot)
        {
            uint32_t tracked = s_xmenTrackedTextureObjects[slot].load(std::memory_order_relaxed);
            if (tracked == object)
            {
                break;
            }
            if (tracked == 0u &&
                s_xmenTrackedTextureObjects[slot].compare_exchange_strong(
                    tracked, object, std::memory_order_relaxed))
            {
                s_xmenTrackedTextureVtables[slot].store(vtable, std::memory_order_relaxed);
                s_xmenTrackedTextureMutations[slot].store(false, std::memory_order_relaxed);
                s_xmenTrackedTextureCount.fetch_add(1u, std::memory_order_relaxed);
                break;
            }
        }
        static std::atomic<uint32_t> s_xmenTextureRegistrationTraceCount{0u};
        const uint32_t traceIndex =
            s_xmenTextureRegistrationTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 96u)
        {
            const uint32_t manager = GPR_U32(ctx, 18);
            const uint32_t list = GPR_U32(ctx, 4);
            std::cout << "[xmen-texture-register] index=" << traceIndex
                      << " tick=" << xmenBranchTick
                      << " slot=" << slot
                      << " manager=0x" << std::hex << manager
                      << " managerVtable=0x" << readRdramProbeU32(rdram, manager)
                      << " list=0x" << list
                      << " countBefore=0x" << readRdramProbeU32(rdram, list + 0x08u)
                      << " entriesBefore=0x" << readRdramProbeU32(rdram, list + 0x10u)
                      << " object=0x" << object
                      << " objectVtable=0x" << vtable
                      << " refcount=0x" << readRdramProbeU32(rdram, object + 0x04u)
                      << " name=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 17), 96u) << "\""
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }

    if (xmenBranchTick >= 1800u)
    {
        const uint32_t trackedCount = std::min<uint32_t>(
            s_xmenTrackedTextureCount.load(std::memory_order_relaxed),
            static_cast<uint32_t>(s_xmenTrackedTextureObjects.size()));
        for (uint32_t slot = 0u; slot < trackedCount; ++slot)
        {
            const uint32_t object = s_xmenTrackedTextureObjects[slot].load(std::memory_order_relaxed);
            const uint32_t expectedVtable =
                s_xmenTrackedTextureVtables[slot].load(std::memory_order_relaxed);
            if (object == 0u || expectedVtable == 0u)
            {
                continue;
            }
            const uint32_t currentVtable = readRdramProbeU32(rdram, object);
            if (currentVtable != expectedVtable &&
                !s_xmenTrackedTextureMutations[slot].exchange(true, std::memory_order_relaxed))
            {
                std::cout << "[xmen-texture-lifetime-break] tick=" << xmenBranchTick
                          << " slot=" << slot
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " object=0x" << object
                          << " expectedVtable=0x" << expectedVtable
                          << " currentVtable=0x" << currentVtable
                          << " word4=0x" << readRdramProbeU32(rdram, object + 0x04u)
                          << " word8=0x" << readRdramProbeU32(rdram, object + 0x08u)
                          << " wordc=0x" << readRdramProbeU32(rdram, object + 0x0Cu)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            }
        }
    }
    if (targetPc == 0x00377D60u || targetPc == 0x003772C0u ||
        targetPc == 0x00372480u || targetPc == 0x00373060u)
    {
        static std::atomic<uint32_t> s_xmenTitleScriptCommandCount{0u};
        const uint32_t traceIndex =
            s_xmenTitleScriptCommandCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 128u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t a2 = GPR_U32(ctx, 6);
            const uint32_t a3 = GPR_U32(ctx, 7);
            std::cout << "[xmen-title-script-command] index=" << traceIndex
                      << " tick=" << xmenBranchTick
                      << " thread=" << eeScheduler().currentThreadId()
                      << " kind=" << describeGuestBranchKind(kind)
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " a0=0x" << a0
                      << " a1=0x" << a1
                      << " a2=0x" << a2
                      << " a3=0x" << a3
                      << " a0str=\"" << readGuestPrintableString(rdram, a0, 80u) << "\""
                      << " a1str=\"" << readGuestPrintableString(rdram, a1, 80u) << "\""
                      << " a2str=\"" << readGuestPrintableString(rdram, a2, 80u) << "\""
                       << " a3str=\"" << readGuestPrintableString(rdram, a3, 80u) << "\""
                       << std::dec << std::endl;
            if (sourcePc == 0x0059CAECu)
            {
                const uint32_t command = GPR_U32(ctx, 20);
                const uint32_t commandName = readRdramProbeU32(rdram, command);
                std::cout << "[xmen-title-script-binding] index=" << traceIndex
                          << " tick=" << xmenBranchTick
                          << " command=0x" << std::hex << command
                          << " name=0x" << commandName
                          << " nameText=\"" << readGuestPrintableString(rdram, commandName, 80u) << "\""
                          << " argCount=0x" << readRdramProbeU32(rdram, command + 0x24u)
                          << " callback=0x" << readRdramProbeU32(rdram, command + 0x28u)
                          << " next=0x" << readRdramProbeU32(rdram, command + 0x2Cu)
                          << " flags=0x" << readRdramProbeU32(rdram, command + 0x30u)
                          << std::dec << std::endl;
            }
        }
    }
    if (targetPc == 0x004B3F50u && sourcePc == 0x005226DCu &&
        GPR_U32(ctx, 4) == 0x00855B50u && GPR_U32(ctx, 5) == 0xFFFFFFFFu)
    {
        std::cout << "[xmen-motion-path-preload-retain] tick=" << xmenBranchTick
                  << " source=0x" << std::hex << sourcePc
                  << " object=0x" << GPR_U32(ctx, 4)
                  << " child=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 4) + 4u)
                  << std::dec << std::endl;
        ctx->pc = fallthroughPc;
        SET_GPR_U32(ctx, 2, GPR_U32(ctx, 4));
        return true;
    }
    if (xmenBranchTick >= 1800u &&
        (sourcePc == 0x004B3CE0u || sourcePc == 0x004B3DF0u))
    {
        static std::atomic<uint32_t> s_xmenMotionPathChildCallCount{0u};
        const uint32_t traceIndex =
            s_xmenMotionPathChildCallCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 768u)
        {
            const uint32_t child = GPR_U32(ctx, 4);
            const uint32_t parent = sourcePc == 0x004B3DF0u
                ? GPR_U32(ctx, 17)
                : 0u;
            std::cout << "[xmen-motion-path-child-call] index=" << traceIndex
                      << " tick=" << xmenBranchTick
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " child=0x" << child
                      << " childVtable=0x" << readRdramProbeU32(rdram, child)
                      << " parent=0x" << parent
                      << " parentVtable=0x" << readRdramProbeU32(rdram, parent)
                      << " parent4=0x" << readRdramProbeU32(rdram, parent + 4u)
                      << " parent8=0x" << readRdramProbeU32(rdram, parent + 8u)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (targetPc == 0u &&
        (sourcePc == 0x004B3CE0u || sourcePc == 0x004B3DF0u) &&
        GPR_U32(ctx, 4) == 0x00C17240u)
    {
        constexpr uint32_t workingPreloadChild = 0x00CCD380u;
        constexpr uint32_t expectedChildVtable = 0x006F30A0u;
        const uint32_t preloadVtable = readRdramProbeU32(rdram, workingPreloadChild);
        const uint32_t methodOffset = sourcePc == 0x004B3CE0u ? 0x104u : 0x5Cu;
        const uint32_t replacementTarget =
            readRdramProbeU32(rdram, preloadVtable + methodOffset);
        const bool canSubstitute =
            preloadVtable == expectedChildVtable &&
            hasFunction(replacementTarget);
        static std::atomic<uint32_t> s_xmenMotionPathPreloadSubstitutionCount{0u};
        const uint32_t traceIndex =
            s_xmenMotionPathPreloadSubstitutionCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 64u)
        {
            std::cout << "[xmen-motion-path-preload-substitution] index=" << traceIndex
                      << " tick=" << xmenBranchTick
                      << " source=0x" << std::hex << sourcePc
                      << " staleChild=0x" << workingPreloadChild
                      << " staleVtable=0x" << preloadVtable
                      << " replacementTarget=0x" << replacementTarget
                      << " applied=" << std::dec << (canSubstitute ? 1u : 0u)
                      << std::endl;
        }
        if (canSubstitute)
        {
            if (sourcePc == 0x004B3DF0u)
            {
                const uint32_t parent = GPR_U32(ctx, 17);
                if (readRdramProbeU32(rdram, parent + 4u) == 0x00C17240u)
                {
                    writeRdramProbeU32(rdram, parent + 4u, workingPreloadChild);
                }
            }
            SET_GPR_U32(ctx, 4, workingPreloadChild);
            targetPc = replacementTarget;
            ctx->pc = replacementTarget;
        }
    }
    if (xmenBranchTick >= 1800u &&
        (sourcePc == 0x002BCF78u || sourcePc == 0x002BCFE8u))
    {
        static std::atomic<uint32_t> s_xmenTitleObjectListLookupCount{0u};
        const uint32_t traceIndex =
            s_xmenTitleObjectListLookupCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 64u)
        {
            const uint32_t owner = GPR_U32(ctx, 19);
            const uint32_t list = readRdramProbeU32(rdram, owner + 0x14u);
            const uint32_t count = readRdramProbeU32(rdram, list + 0x08u);
            const uint32_t entries = readRdramProbeU32(rdram, list + 0x10u);
            const uint32_t item = GPR_U32(ctx, 4);
            const uint32_t package = g_xmenMainBackIgbPackage.load(std::memory_order_relaxed);
            const uint32_t objectEntries =
                g_xmenMainBackIgbObjectEntries.load(std::memory_order_relaxed);
            int32_t packageIndex = -1;
            if (objectEntries != 0u)
            {
                for (uint32_t index = 0u; index < 435u; ++index)
                {
                    if (readRdramProbeU32(rdram, objectEntries + index * 4u) == item)
                    {
                        packageIndex = static_cast<int32_t>(index);
                        break;
                    }
                }
            }
            std::cout << "[xmen-title-object-list-lookup] index=" << traceIndex
                      << " tick=" << xmenBranchTick
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " owner=0x" << owner
                      << " list=0x" << list
                      << " count=0x" << count
                      << " entries=0x" << entries
                      << " item=0x" << item
                      << " itemVtable=0x" << readRdramProbeU32(rdram, item)
                      << " search=0x" << GPR_U32(ctx, 18)
                      << " searchText=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 18), 64u) << "\""
                      << " package=0x" << package
                      << " objectEntries=0x" << objectEntries
                      << " packageIndex=" << std::dec << packageIndex
                      << " s0=0x" << std::hex << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (targetPc == 0x00213A80u && xmenBranchTick >= 1800u)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t trackedCount = std::min<uint32_t>(
            s_xmenTrackedTextureCount.load(std::memory_order_relaxed),
            static_cast<uint32_t>(s_xmenTrackedTextureObjects.size()));
        for (uint32_t slot = 0u; slot < trackedCount; ++slot)
        {
            if (s_xmenTrackedTextureObjects[slot].load(std::memory_order_relaxed) == object)
            {
                std::cout << "[xmen-texture-release] tick=" << xmenBranchTick
                          << " slot=" << slot
                          << " source=0x" << std::hex << sourcePc
                          << " object=0x" << object
                          << " vtable=0x" << readRdramProbeU32(rdram, object)
                          << " refcount=0x" << readRdramProbeU32(rdram, object + 0x04u)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
                break;
            }
        }
        if (readRdramProbeU32(rdram, object) < 0x00100000u)
        {
            static std::atomic<uint32_t> s_xmenMalformedObjectReleaseCount{0u};
            const uint32_t traceIndex =
                s_xmenMalformedObjectReleaseCount.fetch_add(1u, std::memory_order_relaxed);
            if (traceIndex < 64u)
            {
                std::cout << "[xmen-title-malformed-object-release] index=" << traceIndex
                          << " tick=" << xmenBranchTick
                          << " source=0x" << std::hex << sourcePc
                          << " object=0x" << object
                          << " word0=0x" << readRdramProbeU32(rdram, object)
                          << " word4=0x" << readRdramProbeU32(rdram, object + 4u)
                          << " word8=0x" << readRdramProbeU32(rdram, object + 8u)
                          << " wordc=0x" << readRdramProbeU32(rdram, object + 12u)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            }
        }
    }
    if (g_xmenTitleBranchTraceArmed.load(std::memory_order_acquire) &&
        (kind == GuestBranchKind::IndirectCall || kind == GuestBranchKind::IndirectJump) &&
        (targetPc == 0u || !m_memory.isCodeAddress(targetPc) || !hasFunction(targetPc)))
    {
        const uint32_t traceIndex =
            g_xmenTitleBranchTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 16384u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const bool code = m_memory.isCodeAddress(targetPc);
            const bool registered = hasFunction(targetPc);
            std::cout << "[xmen-title-indirect] index=" << traceIndex
                      << " tick=" << xmenBranchTick
                      << " thread=" << eeScheduler().currentThreadId()
                      << " kind=" << describeGuestBranchKind(kind)
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " registered=" << std::dec << (registered ? 1u : 0u)
                      << " code=" << (code ? 1u : 0u)
                      << " a0=0x" << std::hex << object
                      << " vtable=0x" << readRdramProbeU32(rdram, object)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec;
            if (sourcePc == 0x004B3CE0u || sourcePc == 0x004B3DF0u)
            {
                const uint32_t parent = sourcePc == 0x004B3DF0u
                    ? GPR_U32(ctx, 17)
                    : 0u;
                std::cout << " s0=0x" << std::hex << GPR_U32(ctx, 16)
                          << " s1=0x" << GPR_U32(ctx, 17)
                          << " s2=0x" << GPR_U32(ctx, 18)
                          << " s3=0x" << GPR_U32(ctx, 19)
                          << " parent=0x" << parent;
                for (uint32_t offset = 0u; offset < 0x40u; offset += sizeof(uint32_t))
                {
                    std::cout << " obj" << std::dec << offset
                              << "=0x" << std::hex
                              << readRdramProbeU32(rdram, object + offset);
                }
                if (parent != 0u)
                {
                    for (uint32_t offset = 0u; offset < 0x40u; offset += sizeof(uint32_t))
                    {
                        std::cout << " parent" << std::dec << offset
                                  << "=0x" << std::hex
                                  << readRdramProbeU32(rdram, parent + offset);
                    }
                }
            }
            std::cout << std::dec << std::endl;
        }
    }
    if (kind == GuestBranchKind::IndirectCall && sourcePc == 0x00344FE4u)
    {
        static std::atomic<uint32_t> s_xmenInterfaceTableTraceCount{0u};
        const uint32_t count = s_xmenInterfaceTableTraceCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (count < 8u)
        {
            const uint32_t owner = GPR_U32(ctx, 16);
            const uint32_t tableBase = readRdramProbeU32(rdram, owner);
            std::cerr << "[xmen-interface-table] index=" << std::dec << count
                      << " owner=0x" << std::hex << owner
                      << " tableBase=0x" << tableBase
                      << " selector=0x" << GPR_U32(ctx, 5)
                      << " selected=0x" << targetPc;
            for (uint32_t slot = 0u; slot < 32u; ++slot)
            {
                std::cerr << " slot" << std::dec << slot << "=0x" << std::hex
                          << readRdramProbeU32(rdram, tableBase + 0x2B0u + slot * 0x10u);
            }
            std::cerr << std::dec << std::endl;
        }
    }
    if (kind == GuestBranchKind::IndirectCall && sourcePc == 0x00567EE8u)
    {
        static std::atomic<uint32_t> s_xmenCallbackMatrixTraceCount{0u};
        const uint32_t count = s_xmenCallbackMatrixTraceCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (count < 4u)
        {
            const uint32_t nextRow = GPR_U32(ctx, 17);
            const uint32_t tableCursor = GPR_U32(ctx, 16);
            const uint32_t tableBase = tableCursor - nextRow * sizeof(uint32_t);
            const uint32_t selector = GPR_U32(ctx, 18) / sizeof(uint32_t);
            std::cerr << "[xmen-callback-matrix] index=" << std::dec << count
                      << " tableBase=0x" << std::hex << tableBase
                      << " selector=0x" << selector
                      << " selected=0x" << targetPc;
            for (uint32_t row = 0u; row < 15u; ++row)
            {
                const uint32_t record = readRdramProbeU32(
                    rdram, tableBase + row * sizeof(uint32_t));
                std::cerr << " row" << std::dec << row << "=0x" << std::hex << record;
                for (uint32_t slot = 0u; slot < 16u; ++slot)
                {
                    const uint32_t candidate = readRdramProbeU32(
                        rdram, record + slot * sizeof(uint32_t));
                    if (m_memory.isCodeAddress(candidate))
                    {
                        std::cerr << " r" << std::dec << row << 's' << slot
                                  << "=0x" << std::hex << candidate;
                    }
                }
            }
            std::cerr << std::dec << std::endl;
        }
    }
    if ((sourcePc == 0x00521460u && targetPc == 0x00325AE0u) ||
        sourcePc == 0x00325890u || sourcePc == 0x003258F8u ||
        targetPc == 0x00158300u)
    {
        static std::atomic<uint32_t> s_xmenLayerHandoffTraceCount{0u};
        const uint32_t count = s_xmenLayerHandoffTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            std::cerr << "[xmen-layer-handoff] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << std::dec << std::endl;
        }
    }
    static std::atomic<uint32_t> s_xmenPostLegalInflateTraceBudget{0u};
    static std::atomic<uint32_t> s_xmenPostLegalInflateTraceIndex{0u};
    static std::atomic<uint32_t> s_xmenOverlapClassSize{0u};
    static std::atomic<uint32_t> s_xmenOverlapClassSizeLogCount{0u};
    const uint32_t overlapClassSize = readRdramProbeU32(rdram, 0x009E6F08u);
    const uint32_t previousOverlapClassSize = s_xmenOverlapClassSize.exchange(
        overlapClassSize, std::memory_order_relaxed);
    if (overlapClassSize != previousOverlapClassSize &&
        (overlapClassSize != 0u || previousOverlapClassSize != 0u) &&
        s_xmenOverlapClassSizeLogCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
    {
        const uint32_t classNameAddress = readRdramProbeU32(rdram, 0x009E6EDCu);
        std::cerr << "[xmen-overlap-class-size-change] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " old=0x" << previousOverlapClassSize
                  << " value=0x" << overlapClassSize
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s1=0x" << GPR_U32(ctx, 17)
                  << " nameAddress=0x" << classNameAddress
                  << " name=\"" << readGuestPrintableString(rdram, classNameAddress, 96u) << "\""
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    static std::atomic<uint32_t> s_xmenLegalManagerVtable{0u};
    static std::atomic<uint32_t> s_xmenLegalManagerVtableLogCount{0u};
    const uint32_t legalManagerVtable = readRdramProbeU32(rdram, 0x00B70060u);
    uint32_t previousLegalManagerVtable = s_xmenLegalManagerVtable.load(std::memory_order_relaxed);
    if (legalManagerVtable == 0x00708C00u || previousLegalManagerVtable != 0u)
    {
        previousLegalManagerVtable = s_xmenLegalManagerVtable.exchange(
            legalManagerVtable, std::memory_order_relaxed);
        if (previousLegalManagerVtable != 0u && legalManagerVtable != previousLegalManagerVtable &&
            s_xmenLegalManagerVtableLogCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cerr << "[xmen-legal-manager-vtable-change] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " old=0x" << previousLegalManagerVtable
                      << " value=0x" << legalManagerVtable
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    logXmenVtableTripwire(rdram, ctx, "enter", sourcePc, targetPc, kind);
    constexpr uint32_t kXmenTextureFreeList = 0x00753938u;
    static std::atomic<uint32_t> s_xmenTextureFreeListHead{0u};
    const uint32_t textureFreeListHead = readRdramProbeU32(rdram, kXmenTextureFreeList);
    const uint32_t previousTextureFreeListHead =
        s_xmenTextureFreeListHead.exchange(textureFreeListHead, std::memory_order_relaxed);
    if (textureFreeListHead == 0xFFFFFFFFu && previousTextureFreeListHead != 0xFFFFFFFFu)
    {
        std::cerr << "[xmen-texture-freelist:transition] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " previous=0x" << previousTextureFreeListHead
                  << " head=0x" << textureFreeListHead
                  << " kind=" << describeGuestBranchKind(kind)
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (targetPc == 0x002F7020u && GPR_U32(ctx, 4) == 0xFFFFFFFFu)
    {
        std::cerr << "[xmen-texture-freelist:invalid-free] source=0x" << std::hex << sourcePc
                  << " kind=" << describeGuestBranchKind(kind)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (rdram && kind == GuestBranchKind::Return && sourcePc == 0x002FA54Cu &&
        GPR_U32(ctx, 2) == 0xFFFFFFFFu)
    {
        const uint32_t outputAddress = GPR_U32(ctx, 5);
        const uint32_t staleOutputIndex = readRdramProbeU32(rdram, outputAddress);
        if (staleOutputIndex != 0xFFFFFFFFu)
        {
            constexpr uint32_t notFound = 0xFFFFFFFFu;
            std::memcpy(rdram + (outputAddress & PS2_RAM_MASK), &notFound, sizeof(notFound));
        }
        const uint32_t firstHead = readRdramProbeU32(rdram, 0x00753948u);
        const uint32_t secondHead = readRdramProbeU32(rdram, 0x00753940u);
        const GuestThread *owner = m_eeScheduler ? m_eeScheduler->currentThread() : nullptr;
        const R5900Context *active = owner ? &owner->activeContext() : nullptr;
        const auto traceNode = [&](std::ostream &stream, const char *name, uint32_t node)
        {
            stream << " " << name << "=0x" << node;
            if (node != 0u && node != 0xFFFFFFFFu)
            {
                stream << "{next=0x" << readRdramProbeU32(rdram, node + 0x18u)
                       << ",previous=0x" << readRdramProbeU32(rdram, node + 0x1Cu)
                       << ",offset=0x" << readRdramProbeU32(rdram, node + 0x38u)
                       << ",extent=0x" << readRdramProbeU16(rdram, node + 0x40u)
                       << ",type=0x" << static_cast<uint32_t>(rdram[(node + 0x46u) & PS2_RAM_MASK])
                       << ",state=0x" << static_cast<uint32_t>(rdram[(node + 0x48u) & PS2_RAM_MASK])
                       << "}";
            }
        };
        std::cerr << "[xmen-texture-lookup:invalid-result] return=0x" << std::hex << targetPc
                  << " query=0x" << GPR_U32(ctx, 4)
                  << " output=0x" << outputAddress
                  << " staleIndex=0x" << staleOutputIndex
                  << " repairedIndex=0x" << readRdramProbeU32(rdram, outputAddress)
                  << " mode=0x" << GPR_U32(ctx, 6)
                  << " extentOut=0x" << GPR_U32(ctx, 7)
                  << " thread=" << std::dec << (owner ? owner->id : 0)
                  << " entry=0x" << std::hex << (owner ? owner->entry : 0u)
                  << " priority=" << std::dec << (owner ? owner->currentPriority : -1)
                  << " depth=" << (owner ? owner->invocations.size() : 0u)
                  << " status=0x" << std::hex << ctx->cop0_status
                  << " activePc=0x" << (active ? active->pc : 0u)
                  << " basePc=0x" << (owner ? owner->context.pc : 0u)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " trace=" << formatDispatchHistory();
        traceNode(std::cerr, "first", firstHead);
        traceNode(std::cerr, "firstContainer", firstHead - 0x20u);
        traceNode(std::cerr, "second", secondHead);
        std::cerr << std::dec << std::endl;
    }
    if (isCall && targetPc == 0x002F9FF0u && GPR_U32(ctx, 5) == 0xFFFFFFFFu)
    {
        const uint32_t stack = GPR_U32(ctx, 29);
        std::cerr << "[xmen-texture-remove:invalid-object] source=0x" << std::hex << sourcePc
                  << " owner=0x" << GPR_U32(ctx, 4)
                  << " object=0x" << GPR_U32(ctx, 5)
                  << " index=0x" << GPR_U32(ctx, 6)
                  << " stackIndex=0x" << readRdramProbeU32(rdram, stack + 0x3Cu)
                  << " firstHead=0x" << readRdramProbeU32(rdram, 0x00753948u)
                  << " secondHead=0x" << readRdramProbeU32(rdram, 0x00753940u)
                  << " sp=0x" << stack
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (isCall && targetPc == 0x002F6F80u)
    {
        constexpr uint32_t kTextureFreeList = 0x00753938u;
        const uint32_t head = readRdramProbeU32(rdram, kTextureFreeList);
        const uint32_t next = head != 0u && head != 0xFFFFFFFFu
                                  ? readRdramProbeU32(rdram, head + 0x18u)
                                  : 0u;
        if (head == 0xFFFFFFFFu || next == 0xFFFFFFFFu)
        {
            std::cerr << "[xmen-texture-freelist:poison] source=0x" << std::hex << sourcePc
                      << " head=0x" << head
                      << " next=0x" << next
                      << " head0=0x" << readRdramProbeU32(rdram, head)
                      << " head8=0x" << readRdramProbeU32(rdram, head + 8u)
                      << " head18=0x" << readRdramProbeU32(rdram, head + 0x18u)
                      << " head1c=0x" << readRdramProbeU32(rdram, head + 0x1Cu)
                      << " head30=0x" << readRdramProbeU32(rdram, head + 0x30u)
                      << " head34=0x" << readRdramProbeU32(rdram, head + 0x34u)
                      << " head38=0x" << readRdramProbeU32(rdram, head + 0x38u)
                      << " head3c=0x" << readRdramProbeU32(rdram, head + 0x3Cu)
                      << " head40=0x" << readRdramProbeU32(rdram, head + 0x40u)
                      << " head44=0x" << readRdramProbeU32(rdram, head + 0x44u)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x002F8560u)
    {
        static std::atomic<uint32_t> s_xmenTextureUploadEntryCount{0u};
        const uint32_t index =
            s_xmenTextureUploadEntryCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t upload = GPR_U32(ctx, 4);
        const uint32_t descriptor = readRdramProbeU32(rdram, upload);
        const uint32_t width = readRdramProbeU16(rdram, descriptor + 0x3Cu);
        const uint32_t height = readRdramProbeU16(rdram, descriptor + 0x3Eu);
        const uint32_t format = readRdramProbeU16(rdram, descriptor + 0x42u);
        const uint32_t multiplier =
            readRdramProbeU32(rdram, 0x0065A520u + (format * 4u));
        const uint32_t rowBytes = width * multiplier;
        const uint32_t chunkRows = rowBytes != 0u ? 0x000FFFE0u / rowBytes
                                                   : 0xFFFFFFFFu;
        if (index < 64u || chunkRows == 0u)
        {
            const uint32_t manager = readRdramProbeU32(rdram, 0x00750778u);
            std::cerr << "[xmen-texture-upload:entry] index=" << std::dec << index
                      << " source=0x" << std::hex << sourcePc
                      << " upload=0x" << upload
                      << " descriptor=0x" << descriptor
                      << " data=0x" << readRdramProbeU32(rdram, descriptor + 0x30u)
                      << " destination=0x" << readRdramProbeU32(rdram, descriptor + 0x38u)
                      << " width=0x" << width
                      << " height=0x" << height
                      << " format=0x" << format
                      << " multiplier=0x" << multiplier
                      << " rowBytes=0x" << rowBytes
                      << " chunkRows=0x" << chunkRows
                      << " manager=0x" << manager
                      << " packet=0x" << readRdramProbeU32(rdram, manager + 0x30u)
                      << " packetEnd=0x" << readRdramProbeU32(rdram, manager + 0x28u)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
            if (descriptor == 0xFFFFFFFFu)
            {
                const uint32_t list = GPR_U32(ctx, 19);
                const uint32_t sourceSlot = GPR_U32(ctx, 21);
                std::cerr << "[xmen-texture-upload:invalid-source] list=0x" << std::hex
                          << list
                          << " list0=0x" << readRdramProbeU32(rdram, list)
                          << " list1=0x" << readRdramProbeU32(rdram, list + 4u)
                          << " list2=0x" << readRdramProbeU32(rdram, list + 8u)
                          << " list3=0x" << readRdramProbeU32(rdram, list + 12u)
                          << " sourceSlot=0x" << sourceSlot
                          << " source=0x" << readRdramProbeU32(rdram, sourceSlot)
                          << " listCount=0x" << GPR_U32(ctx, 17)
                          << " selectedCount=0x" << GPR_U32(ctx, 18)
                          << " iterator=0x" << GPR_U32(ctx, 22)
                          << " mode=0x" << GPR_U32(ctx, 20)
                          << " option=0x" << GPR_U32(ctx, 30)
                          << " slot0=0x" << readRdramProbeU32(rdram, upload)
                          << " slot1=0x" << readRdramProbeU32(rdram, upload + 4u)
                          << " slot2=0x" << readRdramProbeU32(rdram, upload + 8u)
                          << " slot3=0x" << readRdramProbeU32(rdram, upload + 12u)
                          << std::dec << std::endl;
            }
        }
    }
    const bool traceXmenChainBuilderCall = isCall && targetPc == 0x002E6C60u;
    if (traceXmenChainBuilderCall && sourcePc == 0x002F8864u)
    {
        static std::atomic<uint32_t> s_xmenTextureChunkCount{0u};
        static std::atomic<uint32_t> s_xmenZeroTextureChunkCount{0u};
        const uint32_t index =
            s_xmenTextureChunkCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t chunk = GPR_U32(ctx, 16);
        const uint32_t zeroIndex = chunk == 0u
                                       ? s_xmenZeroTextureChunkCount.fetch_add(
                                             1u, std::memory_order_relaxed)
                                       : 0u;
        if (index < 256u || (chunk == 0u && zeroIndex < 32u))
        {
            const uint32_t manager = readRdramProbeU32(rdram, 0x00750778u);
            const uint32_t upload = GPR_U32(ctx, 22);
            const uint32_t descriptor = readRdramProbeU32(rdram, upload);
            const uint32_t width = readRdramProbeU16(rdram, descriptor + 0x3Cu);
            const uint32_t height = readRdramProbeU16(rdram, descriptor + 0x3Eu);
            const uint32_t format = readRdramProbeU16(rdram, descriptor + 0x42u);
            const uint32_t multiplier =
                readRdramProbeU32(rdram, 0x0065A520u + (format * 4u));
            std::cerr << "[xmen-texture-upload:chunk] index=" << std::dec << index
                      << " zeroIndex=" << zeroIndex
                      << " chunk=0x" << std::hex << chunk
                      << " maxChunk=0x" << GPR_U32(ctx, 18)
                      << " remaining=0x" << GPR_U32(ctx, 20)
                      << " rowOffset=0x" << GPR_U32(ctx, 19)
                      << " upload=0x" << upload
                      << " descriptor=0x" << descriptor
                      << " data=0x" << readRdramProbeU32(rdram, descriptor + 0x30u)
                      << " width=0x" << width
                      << " height=0x" << height
                      << " format=0x" << format
                      << " multiplier=0x" << multiplier
                      << " rowBytes=0x" << (width * multiplier)
                      << " manager=0x" << manager
                      << " packet=0x" << readRdramProbeU32(rdram, manager + 0x30u)
                      << " packetEnd=0x" << readRdramProbeU32(rdram, manager + 0x28u)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    const bool traceXmenChainBuilderReturn =
        kind == GuestBranchKind::Return && sourcePc == 0x002E6E14u;
    static std::atomic<uint32_t> xmenChainDispatchLogCount{0u};
    const bool logXmenChainDispatch =
        (traceXmenChainBuilderCall || traceXmenChainBuilderReturn) &&
        xmenChainDispatchLogCount.fetch_add(1u, std::memory_order_relaxed) < 32u;
    const uint32_t xmenChainCallerSp = GPR_U32(ctx, 29);
    const uint32_t xmenChainSavedReturnSlot = xmenChainCallerSp - 0x20u;
    if (logXmenChainDispatch)
    {
        const GuestThread *owner = m_eeScheduler ? m_eeScheduler->currentThread() : nullptr;
        const R5900Context *active = owner ? &owner->activeContext() : nullptr;
        std::cerr << "[xmen-chain-dispatch:enter] thread=" << std::dec
                  << (owner ? owner->id : 0)
                  << " invocations=" << (owner ? owner->invocations.size() : 0u)
                  << " same-context=" << (active == ctx ? 1 : 0)
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " fallthrough=0x" << fallthroughPc
                  << " kind=" << describeGuestBranchKind(kind)
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " sp=0x" << xmenChainCallerSp
                  << " slot=0x" << xmenChainSavedReturnSlot
                  << " slotValue=0x" << readRdramProbeU64(rdram, xmenChainSavedReturnSlot)
                  << " basePc=0x" << (owner ? owner->context.pc : 0u)
                  << " baseRa=0x" << (owner ? getRegU32(&owner->context, 31) : 0u)
                  << " baseSp=0x" << (owner ? getRegU32(&owner->context, 29) : 0u)
                  << std::dec << std::endl;
    }
    constexpr uint32_t xmenFirstMovieRecord = 0x00666DE8u;
    constexpr uint32_t xmenMovieRecordStride = 0x210u;
    constexpr uint32_t xmenMovieRecordCount = 8u;
    const uint32_t xmenPotentialMovieRecord = GPR_U32(ctx, 4);
    const uint32_t xmenPotentialMovieRecordOffset =
        xmenPotentialMovieRecord - xmenFirstMovieRecord;
    const bool isXmenMovieRecordVirtualCall =
        kind == GuestBranchKind::IndirectCall &&
        xmenPotentialMovieRecord >= xmenFirstMovieRecord &&
        xmenPotentialMovieRecordOffset < xmenMovieRecordStride * xmenMovieRecordCount &&
        xmenPotentialMovieRecordOffset % xmenMovieRecordStride == 0u;
    if (isXmenMovieRecordVirtualCall)
    {
        static std::atomic<uint32_t> s_xmenMovieRecordVirtualCallCount{0u};
        const uint32_t index = s_xmenMovieRecordVirtualCallCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (index < 128u)
        {
            const uint32_t owner = readRdramProbeU32(
                rdram, xmenPotentialMovieRecord + 0x40u);
            std::cerr << "[xmen-movie-record-vcall] index=" << std::dec << index
                      << " thread=" << eeScheduler().currentThreadId()
                      << " recordIndex="
                      << xmenPotentialMovieRecordOffset / xmenMovieRecordStride
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " vtable=0x" << readRdramProbeU32(rdram, xmenPotentialMovieRecord)
                      << " a0=0x" << xmenPotentialMovieRecord
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " state=0x" << readRdramProbeU32(rdram, xmenPotentialMovieRecord + 4u)
                      << " controls=0x" << readRdramProbeU32(rdram, xmenPotentialMovieRecord + 0x70u)
                      << " owner=0x" << owner
                      << " owner44=0x" << readRdramProbeU32(rdram, owner + 0x44u)
                      << " owner48=0x" << readRdramProbeU32(rdram, owner + 0x48u)
                      << " owner4C=0x" << readRdramProbeU32(rdram, owner + 0x4Cu)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieOwnerLifecycleCall =
        isCall && targetPc >= 0x005947B0u && targetPc <= 0x00594D90u;
    if (isXmenMovieOwnerLifecycleCall)
    {
        static std::atomic<uint32_t> s_xmenMovieOwnerLifecycleCallCount{0u};
        const uint32_t index = s_xmenMovieOwnerLifecycleCallCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (index < 128u)
        {
            const uint32_t argument = GPR_U32(ctx, 5);
            const uint32_t wrapper = readRdramProbeU32(rdram, argument + 0x64u);
            const uint32_t record = readRdramProbeU32(rdram, wrapper + 8u);
            std::cerr << "[xmen-movie-owner-lifecycle] index=" << std::dec << index
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << argument
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " wrapper=0x" << wrapper
                      << " record=0x" << record
                      << " recordState=0x" << readRdramProbeU32(rdram, record + 4u)
                      << " controls=0x" << readRdramProbeU32(rdram, record + 0x70u)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieRecordConstructDispatch =
        isCall &&
        (targetPc == 0x0054C958u ||
         (sourcePc >= 0x0054C958u && sourcePc <= 0x0054CC98u));
    if (isXmenMovieRecordConstructDispatch)
    {
        static std::atomic<uint32_t> s_xmenMovieRecordConstructLogCount{0u};
        const uint32_t logIndex = s_xmenMovieRecordConstructLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 96u)
        {
            const uint32_t record = targetPc == 0x0054C958u ? 0u : GPR_U32(ctx, 16);
            std::cerr << "[xmen-movie-record-construct] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " recordState=0x" << (record ? readRdramProbeU32(rdram, record + 0x04u) : 0u)
                      << " backend=0x" << (record ? readRdramProbeU32(rdram, record + 0x40u) : 0u)
                      << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieBufferCreateTrace =
        sourcePc == 0x0054CC30u ||
        sourcePc == 0x00550C04u ||
        sourcePc == 0x0056D8F0u ||
        sourcePc == 0x0056D904u ||
        sourcePc == 0x0056D90Cu ||
        sourcePc == 0x0056D920u ||
        sourcePc == 0x0056D930u ||
        sourcePc == 0x0056D940u ||
        sourcePc == 0x0056D948u ||
        sourcePc == 0x0056D95Cu ||
        sourcePc == 0x0056D964u ||
        sourcePc == 0x0056D984u ||
        sourcePc == 0x0056D98Cu ||
        sourcePc == 0x0056D994u ||
        sourcePc == 0x0056D9D4u;
    if (isXmenMovieBufferCreateTrace)
    {
        static std::atomic<uint32_t> s_xmenMovieBufferCreateLogCount{0u};
        const uint32_t logIndex = s_xmenMovieBufferCreateLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 96u)
        {
            constexpr uint32_t poolBase = 0x006681F0u;
            constexpr uint32_t firstSlot = poolBase + 0x18u;
            constexpr uint32_t slotStride = 0x90u;
            std::cerr << "[xmen-movie-buffer-create] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << static_cast<uint32_t>(kind)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " poolActive=0x" << readRdramProbeU32(rdram, poolBase)
                      << " poolCapacity=0x" << readRdramProbeU32(rdram, poolBase + 0x04u);
            for (uint32_t slotIndex = 0; slotIndex < 8u; ++slotIndex)
            {
                std::cerr << " slot" << std::dec << slotIndex << "=0x" << std::hex
                          << readRdramProbeU32(
                                 rdram, firstSlot + slotIndex * slotStride);
            }
            std::cerr << std::dec << std::endl;
        }
    }
    const bool isXmenMoviePoolInitTrace =
        sourcePc == 0x0052A0CCu ||
        sourcePc == 0x00593E7Cu ||
        sourcePc == 0x0054E5ACu ||
        sourcePc == 0x0054E5D4u ||
        sourcePc == 0x0054E6A0u ||
        sourcePc == 0x0054E6CCu ||
        sourcePc == 0x00550BA0u ||
        sourcePc == 0x0056D7A4u ||
        sourcePc == 0x0056D7B4u ||
        sourcePc == 0x0056D824u ||
        sourcePc == 0x0056D834u ||
        sourcePc == 0x0056D84Cu;
    if (isXmenMoviePoolInitTrace)
    {
        static std::atomic<uint32_t> s_xmenMoviePoolInitLogCount{0u};
        const uint32_t logIndex = s_xmenMoviePoolInitLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 48u)
        {
            std::cerr << "[xmen-movie-pool-init] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << static_cast<uint32_t>(kind)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " v1=0x" << GPR_U32(ctx, 3)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " refCount=0x" << readRdramProbeU32(rdram, 0x00666D78u)
                      << " poolActive=0x" << readRdramProbeU32(rdram, 0x006681F0u)
                      << " poolCapacity=0x" << readRdramProbeU32(rdram, 0x006681F4u)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenLevelLoadEntryCall = isCall && targetPc == 0x00320250u;
    const uint32_t xmenLevelLoadManager =
        isXmenLevelLoadEntryCall ? GPR_U32(ctx, 4) : 0u;
    const bool isXmenLevelObjectLookupCall =
        isCall && (sourcePc == 0x003203D4u || sourcePc == 0x003203E8u);
    if (isXmenLevelLoadEntryCall)
    {
        std::cout << "[xmen-level-load:enter] source=0x" << std::hex << sourcePc
                  << " manager=0x" << GPR_U32(ctx, 4)
                  << " pathA=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 160u) << "\""
                  << " pathB=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 6), 160u) << "\""
                  << " active=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 4) + 0x0C08u)
                  << std::dec << std::endl;
    }
    if (isXmenLevelObjectLookupCall)
    {
        std::cout << "[xmen-level-object-lookup:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " owner=0x" << GPR_U32(ctx, 4)
                  << " path=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 160u) << "\""
                  << std::dec << std::endl;
    }
    const bool isXmenMovieCallbackGateCall =
        isCall &&
        (targetPc == 0x00554028u ||
         (sourcePc >= 0x00554040u && sourcePc <= 0x005542BCu));
    const bool isXmenMovieRecordServiceCall =
        isCall && targetPc == 0x005542E0u;
    if (isXmenMovieRecordServiceCall)
    {
        static std::atomic<uint32_t> s_xmenMovieRecordServiceLogCount{0u};
        const uint32_t logIndex = s_xmenMovieRecordServiceLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 64u)
        {
            const uint32_t record = GPR_U32(ctx, 4);
            constexpr uint32_t firstRecord = 0x00666DE8u;
            constexpr uint32_t recordStride = 0x210u;
            const uint32_t recordOffset = record - firstRecord;
            const uint32_t recordIndex =
                record >= firstRecord && recordOffset % recordStride == 0u
                    ? recordOffset / recordStride
                    : UINT32_MAX;
            std::cerr << "[xmen-movie-record:enter] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " recordIndex=" << recordIndex
                      << " source=0x" << std::hex << sourcePc
                      << " record=0x" << record
                      << " state=0x" << readRdramProbeU32(rdram, record + 0x04u)
                      << " subsystem=0x" << readRdramProbeU32(rdram, record + 0x40u)
                      << " busy=0x" << readRdramProbeU32(rdram, record + 0x60u)
                      << " serving=0x" << readRdramProbeU32(rdram, record + 0x64u)
                      << " pending=0x" << readRdramProbeU32(rdram, record + 0x68u)
                      << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                      << " active=0x" << readRdramProbeU32(rdram, 0x0066DCF4u)
                      << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieManagerBusySet =
        isCall && targetPc == 0x00554530u &&
        (sourcePc == 0x00553AB4u || sourcePc == 0x00553AC8u ||
         sourcePc == 0x005545D0u || sourcePc == 0x005545E4u);
    if (isXmenMovieManagerBusySet)
    {
        static std::atomic<uint32_t> s_xmenMovieManagerBusySetLogCount{0u};
        const uint32_t logIndex = s_xmenMovieManagerBusySetLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 64u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-movie-manager-busy:set] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " object=0x" << object
                      << " value=0x" << GPR_U32(ctx, 5)
                      << " object5C=0x" << readRdramProbeU32(rdram, object + 0x5Cu)
                      << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieManagerWait =
        isCall && sourcePc == 0x00553ABCu && targetPc == 0x0057A608u;
    if (isXmenMovieManagerWait)
    {
        static std::atomic<uint32_t> s_xmenMovieManagerWaitLogCount{0u};
        const uint32_t logIndex = s_xmenMovieManagerWaitLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 32u)
        {
            std::cerr << "[xmen-movie-manager-busy:wait] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieServiceHandoff =
        isCall && (targetPc == 0x00554380u || targetPc == 0x00562D18u);
    if (isXmenMovieServiceHandoff)
    {
        static std::atomic<uint32_t> s_xmenMovieServiceHandoffLogCount{0u};
        const uint32_t logIndex = s_xmenMovieServiceHandoffLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 32u)
        {
            std::cerr << "[xmen-movie-service-handoff] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieDecoderStage =
        targetPc == 0x00559FC0u || targetPc == 0x00562DC8u ||
        targetPc == 0x00562EE0u || targetPc == 0x00562F08u ||
        targetPc == 0x00568480u || sourcePc == 0x00562D34u ||
        sourcePc == 0x00562DECu || sourcePc == 0x00562DF8u;
    if (isXmenMovieDecoderStage)
    {
        static std::atomic<uint32_t> s_xmenMovieDecoderStageLogCount{0u};
        const uint32_t logIndex = s_xmenMovieDecoderStageLogCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (logIndex < 64u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-movie-decoder-stage] index=" << std::dec << logIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=0x" << static_cast<uint32_t>(kind)
                      << " object=0x" << object
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " state44=0x" << readRdramProbeU32(rdram, object + 0x44u)
                      << " state48=0x" << readRdramProbeU32(rdram, object + 0x48u)
                      << " state4C=0x" << readRdramProbeU32(rdram, object + 0x4Cu)
                      << " activeRecord=0x" << readRdramProbeU32(rdram, 0x00667E88u)
                      << std::dec << std::endl;
        }
    }
    uint32_t xmenMovieCallbackGateTraceIndex = UINT32_MAX;
    if (isXmenMovieCallbackGateCall)
    {
        static std::atomic<uint32_t> s_xmenMovieCallbackGateTraceCount{0u};
        xmenMovieCallbackGateTraceIndex = s_xmenMovieCallbackGateTraceCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (xmenMovieCallbackGateTraceIndex < 128u)
        {
            std::cerr << "[xmen-movie-callback:enter] index=" << std::dec
                      << xmenMovieCallbackGateTraceIndex
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " active=0x" << readRdramProbeU32(rdram, 0x0066DCF4u)
                      << " initialized=0x" << readRdramProbeU32(rdram, 0x00666D7Cu)
                      << " manager10=0x" << readRdramProbeU32(rdram, 0x00666D90u)
                      << " manager48=0x" << readRdramProbeU32(rdram, 0x00666DC8u)
                      << " manager4C=0x" << readRdramProbeU32(rdram, 0x00666DCCu)
                      << " manager54=0x" << readRdramProbeU32(rdram, 0x00666DD4u)
                      << " manager60=0x" << readRdramProbeU32(rdram, 0x00666DE0u)
                      << " manager64=0x" << readRdramProbeU32(rdram, 0x00666DE4u)
                      << " manager68=0x" << readRdramProbeU32(rdram, 0x00666DE8u)
                      << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                      << std::dec << std::endl;
        }
    }
    const std::array<uint32_t, 9> xmenMovieScheduleTargets = {
        0x00576420u, 0x00576440u, 0x0057A4C0u,
        0x00579B98u, 0x00591750u, 0x005786E0u,
        0x00578730u, 0x00577DF0u, 0x0057F488u,
    };
    const auto xmenMovieScheduleIt = std::find(
        xmenMovieScheduleTargets.begin(), xmenMovieScheduleTargets.end(), targetPc);
    const size_t xmenMovieScheduleIndex = static_cast<size_t>(
        xmenMovieScheduleIt - xmenMovieScheduleTargets.begin());
    const uint32_t xmenMovieStreamState = readRdramProbeU32(rdram, 0x006787F8u);
    const uint32_t xmenMoviePlaybackState = (xmenMovieStreamState >> 8u) & 0xFFu;
    const bool isXmenWorkerRegistration =
        isCall &&
        (targetPc == 0x00591440u ||
         targetPc == 0x00591510u ||
         targetPc == 0x00591588u);
    if (isXmenWorkerRegistration)
    {
        static std::atomic<uint32_t> s_xmenWorkerRegistrationTraceCount{0u};
        const uint32_t count = s_xmenWorkerRegistrationTraceCount.fetch_add(
            1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            std::cerr << "[xmen-worker-registration:enter] index=" << std::dec << count
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " operation=0x" << targetPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " stream0=0x" << xmenMovieStreamState
                      << std::dec << std::endl;
        }
    }
    constexpr std::array<uint32_t, 13> xmenMovieStateTargets = {
        0x0054F2B0u, 0x005545B8u, 0x005539F0u,
        0x005916A0u, 0x005790A0u, 0x00578FC8u,
        0x0054ED48u, 0x00557F80u, 0x00565BB8u,
        0x00552850u, 0x005526A8u, 0x005527F8u,
        0x00552CA0u,
    };
    const auto xmenMovieStateIt = std::find(
        xmenMovieStateTargets.begin(), xmenMovieStateTargets.end(), targetPc);
    size_t xmenMovieStateIndex = static_cast<size_t>(
        xmenMovieStateIt - xmenMovieStateTargets.begin());
    const bool isXmenMovieHighLevelVtableCall =
        isCall && sourcePc == 0x00594660u;
    if (isXmenMovieHighLevelVtableCall)
    {
        xmenMovieStateIndex = xmenMovieStateTargets.size();
    }
    const bool isXmenMovieStateCall =
        isCall && (xmenMovieStateIndex < xmenMovieStateTargets.size() ||
                   isXmenMovieHighLevelVtableCall);
    uint32_t xmenMovieStateTraceIndex = UINT32_MAX;
    uint32_t xmenMovieStateRecord = 0u;
    if (isXmenMovieStateCall)
    {
        static std::array<std::atomic<uint32_t>, 14> s_xmenMovieStateCounts{};
        xmenMovieStateTraceIndex =
            s_xmenMovieStateCounts[xmenMovieStateIndex].fetch_add(
                1u, std::memory_order_relaxed);
        if (targetPc == 0x00565BB8u)
        {
            xmenMovieStateRecord = sourcePc == 0x00553BD4u
                                       ? GPR_U32(ctx, 17)
                                       : sourcePc == 0x00553CC0u
                                             ? GPR_U32(ctx, 16)
                                             : 0u;
        }
        else if (targetPc == 0x00557F80u && sourcePc == 0x00553C50u)
        {
            xmenMovieStateRecord = GPR_U32(ctx, 17);
        }
        else if (targetPc == 0x005539F0u && sourcePc == 0x005545D8u)
        {
            xmenMovieStateRecord = GPR_U32(ctx, 16);
        }
        else if (targetPc == 0x0054F2B0u ||
                 targetPc == 0x005545B8u ||
                 targetPc == 0x0054ED48u ||
                 targetPc == 0x00552850u ||
                 targetPc == 0x005526A8u ||
                 targetPc == 0x005527F8u ||
                 targetPc == 0x00552CA0u ||
                 isXmenMovieHighLevelVtableCall)
        {
            xmenMovieStateRecord = GPR_U32(ctx, 4);
        }

        if (xmenMovieStateTraceIndex < 64u)
        {
            const uint32_t owner = xmenMovieStateRecord != 0u
                                       ? readRdramProbeU32(
                                             rdram, xmenMovieStateRecord + 0x40u)
                                       : 0u;
            std::cerr << "[xmen-movie-state:enter] targetIndex=" << std::dec
                      << xmenMovieStateIndex
                      << " call=" << xmenMovieStateTraceIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " record=0x" << xmenMovieStateRecord
                      << " control70=0x"
                      << (xmenMovieStateRecord != 0u
                              ? readRdramProbeU32(
                                    rdram, xmenMovieStateRecord + 0x70u)
                              : 0u)
                      << " owner=0x" << owner
                      << " owner44=0x" << readRdramProbeU32(rdram, owner + 0x44u)
                      << " owner48=0x" << readRdramProbeU32(rdram, owner + 0x48u)
                      << " owner4C=0x" << readRdramProbeU32(rdram, owner + 0x4Cu)
                      << " manager24=0x"
                      << readRdramProbeU32(rdram, 0x00666DA4u)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    const std::array<uint32_t, 7> xmenMoviePipelineSources = {
        0x0054F2CCu, 0x0054F2E0u, 0x0054F300u, 0x0054F328u,
        0x0054F340u, 0x0054F350u, 0x0054F35Cu,
    };
    const auto xmenMoviePipelineIt = std::find(
        xmenMoviePipelineSources.begin(), xmenMoviePipelineSources.end(), sourcePc);
    const size_t xmenMoviePipelineIndex = static_cast<size_t>(
        xmenMoviePipelineIt - xmenMoviePipelineSources.begin());
    const bool isXmenMoviePipelineCall =
        isCall && xmenMoviePipelineIndex < xmenMoviePipelineSources.size();
    uint32_t xmenMoviePipelineTraceIndex = UINT32_MAX;
    if (isXmenMoviePipelineCall)
    {
        static std::array<std::atomic<uint32_t>, 7> s_xmenMoviePipelineCounts{};
        xmenMoviePipelineTraceIndex =
            s_xmenMoviePipelineCounts[xmenMoviePipelineIndex].fetch_add(
                1u, std::memory_order_relaxed);
        if (xmenMoviePipelineTraceIndex < 64u)
        {
            const uint32_t record = GPR_U32(ctx, 16);
            const uint32_t owner = readRdramProbeU32(rdram, record + 0x40u);
            const uint32_t stack = GPR_U32(ctx, 29);
            std::cerr << "[xmen-movie-pipeline:enter] stage=" << std::dec
                      << xmenMoviePipelineIndex
                      << " call=" << xmenMoviePipelineTraceIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " record=0x" << record
                      << " recordState=0x" << readRdramProbeU32(rdram, record + 4u)
                      << " recordId=0x" << readRdramProbeU32(rdram, record + 0xCu)
                      << " recordMode=0x" << (readRdramProbeU32(rdram, record + 0x72u) & 0xFFu)
                      << " owner=0x" << owner
                      << " owner48=0x" << readRdramProbeU32(rdram, owner + 0x48u)
                      << " owner4C=0x" << readRdramProbeU32(rdram, owner + 0x4Cu)
                      << " sp0=0x" << readRdramProbeU32(rdram, stack)
                      << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenMovieWorkerDispatch =
        isCall && sourcePc == 0x005917B8u;
    uint32_t xmenMovieWorkerTraceIndex = UINT32_MAX;
    uint32_t xmenMovieWorkerCategory = UINT32_MAX;
    uint32_t xmenMovieWorkerSlot = UINT32_MAX;
    if (isXmenMovieWorkerDispatch)
    {
        constexpr uint32_t workerTableBase = 0x0086DFE0u;
        const uint32_t slotAddress = GPR_U32(ctx, 16);
        if (slotAddress >= workerTableBase)
        {
            const uint32_t slotOffset = slotAddress - workerTableBase;
            xmenMovieWorkerCategory = slotOffset / 0x30u;
            xmenMovieWorkerSlot = (slotOffset % 0x30u) / 8u;
        }

        static std::array<std::atomic<uint32_t>, 36> s_xmenMovieWorkerDispatchCounts{};
        const uint32_t countIndex =
            std::min<uint32_t>(xmenMovieWorkerCategory * 6u + xmenMovieWorkerSlot, 35u);
        xmenMovieWorkerTraceIndex =
            s_xmenMovieWorkerDispatchCounts[countIndex].fetch_add(1u, std::memory_order_relaxed);
        if (xmenMovieWorkerTraceIndex < 8u)
        {
            std::cerr << "[xmen-movie-worker:enter] thread=" << std::dec
                      << eeScheduler().currentThreadId()
                      << " category=" << xmenMovieWorkerCategory
                      << " slot=" << xmenMovieWorkerSlot
                      << " call=" << xmenMovieWorkerTraceIndex
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " context=0x" << GPR_U32(ctx, 4)
                      << " stream0=0x" << xmenMovieStreamState
                      << " streamC=0x" << readRdramProbeU32(rdram, 0x00678804u)
                      << " stream20=0x" << readRdramProbeU32(rdram, 0x00678818u)
                      << " stream24=0x" << readRdramProbeU32(rdram, 0x0067881Cu)
                      << " stream28=0x" << readRdramProbeU32(rdram, 0x00678820u)
                      << " stream34=0x" << readRdramProbeU32(rdram, 0x0067882Cu)
                      << " stream58=0x" << readRdramProbeU32(rdram, 0x00678850u)
                      << " wrapper0=0x" << readRdramProbeU32(rdram, 0x0066A168u)
                      << " wrapper14=0x" << readRdramProbeU32(rdram, 0x0066A17Cu)
                      << " wrapper20=0x" << readRdramProbeU32(rdram, 0x0066A188u)
                      << std::dec << std::endl;
        }
    }
    const std::array<uint32_t, 7> xmenMovieStreamCallbackSources = {
        0x0059099Cu, 0x00587CC0u, 0x00587D14u, 0x00587CACu,
        0x00587D40u, 0x00587D58u, 0x00587D8Cu,
    };
    const auto xmenMovieStreamCallbackIt = std::find(
        xmenMovieStreamCallbackSources.begin(), xmenMovieStreamCallbackSources.end(), sourcePc);
    const size_t xmenMovieStreamCallbackIndex = static_cast<size_t>(
        xmenMovieStreamCallbackIt - xmenMovieStreamCallbackSources.begin());
    const bool isXmenMovieStreamCallback =
        isCall && xmenMoviePlaybackState >= 2u &&
        xmenMovieStreamCallbackIndex < xmenMovieStreamCallbackSources.size();
    uint32_t xmenMovieStreamCallbackTraceIndex = UINT32_MAX;
    uint32_t xmenMovieStreamCallbackOutput = 0u;
    if (isXmenMovieStreamCallback)
    {
        static std::array<std::atomic<uint32_t>, 7> s_xmenMovieStreamCallbackCounts{};
        xmenMovieStreamCallbackTraceIndex =
            s_xmenMovieStreamCallbackCounts[xmenMovieStreamCallbackIndex].fetch_add(
                1u, std::memory_order_relaxed);
        xmenMovieStreamCallbackOutput = GPR_U32(ctx, 7);
        if (xmenMovieStreamCallbackTraceIndex < 128u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-movie-stream-callback:enter] sourceIndex=" << std::dec
                      << xmenMovieStreamCallbackIndex
                      << " call=" << xmenMovieStreamCallbackTraceIndex
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << object
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << xmenMovieStreamCallbackOutput
                      << " object0=0x" << readRdramProbeU32(rdram, object)
                      << " object4=0x" << readRdramProbeU32(rdram, object + 4u)
                      << " object8=0x" << readRdramProbeU32(rdram, object + 8u)
                      << " objectC=0x" << readRdramProbeU32(rdram, object + 0xCu)
                      << " object10=0x" << readRdramProbeU32(rdram, object + 0x10u)
                      << " out0=0x" << readRdramProbeU32(rdram, xmenMovieStreamCallbackOutput)
                      << " out4=0x" << readRdramProbeU32(rdram, xmenMovieStreamCallbackOutput + 4u)
                      << " stream0=0x" << xmenMovieStreamState
                      << std::dec << std::endl;
        }
    }
    if (isCall && xmenMovieScheduleIndex < xmenMovieScheduleTargets.size())
    {
        static std::array<std::atomic<uint32_t>, 9> s_xmenMovieScheduleCounts{};
        const uint32_t count = s_xmenMovieScheduleCounts[xmenMovieScheduleIndex].fetch_add(
            1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-movie-schedule] targetIndex=" << std::dec
                      << xmenMovieScheduleIndex
                      << " call=" << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " stream0=0x" << xmenMovieStreamState
                      << " stream44=0x" << readRdramProbeU32(rdram, 0x0067883Cu)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x0057F2B8u)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t state0 = readRdramProbeU32(rdram, object);
        const uint32_t state44 = readRdramProbeU32(rdram, object + 0x44u);
        const uint32_t state48 = readRdramProbeU32(rdram, object + 0x48u);
        const uint32_t handle = readRdramProbeU32(rdram, object + 0x08u);
        const uint32_t position = readRdramProbeU32(rdram, object + 0x0Cu);
        const uint32_t sectorBytes = readRdramProbeU32(rdram, object + 0x10u);
        const uint32_t sectorCount = readRdramProbeU32(rdram, object + 0x14u);
        const uint64_t signature = static_cast<uint64_t>(object) |
                                   (static_cast<uint64_t>(state0) << 24u) ^
                                   (static_cast<uint64_t>(state44) << 8u) ^
                                   (static_cast<uint64_t>(state48) << 32u) ^
                                   (static_cast<uint64_t>(handle) << 40u);
        static std::atomic<uint64_t> s_xmenMovieStateSignature{~uint64_t{0u}};
        static std::atomic<uint32_t> s_xmenMovieStateTraceCount{0u};
        const uint64_t previous = s_xmenMovieStateSignature.exchange(
            signature, std::memory_order_relaxed);
        const uint32_t count = s_xmenMovieStateTraceCount.load(std::memory_order_relaxed);
        if (signature != previous && count < 256u)
        {
            s_xmenMovieStateTraceCount.fetch_add(1u, std::memory_order_relaxed);
            std::cerr << "[xmen-movie-state] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " object=0x" << object
                      << " state0=0x" << state0
                      << " state44=0x" << state44
                      << " state48=0x" << state48
                      << " handle=0x" << handle
                      << " position=0x" << position
                      << " sectorBytes=0x" << sectorBytes
                      << " sectorCount=0x" << sectorCount
                      << " path=\"" << readGuestPrintableString(
                             rdram, readRdramProbeU32(rdram, object + 0x50u), 192u)
                      << "\"" << std::dec << std::endl;
        }
    }
    const bool isXmenCdFileCall =
        isCall &&
        (targetPc == 0x005880E0u || targetPc == 0x005883E0u ||
         targetPc == 0x00588508u || targetPc == 0x00588690u ||
         targetPc == 0x005886E8u || targetPc == 0x00588780u ||
         targetPc == 0x005887B8u || targetPc == 0x00588960u ||
         targetPc == 0x00588A60u || targetPc == 0x00588A98u ||
         targetPc == 0x00588AA0u || targetPc == 0x00588C38u ||
         targetPc == 0x00116780u || targetPc == 0x00116AB8u ||
         targetPc == 0x00116C28u || targetPc == 0x001176D0u);
    if (isXmenCdFileCall)
    {
        static std::atomic<uint32_t> s_xmenCdFileTraceCount{0u};
        const uint32_t count = s_xmenCdFileTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 512u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-cd-file-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << object
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " object0=0x" << readRdramProbeU32(rdram, object)
                      << " object4=0x" << readRdramProbeU32(rdram, object + 4u)
                      << " object8=0x" << readRdramProbeU32(rdram, object + 8u)
                      << " objectC=0x" << readRdramProbeU32(rdram, object + 0xCu)
                      << " object10=0x" << readRdramProbeU32(rdram, object + 0x10u)
                      << " object14=0x" << readRdramProbeU32(rdram, object + 0x14u)
                      << std::dec << std::endl;
        }
    }
    const std::array<uint32_t, 10> xmenMovieCallTargets = {
        0x00577260u, 0x00577360u, 0x00577408u, 0x00577598u, 0x005775D8u,
        0x005776C8u, 0x00577790u, 0x0057EDC0u, 0x0057EE38u, 0x0057EF78u,
    };
    size_t xmenMovieCallIndex = xmenMovieCallTargets.size();
    if (isCall)
    {
        const auto targetIt = std::find(
            xmenMovieCallTargets.begin(), xmenMovieCallTargets.end(), targetPc);
        xmenMovieCallIndex = static_cast<size_t>(targetIt - xmenMovieCallTargets.begin());
    }
    const bool isXmenMovieCall = xmenMovieCallIndex < xmenMovieCallTargets.size();
    uint32_t xmenMovieCallObject = 0u;
    uint32_t xmenMovieStreamObject = 0u;
    uint32_t xmenMovieCallTraceIndex = UINT32_MAX;
    if (isXmenMovieCall)
    {
        static std::array<std::atomic<uint32_t>, 10> s_xmenMovieCallCounts{};
        xmenMovieCallTraceIndex = s_xmenMovieCallCounts[xmenMovieCallIndex].fetch_add(
            1u, std::memory_order_relaxed);
        xmenMovieCallObject = GPR_U32(ctx, 4);
        const bool targetsStreamDirectly =
            targetPc == 0x0057EDC0u || targetPc == 0x0057EE38u || targetPc == 0x0057EF78u;
        xmenMovieStreamObject = targetsStreamDirectly
                                    ? xmenMovieCallObject
                                    : readRdramProbeU32(rdram, xmenMovieCallObject + 4u);
        if (xmenMovieCallTraceIndex < 64u)
        {
            std::cerr << "[xmen-movie-call:enter] targetIndex=" << std::dec
                      << xmenMovieCallIndex
                      << " call=" << xmenMovieCallTraceIndex
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << xmenMovieCallObject
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " wrapper0=0x" << readRdramProbeU32(rdram, xmenMovieCallObject)
                      << " wrapper4=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 4u)
                      << " wrapper8=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 8u)
                      << " wrapperC=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x0Cu)
                      << " wrapper10=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x10u)
                      << " wrapper14=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x14u)
                      << " wrapper18=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x18u)
                      << " wrapper1C=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x1Cu)
                      << " wrapper20=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x20u)
                      << " wrapper24=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x24u)
                      << " wrapper28=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x28u)
                      << " wrapper2C=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x2Cu)
                      << " wrapper30=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x30u)
                      << " stream=0x" << xmenMovieStreamObject
                      << " stream0=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject)
                      << " stream8=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 8u)
                      << " stream10=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x10u)
                      << " stream14=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x14u)
                      << " stream44=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x44u)
                      << " stream48=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x48u)
                      << std::dec << std::endl;
        }
    }
    const std::array<uint32_t, 4> xmenMpegPathTargets = {
        0x00561D18u, 0x00561DD8u, 0x00561948u, 0x005624F8u,
    };
    const std::array<uint32_t, 6> xmenSofdecServerTargets = {
        0x0055C4D8u, 0x0055D100u, 0x0055EC78u, 0x0055F1E8u, 0x0055F350u,
        0x0055FEE0u,
    };
    const auto xmenSofdecServerIt = std::find(
        xmenSofdecServerTargets.begin(), xmenSofdecServerTargets.end(), targetPc);
    const size_t xmenSofdecServerIndex = static_cast<size_t>(
        xmenSofdecServerIt - xmenSofdecServerTargets.begin());
    const bool isXmenSofdecServerCall =
        isCall && xmenSofdecServerIndex < xmenSofdecServerTargets.size();
    uint32_t xmenSofdecServerTraceIndex = UINT32_MAX;
    uint32_t xmenSofdecServerOwner = 0u;
    if (isXmenSofdecServerCall)
    {
        static std::array<std::atomic<uint32_t>, 6> s_xmenSofdecServerCounts{};
        xmenSofdecServerTraceIndex = s_xmenSofdecServerCounts[xmenSofdecServerIndex].fetch_add(
            1u, std::memory_order_relaxed);
        xmenSofdecServerOwner = GPR_U32(ctx, 4);
        if (xmenSofdecServerTraceIndex < 128u)
        {
            std::cerr << "[xmen-sofdec-server:enter] targetIndex=" << std::dec
                      << xmenSofdecServerIndex
                      << " call=" << xmenSofdecServerTraceIndex
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << xmenSofdecServerOwner
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " state0=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9A0u)
                      << " state1=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9A4u)
                      << " state2=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9A8u)
                      << " state3=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9ACu)
                      << " state4=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9B0u)
                      << " state5=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9B4u)
                      << " stream=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x1C68u)
                      << " decoder=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x1C68u)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    const auto xmenMpegPathIt = std::find(
        xmenMpegPathTargets.begin(), xmenMpegPathTargets.end(), targetPc);
    const size_t xmenMpegPathIndex = static_cast<size_t>(
        xmenMpegPathIt - xmenMpegPathTargets.begin());
    const bool isXmenMpegPathCall = isCall && xmenMpegPathIndex < xmenMpegPathTargets.size();
    uint32_t xmenMpegPathTraceIndex = UINT32_MAX;
    uint32_t xmenMpegOwner = 0u;
    uint32_t xmenMpegDecoderBase = 0u;
    uint32_t xmenMpegIpuState = 0u;
    if (isXmenMpegPathCall)
    {
        static std::array<std::atomic<uint32_t>, 4> s_xmenMpegPathCounts{};
        xmenMpegPathTraceIndex = s_xmenMpegPathCounts[xmenMpegPathIndex].fetch_add(
            1u, std::memory_order_relaxed);

        if (targetPc == 0x00561948u)
        {
            xmenMpegIpuState = GPR_U32(ctx, 4);
            if (xmenMpegIpuState >= 0xE18u)
            {
                xmenMpegDecoderBase = xmenMpegIpuState - 0xE18u;
            }
        }
        else
        {
            xmenMpegOwner = targetPc == 0x005624F8u ? GPR_U32(ctx, 7) : GPR_U32(ctx, 4);
            xmenMpegDecoderBase = readRdramProbeU32(rdram, xmenMpegOwner + 0x1C68u);
            if (xmenMpegDecoderBase != 0u)
            {
                xmenMpegIpuState = xmenMpegDecoderBase + 0xE18u;
            }
        }

        if (xmenMpegPathTraceIndex < 64u)
        {
            std::cerr << "[xmen-mpeg-path:enter] targetIndex=" << std::dec << xmenMpegPathIndex
                      << " call=" << xmenMpegPathTraceIndex
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " owner=0x" << xmenMpegOwner
                      << " decoder=0x" << xmenMpegDecoderBase
                      << " globalOwner=0x" << readRdramProbeU32(rdram, 0x006680E8u)
                      << " ready=0x" << readRdramProbeU32(rdram, xmenMpegDecoderBase + 0xDA8u)
                      << " state=0x" << xmenMpegIpuState
                      << " state0=0x" << readRdramProbeU32(rdram, xmenMpegIpuState)
                      << " state4=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 4u)
                      << " state8=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 8u)
                      << " stateC=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0xCu)
                      << " state10=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x10u)
                      << " state14=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x14u)
                      << " state18=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x18u)
                      << " state20=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x20u)
                      << " state44=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x44u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x00565D20u)
    {
        static std::atomic<uint32_t> s_xmenMovieEventRegistrationCount{0u};
        const uint32_t count = s_xmenMovieEventRegistrationCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 128u)
        {
            const uint32_t owner = GPR_U32(ctx, 4);
            const uint32_t event = GPR_U32(ctx, 5);
            const uint32_t slot = owner + 0x9A0u + event * 4u;
            std::cerr << "[xmen-movie-event-register] index=" << std::dec << count
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " owner=0x" << owner
                      << " event=0x" << event
                      << " callback=0x" << GPR_U32(ctx, 6)
                      << " oldCallback=0x" << readRdramProbeU32(rdram, slot)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x005506D0u)
    {
        static std::atomic<uint32_t> s_xmenMovieFrameRegistrationWrapperCount{0u};
        const uint32_t count = s_xmenMovieFrameRegistrationWrapperCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            std::cerr << "[xmen-movie-frame-register-wrapper] index=" << std::dec << count
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " object=0x" << GPR_U32(ctx, 4)
                      << " callback=0x" << GPR_U32(ctx, 5)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x005640E0u)
    {
        static std::atomic<uint32_t> s_xmenMovieFrameReadyCount{0u};
        const uint32_t count = s_xmenMovieFrameReadyCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t owner = GPR_U32(ctx, 4);
            const uint32_t decoder = readRdramProbeU32(rdram, owner + 0x1C68u);
            const uint32_t frameState = GPR_U32(ctx, 20);
            const uint32_t frameConfig = GPR_U32(ctx, 30);
            const uint32_t frameDescriptor = GPR_U32(ctx, 29);
            std::cerr << "[xmen-movie-frame-ready] index=" << std::dec << count
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " owner=0x" << owner
                      << " eventDelta=0x" << GPR_U32(ctx, 5)
                      << " eventValue=0x" << GPR_U32(ctx, 6)
                      << " callback=0x" << readRdramProbeU32(rdram, owner + 0xA30u)
                      << " decoder=0x" << decoder
                      << " frameState=0x" << frameState
                      << " frameConfig=0x" << frameConfig
                      << " frameMode=0x" << readRdramProbeU32(rdram, frameConfig + 0x38u)
                      << " currentFrame=0x" << readRdramProbeU32(rdram, decoder + 0x140u)
                      << " previousFrame=0x" << readRdramProbeU32(rdram, decoder + 0x144u)
                      << " queuedFrames=0x" << readRdramProbeU32(rdram, decoder + 0x148u)
                      << " desc0=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x00u)
                      << " desc4=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x04u)
                      << " desc18=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x18u)
                      << " image=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x20u)
                      << " desc28=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x28u)
                      << " desc2c=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x2Cu)
                      << " frame=0x" << readRdramProbeU32(rdram, frameDescriptor + 0x30u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x00564124u)
    {
        static std::atomic<uint32_t> s_xmenMovieFrameCallbackCount{0u};
        const uint32_t count = s_xmenMovieFrameCallbackCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            std::cerr << "[xmen-movie-frame-callback] index=" << std::dec << count
                      << " thread=" << eeScheduler().currentThreadId()
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " owner=0x" << GPR_U32(ctx, 4)
                      << " eventValue=0x" << GPR_U32(ctx, 5)
                      << " counters=0x" << GPR_U32(ctx, 6)
                      << std::dec << std::endl;
        }
    }
    static std::atomic<uint32_t> s_xmenLegalViewerAddress{0u};
    if (isCall && sourcePc == 0x0031F5BCu && targetPc == 0x005C1AA0u)
    {
        const uint32_t viewer = GPR_U32(ctx, 4);
        const uint32_t previous = s_xmenLegalViewerAddress.exchange(viewer, std::memory_order_relaxed);
        if (viewer != previous)
        {
            std::cerr << "[xmen-legal-viewer-address] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " old=0x" << previous
                      << " value=0x" << viewer
                      << " vtable=0x" << readRdramProbeU32(rdram, viewer + 0xEC8u)
                      << std::dec << std::endl;
        }
    }
    const uint32_t xmenLegalViewer = s_xmenLegalViewerAddress.load(std::memory_order_relaxed);
    if (isCall && sourcePc == 0x002DE69Cu && targetPc == 0x002DE130u)
    {
        static std::atomic<uint32_t> s_xmenGifIrqDispatchTraceCount{0u};
        const uint32_t count = s_xmenGifIrqDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 128u)
        {
            const uint32_t state = GPR_U32(ctx, 5);
            std::cerr << "[xmen-gif-irq-dispatch] index=" << std::dec << count
                      << " cause=0x" << std::hex << GPR_U32(ctx, 4)
                      << " state=0x" << state;
            for (uint32_t offset = 0u; offset <= 0x40u; offset += 4u)
            {
                std::cerr << " f" << std::dec << offset
                          << "=0x" << std::hex << readRdramProbeU32(rdram, state + offset);
            }
            std::cerr << " frameSema=0x" << readRdramProbeU32(rdram, 0x00753800u)
                      << " completed=0x" << readRdramProbeU32(rdram, 0x007537E0u)
                      << " submitted=0x" << readRdramProbeU32(rdram, 0x007537E8u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x002DE1E8u)
    {
        static std::atomic<uint32_t> s_xmenGifIrqCallbackTraceCount{0u};
        const uint32_t count = s_xmenGifIrqCallbackTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            const uint32_t node = GPR_U32(ctx, 20);
            std::cerr << "[xmen-gif-irq-callback] index=" << std::dec << count
                      << " target=0x" << std::hex << targetPc
                      << " node=0x" << node
                      << " next=0x" << readRdramProbeU32(rdram, node + 0x4u)
                      << " callback=0x" << readRdramProbeU32(rdram, node + 0x8u)
                      << " argument=0x" << readRdramProbeU32(rdram, node + 0xCu)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " frameSema=0x" << readRdramProbeU32(rdram, 0x00753800u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x002F6AB0u && targetPc == 0x0010AF50u)
    {
        static std::atomic<uint32_t> s_xmenFrameSemaSignalTraceCount{0u};
        const uint32_t count = s_xmenFrameSemaSignalTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            std::cerr << "[xmen-frame-sema-signal] index=" << std::dec << count
                      << " id=" << GPR_U32(ctx, 4)
                      << " completed=0x" << std::hex << readRdramProbeU32(rdram, 0x007537E0u)
                      << " submitted=0x" << readRdramProbeU32(rdram, 0x007537E8u)
                      << " active=0x" << readRdramProbeU32(rdram, 0x00753840u)
                      << " pending=0x" << readRdramProbeU32(rdram, 0x007537D0u)
                      << " busy=0x" << readRdramProbeU32(rdram, 0x00753870u)
                      << " irqDepth=0x" << readRdramProbeU32(rdram, 0x00753878u)
                      << " mode=0x" << (readRdramProbeU32(rdram, 0x00750684u) & 0xffffu)
                      << " csr=0x" << m_memory.gs().csr.load(std::memory_order_relaxed)
                      << " imr=0x" << m_memory.gs().imr
                      << " siglblid=0x" << m_memory.gs().siglblid
                      << std::dec << std::endl;
        }
    }
    if (isCall &&
        (sourcePc == 0x00431C24u || sourcePc == 0x00431C4Cu ||
         sourcePc == 0x00431C74u || sourcePc == 0x00431C9Cu))
    {
        static std::atomic<uint32_t> s_xmenTextureFactoryTraceCount{0u};
        const uint32_t count = s_xmenTextureFactoryTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t owner = GPR_U32(ctx, 17);
            std::cerr << "[xmen-texture-factory] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " object=0x" << object
                      << " objectVt=0x" << readRdramProbeU32(rdram, object)
                      << " owner=0x" << owner
                      << " owner20=0x" << readRdramProbeU32(rdram, owner + 0x20u)
                      << " owner24=0x" << readRdramProbeU32(rdram, owner + 0x24u)
                      << " owner28=0x" << readRdramProbeU32(rdram, owner + 0x28u)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x00325F38u || sourcePc == 0x0013781Cu))
    {
        static std::atomic<uint32_t> s_xmenLegalLayerStateTraceCount{0u};
        const uint32_t count = s_xmenLegalLayerStateTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[xmen-legal-layer-state] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << vtable
                      << " slot38=0x" << readRdramProbeU32(rdram, vtable + 0x38u)
                      << " f18=0x" << readRdramProbeU32(rdram, object + 0x18u)
                      << " f190=0x" << readRdramProbeU32(rdram, object + 0x190u)
                      << " f194=0x" << readRdramProbeU32(rdram, object + 0x194u)
                      << " f198=0x" << readRdramProbeU32(rdram, object + 0x198u)
                      << " f19c=0x" << readRdramProbeU32(rdram, object + 0x19Cu)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (sourcePc == 0x005C1A3Cu && targetPc == 0x003246E0u)
    {
        constexpr uint32_t xmenLegalImageOffset = 0x0ED4u;
        const uint32_t viewer = GPR_U32(ctx, 17);
        const uint32_t wrappedImage = readRdramProbeU32(rdram, xmenLegalViewer + xmenLegalImageOffset);
        const uint32_t resolvedImage = GPR_U32(ctx, 2);
        const uint32_t wrappedVtable = readRdramProbeU32(rdram, wrappedImage);
        const uint32_t resolvedVtable = readRdramProbeU32(rdram, resolvedImage);
        if (viewer == xmenLegalViewer && wrappedVtable == 0x0070B2A0u &&
            resolvedImage != 0u && resolvedImage != wrappedImage && resolvedVtable == 0x006F5440u)
        {
            writeRdramProbeU32(rdram, xmenLegalViewer + xmenLegalImageOffset, resolvedImage);
            std::cerr << "[xmen-legal-image-resolve] viewer=0x" << std::hex << viewer
                      << " wrapped=0x" << wrappedImage
                      << " wrappedVt=0x" << wrappedVtable
                      << " resolved=0x" << resolvedImage
                      << " resolvedVt=0x" << resolvedVtable
                      << std::dec << std::endl;
        }
    }
    const uint32_t xmenLegalEd4 = readRdramProbeU32(rdram, xmenLegalViewer + 0xED4u);
    const bool xmenLegalSceneActive = xmenLegalEd4 != 0u && xmenLegalEd4 != 0x3FFFFFFFu;
    static std::atomic<bool> s_xmenLegalLayerTraceArmed{false};
    if (xmenLegalSceneActive && targetPc == 0x003272A0u)
    {
        s_xmenLegalLayerTraceArmed.store(true, std::memory_order_relaxed);
    }
    if (kind == GuestBranchKind::IndirectCall &&
        s_xmenLegalLayerTraceArmed.load(std::memory_order_relaxed))
    {
        static std::atomic<uint32_t> s_xmenLegalIndirectTraceCount{0u};
        const uint32_t count = s_xmenLegalIndirectTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-legal-indirect] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (s_xmenLegalLayerTraceArmed.load(std::memory_order_relaxed) &&
        (sourcePc == 0x0030F840u || sourcePc == 0x0030F87Cu ||
         sourcePc == 0x0030F8B8u || sourcePc == 0x00158380u))
    {
        static std::atomic<uint32_t> s_xmenDescriptorTraceCount{0u};
        const uint32_t count = s_xmenDescriptorTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            const uint32_t object = sourcePc == 0x00158380u ? GPR_U32(ctx, 17) : GPR_U32(ctx, 4);
            const uint32_t descriptor = sourcePc == 0x00158380u ? object + 0x6Cu : GPR_U32(ctx, 5);
            std::cerr << "[xmen-render-descriptor] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " object=0x" << object
                      << " objectVt=0x" << readRdramProbeU32(rdram, object)
                      << " object8=0x" << readRdramProbeU32(rdram, object + 0x8u)
                      << " objectC=0x" << readRdramProbeU32(rdram, object + 0xCu)
                      << " object10=0x" << readRdramProbeU32(rdram, object + 0x10u)
                      << " object14=0x" << readRdramProbeU32(rdram, object + 0x14u)
                      << " descriptor=0x" << descriptor;
            for (uint32_t descriptorIndex = 0u; descriptorIndex < (sourcePc == 0x00158380u ? 3u : 1u);
                 ++descriptorIndex)
            {
                const uint32_t current = descriptor + descriptorIndex * 0x1Cu;
                std::cerr << " d" << std::dec << descriptorIndex
                          << "=[0x" << std::hex << readRdramProbeU32(rdram, current)
                          << ",0x" << readRdramProbeU32(rdram, current + 0x4u)
                          << ",0x" << readRdramProbeU32(rdram, current + 0x8u)
                          << ",0x" << readRdramProbeU32(rdram, current + 0xCu)
                          << ",0x" << readRdramProbeU32(rdram, current + 0x10u)
                          << ",0x" << readRdramProbeU32(rdram, current + 0x14u)
                          << ",0x" << readRdramProbeU32(rdram, current + 0x18u) << "]";
            }
            std::cerr << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << std::dec << std::endl;
        }
    }
    if (xmenLegalSceneActive && (sourcePc == 0x00326E6Cu || sourcePc == 0x00326E78u))
    {
        static std::atomic<uint32_t> s_xmenLegalManagerLookupTraceCount{0u};
        const uint32_t count = s_xmenLegalManagerLookupTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            const uint32_t manager = sourcePc == 0x00326E6Cu ? GPR_U32(ctx, 4) : GPR_U32(ctx, 17);
            const uint32_t output = sourcePc == 0x00326E6Cu ? GPR_U32(ctx, 5) : GPR_U32(ctx, 29) + 0x3Cu;
            const uint32_t vtable = readRdramProbeU32(rdram, manager);
            std::cerr << "[xmen-legal-manager-lookup] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " manager=0x" << manager
                      << " managerVt=0x" << vtable
                      << " slot64=0x" << readRdramProbeU32(rdram, vtable + 0x64u)
                      << " manager4=0x" << readRdramProbeU32(rdram, manager + 0x4u)
                      << " manager8=0x" << readRdramProbeU32(rdram, manager + 0x8u)
                      << " managerC=0x" << readRdramProbeU32(rdram, manager + 0xCu)
                      << " manager10=0x" << readRdramProbeU32(rdram, manager + 0x10u)
                      << " manager14=0x" << readRdramProbeU32(rdram, manager + 0x14u)
                      << " output=0x" << output
                      << " outputValue=0x" << readRdramProbeU32(rdram, output)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << std::dec << std::endl;
        }
    }
    if (isCall && xmenLegalSceneActive &&
        (targetPc == 0x001375A0u ||
         (sourcePc >= 0x001375A0u && sourcePc < 0x00137A28u)))
    {
        static std::atomic<uint32_t> s_xmenLegalRenderRootTraceCount{0u};
        const uint32_t count = s_xmenLegalRenderRootTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 512u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t s1 = GPR_U32(ctx, 17);
            std::cerr << "[xmen-legal-render-root] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " a0=0x" << a0
                      << " a0vt=0x" << readRdramProbeU32(rdram, a0)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << s1
                      << " s1vt=0x" << readRdramProbeU32(rdram, s1)
                      << " s1ListVt=0x" << readRdramProbeU32(rdram, s1 + 0x10u)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << " s5=0x" << GPR_U32(ctx, 21)
                      << " legalEd4=0x" << xmenLegalEd4
                      << " legalEd8=0x" << readRdramProbeU32(rdram, xmenLegalViewer + 0xED8u)
                      << " legalEd4Vt=0x" << readRdramProbeU32(rdram, xmenLegalEd4)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && xmenLegalSceneActive && sourcePc == 0x002E66F4u)
    {
        static std::atomic<uint32_t> s_xmenLegalVifProducerTraceCount{0u};
        const uint32_t count = s_xmenLegalVifProducerTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 128u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            std::cerr << "[xmen-legal-vif-producer] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " a0=0x" << a0
                      << " a0vt=0x" << readRdramProbeU32(rdram, a0)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && s_xmenLegalLayerTraceArmed.load(std::memory_order_relaxed) &&
        ((sourcePc >= 0x00158300u && sourcePc < 0x00158508u) ||
         (sourcePc >= 0x00325840u && sourcePc < 0x00325AD8u) ||
         (sourcePc >= 0x003272A0u && sourcePc < 0x00327334u)))
    {
        static std::atomic<uint32_t> s_xmenLegalLayerRenderTraceCount{0u};
        const uint32_t count = s_xmenLegalLayerRenderTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 768u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t s4 = GPR_U32(ctx, 20);
            const bool inRendererSubmit = sourcePc >= 0x00158300u && sourcePc < 0x00158508u;
            const bool inLayerSubmit = sourcePc >= 0x00325840u && sourcePc < 0x00325AD8u;
            const uint32_t layer = inRendererSubmit && s4 >= 0xACu
                ? s4 - 0xACu
                : (inLayerSubmit ? GPR_U32(ctx, 16) : GPR_U32(ctx, 18));
            const uint32_t renderState = inRendererSubmit ? s4 : layer + 0xACu;
            const uint32_t renderer = readRdramProbeU32(rdram, renderState + 0x58u);
            std::cerr << "[xmen-legal-layer-render] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " a0=0x" << a0
                      << " a0vt=0x" << readRdramProbeU32(rdram, a0)
                      << " a1=0x" << a1
                      << " a1vt=0x" << readRdramProbeU32(rdram, a1)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << s4
                      << " layer=0x" << layer
                      << " layerVt=0x" << readRdramProbeU32(rdram, layer)
                      << " scene0=0x" << readRdramProbeU32(rdram, layer + 0x4u)
                      << " scene4=0x" << readRdramProbeU32(rdram, layer + 0x8u)
                      << " scene8=0x" << readRdramProbeU32(rdram, layer + 0xCu)
                      << " sceneC=0x" << readRdramProbeU32(rdram, layer + 0x10u)
                      << " renderState=0x" << renderState
                      << " renderStateCount=0x" << readRdramProbeU32(rdram, renderState + 0x50u)
                      << " renderer=0x" << renderer
                      << " rendererVt=0x" << readRdramProbeU32(rdram, renderer)
                      << " renderResult=0x" << readRdramProbeU32(rdram, renderState + 0x64u)
                      << " legalEd4=0x" << xmenLegalEd4
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall)
    {
        const bool isIgRenderMethod =
            (targetPc >= 0x003EBF40u && targetPc <= 0x003EFDC0u);
        const bool isIgListOrNodeMethod =
            targetPc == 0x0019C140u || targetPc == 0x0019C250u ||
            targetPc == 0x001B9CD0u || targetPc == 0x001AD5D0u ||
            targetPc == 0x001AD8A0u || targetPc == 0x0019BFB0u ||
            targetPc == 0x00213300u || targetPc == 0x00213310u ||
            targetPc == 0x00213320u || targetPc == 0x00213330u ||
            targetPc == 0x00213360u || targetPc == 0x00213370u ||
            targetPc == 0x00213380u || targetPc == 0x002133A0u ||
            targetPc == 0x002133B0u || targetPc == 0x002133C0u ||
            targetPc == 0x002133D0u || targetPc == 0x002133F0u ||
            targetPc == 0x00213400u || targetPc == 0x00213410u ||
            targetPc == 0x00213420u;
        if (xmenLegalSceneActive && (isIgRenderMethod || isIgListOrNodeMethod))
        {
            static std::atomic<uint32_t> s_xmenIgDispatchTraceCount{0u};
            const uint32_t count = s_xmenIgDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 32u)
            {
                constexpr uint32_t renderContext = 0x00AE85D0u;
                const uint32_t a0 = GPR_U32(ctx, 4);
                const uint32_t a1 = GPR_U32(ctx, 5);
                std::cerr << "[xmen-ig-dispatch] index=" << std::dec << count
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " kind=" << describeGuestBranchKind(kind)
                          << " a0=0x" << a0
                          << " a0vt=0x" << readRdramProbeU32(rdram, a0)
                          << " a1=0x" << a1
                          << " a1vt=0x" << readRdramProbeU32(rdram, a1)
                          << " a2=0x" << GPR_U32(ctx, 6)
                          << " a3=0x" << GPR_U32(ctx, 7)
                          << " v0=0x" << GPR_U32(ctx, 2)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " sp=0x" << GPR_U32(ctx, 29)
                          << " legalEd8=0x" << readRdramProbeU32(rdram, xmenLegalViewer + 0xED8u)
                          << " legalEd4=0x" << xmenLegalEd4
                          << " contextHead=0x" << readRdramProbeU32(rdram, renderContext + 0x1Cu)
                          << " contextCount=0x" << readRdramProbeU32(rdram, renderContext + 0x4u)
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            }
        }
    }
    if (isCall && targetPc == 0x004A32E0u &&
        (sourcePc == 0x0014CB0Cu || sourcePc == 0x0014CBB4u || sourcePc == 0x0014CC5Cu))
    {
        static std::atomic<uint32_t> s_xmenFactoryLookupTraceCount{0u};
        const uint32_t count = s_xmenFactoryLookupTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t output = GPR_U32(ctx, 6);
            std::cerr << "[xmen-factory-lookup:enter] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " factory=0x" << GPR_U32(ctx, 4)
                      << " selector=0x" << GPR_U32(ctx, 5)
                      << " output=0x" << output
                      << " outputBefore=0x" << readRdramProbeU32(rdram, output)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x00185F20u)
    {
        static std::atomic<uint32_t> s_xmenShadowBoundsEntryTraceCount{0u};
        const uint32_t count = s_xmenShadowBoundsEntryTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u || GPR_U32(ctx, 5) == 0u)
        {
            std::cerr << "[xmen-shadow-bounds-entry] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " state=0x" << GPR_U32(ctx, 4)
                      << " root=0x" << GPR_U32(ctx, 5)
                      << " stateVtable=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 4))
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x00185E50u || sourcePc == 0x00185E58u || sourcePc == 0x00185EC4u))
    {
        static std::atomic<uint32_t> s_xmenShadowTraversalRootTraceCount{0u};
        const uint32_t count = s_xmenShadowTraversalRootTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 24u)
        {
            const uint32_t rootSlot = GPR_U32(ctx, 16);
            std::cerr << "[xmen-shadow-traversal-root] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " rootSlot=0x" << rootSlot
                      << " root=0x" << readRdramProbeU32(rdram, rootSlot)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x00214F30u && GPR_U32(ctx, 4) == 0u)
    {
        static std::atomic<uint32_t> s_xmenNullTypeCheckTraceCount{0u};
        const uint32_t count = s_xmenNullTypeCheckTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            const uint32_t requestedType = GPR_U32(ctx, 5);
            const uint32_t typeName = readRdramProbeU32(rdram, requestedType + 0x1Cu);
            const uint32_t stack = GPR_U32(ctx, 29);
            const uint32_t parentNode = readRdramProbeU32(rdram, stack + 0x30u);
            const uint32_t childIndex = readRdramProbeU32(rdram, stack);
            const uint32_t childList = readRdramProbeU32(rdram, parentNode + 0x1Cu);
            const uint32_t childItems = readRdramProbeU32(rdram, childList + 0x10u);
            std::cerr << "[xmen-null-type-check-entry] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " return=0x" << fallthroughPc
                      << " requestedType=0x" << requestedType
                      << " typeVtable=0x" << readRdramProbeU32(rdram, requestedType + 0x5Cu)
                      << " typeName=0x" << typeName
                      << " typeNameText=\"" << readGuestPrintableString(rdram, typeName, 96u) << '\"'
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s6=0x" << GPR_U32(ctx, 22)
                      << " s7=0x" << GPR_U32(ctx, 23)
                      << " s8=0x" << GPR_U32(ctx, 30)
                      << " sp=0x" << stack
                      << " parentNode=0x" << parentNode
                      << " parentVtable=0x" << readRdramProbeU32(rdram, parentNode)
                      << " childIndex=0x" << childIndex
                      << " savedChildCount=0x" << readRdramProbeU32(rdram, stack + 0x60u)
                      << " childList=0x" << childList
                      << " childListVtable=0x" << readRdramProbeU32(rdram, childList)
                      << " childCount=0x" << readRdramProbeU32(rdram, childList + 8u)
                      << " childItems=0x" << childItems
                      << " selectedChild=0x" << readRdramProbeU32(rdram, childItems + childIndex * 4u)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x0024B8F0u)
    {
        static std::atomic<uint32_t> s_xmenControllerUpdateTraceCount{0u};
        const uint32_t count = s_xmenControllerUpdateTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 2048u && (count % 128u) == 0u)
        {
            const uint32_t listener = GPR_U32(ctx, 5);
            std::cerr << "[xmen-controller-update] index=" << std::dec << count
                      << " tick=" << xmenBranchTick
                      << " source=0x" << std::hex << sourcePc
                      << " controller=0x" << GPR_U32(ctx, 4)
                      << " listener=0x" << listener
                      << " listenerVtable=0x" << readRdramProbeU32(rdram, listener)
                      << " callback0c=0x" << readRdramProbeU32(rdram, listener + 0x0Cu)
                      << " callback10=0x" << readRdramProbeU32(rdram, listener + 0x10u)
                      << " callback14=0x" << readRdramProbeU32(rdram, listener + 0x14u)
                      << " callback18=0x" << readRdramProbeU32(rdram, listener + 0x18u)
                      << " callback1c=0x" << readRdramProbeU32(rdram, listener + 0x1Cu)
                      << " callback20=0x" << readRdramProbeU32(rdram, listener + 0x20u)
                      << " callback24=0x" << readRdramProbeU32(rdram, listener + 0x24u)
                      << " callback28=0x" << readRdramProbeU32(rdram, listener + 0x28u)
                      << " callback2c=0x" << readRdramProbeU32(rdram, listener + 0x2Cu)
                      << " callback30=0x" << readRdramProbeU32(rdram, listener + 0x30u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x0024B2E0u && sourcePc >= 0x00300600u && sourcePc <= 0x00300900u)
    {
        static std::atomic<uint32_t> s_xmenControllerReaddEarlyTraceCount{0u};
        const uint32_t count = s_xmenControllerReaddEarlyTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            const uint32_t manager = GPR_U32(ctx, 2);
            const uint32_t owner = GPR_U32(ctx, 4);
            const uint32_t list = readRdramProbeU32(rdram, owner + 8u);
            const uint32_t items = readRdramProbeU32(rdram, list + 16u);
            std::cerr << "[xmen-controller-readd-early] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " manager=0x" << manager
                      << " owner=0x" << owner
                      << " value=0x" << GPR_U32(ctx, 5)
                      << " managerVtable=0x" << readRdramProbeU32(rdram, manager)
                      << " managerE38=0x" << readRdramProbeU32(rdram, manager + 0xE38u)
                      << " managerE3c=0x" << readRdramProbeU32(rdram, manager + 0xE3Cu)
                      << " managerE40=0x" << readRdramProbeU32(rdram, manager + 0xE40u)
                      << " managerE44=0x" << readRdramProbeU32(rdram, manager + 0xE44u)
                      << " list=0x" << list
                      << " listCount=0x" << readRdramProbeU32(rdram, list + 8u)
                      << " items=0x" << items
                      << " item0=0x" << readRdramProbeU32(rdram, items)
                      << " item1=0x" << readRdramProbeU32(rdram, items + 4u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x0024C104u)
    {
        const uint32_t packet = GPR_U32(ctx, 22);
        const uint16_t buttons = static_cast<uint16_t>(readRdramProbeU32(rdram, packet + 2u));
        if (buttons != 0xFFFFu)
        {
            static std::atomic<uint32_t> s_xmenControllerPacketTraceCount{0u};
            const uint32_t count = s_xmenControllerPacketTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 64u)
            {
                std::cerr << "[xmen-controller-packet] index=" << std::dec << count
                          << " tick=" << xmenBranchTick
                          << " packet=0x" << std::hex << packet
                          << " buttons=0x" << buttons
                          << " controller=0x" << GPR_U32(ctx, 17)
                          << " listener=0x" << GPR_U32(ctx, 16)
                          << " callback=0x" << targetPc
                          << std::dec << std::endl;
            }
        }
    }
    if (isCall && (sourcePc == 0x0024C158u || sourcePc == 0x0024C194u))
    {
        static std::atomic<uint32_t> s_xmenControllerEdgeTraceCount{0u};
        const uint32_t count = s_xmenControllerEdgeTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-controller-edge] index=" << std::dec << count
                      << " tick=" << xmenBranchTick
                      << " kind=" << (sourcePc == 0x0024C194u ? "pressed" : "released")
                      << " bit=" << GPR_U32(ctx, 19)
                      << " controller=0x" << std::hex << GPR_U32(ctx, 17)
                      << " listener=0x" << GPR_U32(ctx, 16)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x0024C174u || sourcePc == 0x0024C1B0u))
    {
        static std::atomic<uint32_t> s_xmenControllerDispatchTraceCount{0u};
        const uint32_t count = s_xmenControllerDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-controller-dispatch] index=" << std::dec << count
                      << " tick=" << xmenBranchTick
                      << " kind=" << (sourcePc == 0x0024C1B0u ? "pressed" : "released")
                      << " target=0x" << std::hex << targetPc
                      << " listener=0x" << GPR_U32(ctx, 4)
                      << " controllerId=0x" << GPR_U32(ctx, 5)
                      << " controller=0x" << GPR_U32(ctx, 6)
                      << " event=0x" << GPR_U32(ctx, 7)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x0024B4FCu || sourcePc == 0x0024B52Cu))
    {
        static std::atomic<uint32_t> s_xmenControllerListenerTraceCount{0u};
        const uint32_t count = s_xmenControllerListenerTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-controller-listener] index=" << std::dec << count
                      << " tick=" << xmenBranchTick
                      << " kind=" << (sourcePc == 0x0024B4FCu ? "pressed" : "released")
                      << " target=0x" << std::hex << targetPc
                      << " controllerId=0x" << GPR_U32(ctx, 4)
                      << " controller=0x" << GPR_U32(ctx, 5)
                      << " event=0x" << GPR_U32(ctx, 6)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x00422430u)
    {
        static std::atomic<uint32_t> s_xmenNewGameHandlerTraceCount{0u};
        const uint32_t count = s_xmenNewGameHandlerTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            std::cerr << "[xmen-newgame-handler] index=" << std::dec << count
                      << " tick=" << xmenBranchTick
                      << " source=0x" << std::hex << sourcePc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    const bool isPackageCallback =
        targetPc == 0x004B82D0u || targetPc == 0x004B8320u ||
        targetPc == 0x004B83B0u || targetPc == 0x004B84F0u ||
        targetPc == 0x004B8550u || targetPc == 0x004B85E0u ||
        targetPc == 0x004B8640u || targetPc == 0x004B86A0u ||
        targetPc == 0x004B8770u || targetPc == 0x004B87C0u ||
        targetPc == 0x004B8810u || targetPc == 0x004B8990u ||
        targetPc == 0x004B8A10u || targetPc == 0x004B8A90u ||
        targetPc == 0x00614140u || targetPc == 0x004F5930u;
    const bool isApplicationCallback =
        targetPc == 0x0014AE50u || targetPc == 0x0014B240u ||
        targetPc == 0x0014B600u || targetPc == 0x0014B610u ||
        targetPc == 0x0014BE90u || targetPc == 0x0014BFA0u ||
        targetPc == 0x0014BFB0u || targetPc == 0x0014AD80u ||
        targetPc == 0x0014AD90u;
    if (isCall && isApplicationCallback)
    {
        static std::atomic<uint32_t> s_xmenApplicationCallbackTraceCount{0u};
        const uint32_t count = s_xmenApplicationCallbackTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            std::cerr << "[xmen-application-callback] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a0Vtable=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 4))
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << std::dec << std::endl;
        }
    }
    if (isCall && isPackageCallback)
    {
        static std::atomic<uint32_t> s_xmenPackageCallbackTraceCount{0u};
        const uint32_t count = s_xmenPackageCallbackTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 512u)
        {
            std::cerr << "[xmen-package-callback] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a1Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 192u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << std::dec << std::endl;
        }
    }
    if (isCall)
    {
        uint32_t remaining = s_xmenPostLegalInflateTraceBudget.load(std::memory_order_relaxed);
        while (remaining != 0u &&
               !s_xmenPostLegalInflateTraceBudget.compare_exchange_weak(
                   remaining, remaining - 1u, std::memory_order_relaxed))
        {
        }
        if (remaining != 0u)
        {
            const uint32_t index = s_xmenPostLegalInflateTraceIndex.fetch_add(
                1u, std::memory_order_relaxed);
            std::cerr << "[xmen-post-legal-call] index=" << std::dec << index
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << (kind == GuestBranchKind::DirectCall ? "direct" : "indirect")
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x0030C660u || sourcePc == 0x0030C674u ||
                   sourcePc == 0x00613C2Cu || sourcePc == 0x00613CA8u))
    {
        static std::atomic<uint32_t> s_xmenLocalizedLoadTraceCount{0u};
        const uint32_t count = s_xmenLocalizedLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 24u)
        {
            std::cerr << "[xmen-localized-load] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a0Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 4), 192u) << "\""
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a1Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 192u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a2Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 6), 192u) << "\""
                      << std::dec << std::endl;
        }
    }
    if (isCall && ((sourcePc >= 0x004B8190u && sourcePc < 0x004B8600u) ||
                   sourcePc == 0x0031E928u || targetPc == 0x004B8140u ||
                   targetPc == 0x00613720u || targetPc == 0x004F71B0u))
    {
        static std::atomic<uint32_t> s_xmenPackageMountTraceCount{0u};
        const uint32_t count = s_xmenPackageMountTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            const uint32_t registry = readGuestWord(rdram, 0x00844040u);
            std::cerr << "[xmen-package-mount] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " registry=0x" << registry
                      << " registryUsedBits=0x" << readGuestWord(rdram, registry + 0x93F8u)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a0Vtable=0x" << readGuestWord(rdram, GPR_U32(ctx, 4))
                      << " a0CallbackCount=0x" << readGuestWord(rdram, GPR_U32(ctx, 4) + 0x2B0u)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a1Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 192u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << '\n';
        }
    }
    const bool isXmenObjectAcquireCall = isCall && sourcePc == 0x005D54C0u;
    const bool isXmenObjectMethodCall = isCall && sourcePc == 0x005D54F4u;
    const bool isXmenLevelActivationCall = isCall && targetPc == 0x00321B90u;
    const uint32_t xmenActivationManager = isXmenLevelActivationCall ? GPR_U32(ctx, 4) : 0u;
    const uint32_t xmenActivationObject = isXmenLevelActivationCall ? GPR_U32(ctx, 5) : 0u;
    const bool isXmenLegalPlatformNameCall = isCall && sourcePc == 0x003201E8u;
    const bool isXmenLegalPackageLookupCall = isCall && sourcePc == 0x003207C8u;
    const bool isXmenLegalMetadataExtractCall = isCall && sourcePc == 0x003208D8u;
    if (isCall && (sourcePc == 0x00213E20u || sourcePc == 0x00210708u ||
                   sourcePc == 0x00210720u || sourcePc == 0x00210738u))
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t base = GPR_U32(ctx, 5);
        const uint32_t fieldOffset = readRdramProbeU32(rdram, object + 0x08u);
        const uint32_t fieldSize = readRdramProbeU32(rdram, object + 0x14u) & 0xFFFFu;
        const uint32_t destination = base + fieldOffset;
        const uint64_t destinationEnd = static_cast<uint64_t>(destination) + fieldSize;
        if (destination <= 0x00B70060u && destinationEnd > 0x00B70060u)
        {
            static std::atomic<uint32_t> s_xmenDisposeTraceCount{0u};
            const uint32_t count = s_xmenDisposeTraceCount.fetch_add(1u, std::memory_order_relaxed);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[xmen-dispose-dispatch] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << vtable
                      << " base=0x" << base
                      << " fieldOffset=0x" << fieldOffset
                      << " fieldSize=0x" << fieldSize
                      << " destination=0x" << destination
                      << " typeWord=0x" << readRdramProbeU32(rdram, object + 0x24u)
                      << " slot70=0x" << readRdramProbeU32(rdram, vtable + 0x70u)
                      << " slot74=0x" << readRdramProbeU32(rdram, vtable + 0x74u)
                      << " slot78=0x" << readRdramProbeU32(rdram, vtable + 0x78u)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << std::dec << std::endl;
        }
    }
    if (isXmenLegalPlatformNameCall)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t vtable = readRdramProbeU32(rdram, object);
        std::cerr << "[xmen-legal-platform-name:enter] target=0x" << std::hex << targetPc
                  << " object=0x" << object
                  << " vtable=0x" << vtable
                  << " slot10=0x" << readRdramProbeU32(rdram, vtable + 0x10u)
                  << " selector=0x" << GPR_U32(ctx, 5)
                  << std::dec << std::endl;
    }
    if (isCall && (sourcePc == 0x00320A3Cu || sourcePc == 0x00320BF0u))
    {
        const uint32_t sp = GPR_U32(ctx, 29);
        std::cerr << "[xmen-legal-factory:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a1Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 128u) << "\""
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " a2Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 6), 128u) << "\""
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s2=0x" << GPR_U32(ctx, 18)
                  << " out=0x" << readRdramProbeU32(rdram, sp + 0x1ACu)
                  << std::dec << std::endl;
    }
    if (isXmenLegalPackageLookupCall || isXmenLegalMetadataExtractCall)
    {
        std::cerr << "[xmen-legal-metadata:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a1Value=\"" << readGuestPrintableString(rdram, GPR_U32(ctx, 5), 160u) << "\""
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << std::dec << std::endl;
    }
    if (isCall && (sourcePc == 0x004B7C3Cu || sourcePc == 0x003B16D0u ||
                   sourcePc == 0x003B1710u || sourcePc == 0x004B7C78u ||
                   sourcePc == 0x004B7D38u || sourcePc == 0x006128F8u))
    {
        static std::atomic<uint32_t> s_xmenPackageStreamTraceCount{0u};
        const uint32_t count = s_xmenPackageStreamTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-package-stream] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << readRdramProbeU32(rdram, object)
                      << " selector=0x" << readRdramProbeU32(rdram, 0x007A2134u)
                      << " active=0x" << (readRdramProbeU32(rdram, 0x007A2134u) >> 24u)
                      << " object4=0x" << readRdramProbeU32(rdram, object + 4u)
                      << " handle=0x" << readRdramProbeU32(rdram, object + 0xC8u)
                      << " size=0x" << readRdramProbeU32(rdram, object + 0xD0u)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << std::dec << std::endl;
        }
    }
    const uint32_t xmenCallObject = GPR_U32(ctx, 4);
    const bool isXmenLegalManagerCall =
        isCall && xmenCallObject != 0u && readRdramProbeU32(rdram, xmenCallObject) == 0x00708C00u;
    if (isXmenLegalManagerCall)
    {
        static std::atomic<uint32_t> s_xmenLegalManagerCallCount{0u};
        const uint32_t count = s_xmenLegalManagerCallCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            const uint32_t objectArray = readRdramProbeU32(rdram, xmenCallObject + 0x0BD4u);
            const uint32_t objectCount = readRdramProbeU32(rdram, xmenCallObject + 0x0BE0u);
            const uint32_t activeObject = readRdramProbeU32(rdram, xmenCallObject + 0x0C08u);
            std::cerr << "[xmen-legal-manager] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " pathA=\"" << readGuestPrintableString(rdram, xmenCallObject + 0x09CCu, 128u) << "\""
                      << " pathB=\"" << readGuestPrintableString(rdram, xmenCallObject + 0x0A4Cu, 128u) << "\""
                      << " flags=0x" << readRdramProbeU32(rdram, xmenCallObject + 0x0948u)
                      << " objects=0x" << objectArray
                      << " count=0x" << objectCount
                      << " first=0x" << readRdramProbeU32(rdram, objectArray)
                      << " active=0x" << activeObject
                      << " activeVtable=0x" << readRdramProbeU32(rdram, activeObject)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " f12=" << std::dec << ctx->f[12]
                      << std::endl;
        }
    }
    static thread_local bool s_xmenLookupGlobalInitialized = false;
    static thread_local uint32_t s_xmenLookupGlobal = 0u;
    const uint32_t xmenLookupGlobal = readRdramProbeU32(rdram, 0x00749EA8u);
    if (!s_xmenLookupGlobalInitialized || xmenLookupGlobal != s_xmenLookupGlobal)
    {
        std::cerr << "[xmen-lookup-global] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " old=0x" << s_xmenLookupGlobal
                  << " value=0x" << xmenLookupGlobal
                  << " pc=0x" << ctx->pc
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
        s_xmenLookupGlobal = xmenLookupGlobal;
        s_xmenLookupGlobalInitialized = true;
    }
    if (isXmenObjectAcquireCall)
    {
        const uint32_t manager = GPR_U32(ctx, 5);
        const uint32_t managerVtable = readRdramProbeU32(rdram, manager);
        std::cerr << "[xmen-object-acquire:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " out=0x" << GPR_U32(ctx, 4)
                  << " manager=0x" << manager
                  << " managerVtable=0x" << managerVtable
                  << " slot18c=0x" << readRdramProbeU32(rdram, managerVtable + 0x18cu)
                  << " ownerField=0x" << GPR_U32(ctx, 6)
                  << " ownerValue=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 6))
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (isXmenObjectMethodCall)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t vtable = readRdramProbeU32(rdram, object);
        std::cerr << "[xmen-object-method:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " object=0x" << object
                  << " vtable=0x" << vtable
                  << " slot14=0x" << readRdramProbeU32(rdram, vtable + 0x14u)
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s0_0c=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 16) + 0x0cu)
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " f12=" << std::dec << ctx->f[12]
                  << " trace=" << formatDispatchHistory()
                  << std::endl;
    }
    if (isXmenLevelActivationCall)
    {
        std::cout << "[xmen-level-activation:enter] source=0x" << std::hex << sourcePc
                  << " manager=0x" << xmenActivationManager
                  << " old=0x" << readRdramProbeU32(rdram, xmenActivationManager + 0x0C08u)
                  << " object=0x" << xmenActivationObject
                  << " objectVtable=0x" << readRdramProbeU32(rdram, xmenActivationObject)
                  << " objectFlags=0x" << readRdramProbeU32(rdram, xmenActivationObject + 0x00u)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    const bool isXmenFrameTimingCall =
        isCall && (sourcePc == 0x00158CB8u || sourcePc == 0x0014B884u || sourcePc == 0x0014B89Cu ||
                   sourcePc == 0x0014B8F8u || sourcePc == 0x0014B934u);
    const bool isXmenFontPoolAllocation =
        isCall && sourcePc == 0x0030AA38u;
    const bool isXmenPlatformInitCall =
        isCall && sourcePc >= 0x003AF824u && sourcePc <= 0x003AF8B4u;
    const bool isXmenGraphicsBootstrapCall =
        isCall && sourcePc >= 0x00130BBCu && sourcePc <= 0x00130F04u;
    const auto ensureXmenFixedPool = [&](uint32_t blockListAddress,
                                         uint32_t freeListAddress,
                                         uint32_t blockCountAddress,
                                         uint32_t freeCountAddress,
                                         uint32_t nodeSize,
                                         uint32_t nodeCount,
                                         const char *tag) -> bool
    {
        if (readRdramProbeU32(rdram, freeCountAddress) != 0u)
        {
            return true;
        }

        constexpr uint32_t kPoolAllocationSize = 0x1604u;
        const uint32_t block = ps2xGuestBumpAlloc(rdram, kPoolAllocationSize, 4u);
        if (block == 0u)
        {
            return false;
        }

        uint32_t freeHead = readRdramProbeU32(rdram, freeListAddress);
        writeRdramProbeU32(rdram, block, readRdramProbeU32(rdram, blockListAddress));
        writeRdramProbeU32(rdram, blockListAddress, block);
        for (uint32_t i = 0u; i < nodeCount; ++i)
        {
            const uint32_t node = block + 4u + i * nodeSize;
            writeRdramProbeU32(rdram, node, freeHead);
            freeHead = node;
        }
        writeRdramProbeU32(rdram, freeListAddress, freeHead);
        writeRdramProbeU32(rdram, freeCountAddress, nodeCount);
        writeRdramProbeU32(rdram, blockCountAddress,
                           readRdramProbeU32(rdram, blockCountAddress) + nodeCount);
        std::cerr << "[" << tag << ":grow] block=0x" << std::hex << block
                  << " head=0x" << freeHead << std::dec << std::endl;
        return true;
    };

    if (isCall && targetPc == 0x0030B860u)
    {
        // This function contains an inlined copy of the 0x2c-node pool grow loop.
        ensureXmenFixedPool(0x00759A8Cu, 0x00759A90u, 0x00759A94u, 0x00759A9Cu,
                            0x2Cu, 128u, "xmen-font-node-pool");
    }
    if (isCall && (targetPc == 0x0030A7F0u || targetPc == 0x0030BDB0u || targetPc == 0x0030BEE0u))
    {
        // The 0x58-node allocator is emitted both standalone and inside a later vtable aggregate.
        ensureXmenFixedPool(0x00759ABCu, 0x00759AC0u, 0x00759AC4u, 0x00759ACCu,
                            0x58u, 64u, "xmen-font-tree-pool");
    }
    if (isCall && targetPc == 0x0030AA10u)
    {
        constexpr uint32_t kBlockListAddress = 0x00759A8Cu;
        constexpr uint32_t kFreeListAddress = 0x00759A90u;
        constexpr uint32_t kBlockCountAddress = 0x00759A94u;
        constexpr uint32_t kLiveCountAddress = 0x00759A98u;
        constexpr uint32_t kFreeCountAddress = 0x00759A9Cu;
        constexpr uint32_t kNodeSize = 0x2Cu;
        constexpr uint32_t kNodeCount = 128u;

        if (!ensureXmenFixedPool(kBlockListAddress, kFreeListAddress, kBlockCountAddress,
                                 kFreeCountAddress, kNodeSize, kNodeCount,
                                 "xmen-font-node-pool"))
        {
            SET_GPR_U32(ctx, 2, 0u);
            ctx->pc = fallthroughPc;
            return true;
        }

        uint32_t freeHead = readRdramProbeU32(rdram, kFreeListAddress);
        const uint32_t freeCount = readRdramProbeU32(rdram, kFreeCountAddress);
        const uint32_t result = freeHead;
        freeHead = readRdramProbeU32(rdram, result);
        writeRdramProbeU32(rdram, kFreeListAddress, freeHead);
        writeRdramProbeU32(rdram, kFreeCountAddress, freeCount - 1u);
        writeRdramProbeU32(rdram, kLiveCountAddress,
                           readRdramProbeU32(rdram, kLiveCountAddress) + 1u);
        SET_GPR_U32(ctx, 2, result);
        ctx->pc = fallthroughPc;
        return true;
    }
    if (isCall && sourcePc == 0x0030D54Cu && targetPc == 0x00405510u)
    {
        static std::atomic<uint32_t> s_xmenFontListTraceCount{0u};
        const uint32_t traceIndex = s_xmenFontListTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 192u)
        {
            const uint32_t key = GPR_U32(ctx, 4);
            const uint32_t node = key - 4u;
            const uint32_t query = GPR_U32(ctx, 5);
            std::cerr << "[xmen-font-list] index=" << traceIndex
                      << " node=0x" << std::hex << node
                      << " next=0x" << readRdramProbeU32(rdram, node + 0x28u)
                      << " sentinel=0x" << GPR_U32(ctx, 18)
                      << " key=[0x" << readRdramProbeU32(rdram, key)
                      << ",0x" << readRdramProbeU32(rdram, key + 4u)
                      << ",0x" << readRdramProbeU32(rdram, key + 8u)
                      << "] query=[0x" << readRdramProbeU32(rdram, query)
                      << ",0x" << readRdramProbeU32(rdram, query + 4u)
                      << ",0x" << readRdramProbeU32(rdram, query + 8u)
                      << "] sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x0030B080u)
    {
        static std::atomic<uint32_t> s_xmenFontChildLookupEnterCount{0u};
        const uint32_t traceIndex = s_xmenFontChildLookupEnterCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 32u)
        {
            const uint32_t root = GPR_U32(ctx, 4);
            const uint32_t key = GPR_U32(ctx, 5);
            std::cerr << "[xmen-font-child-lookup:enter] index=" << traceIndex
                      << " source=0x" << std::hex << sourcePc
                      << " root=0x" << root
                      << " fields=[0x" << readRdramProbeU32(rdram, root)
                      << ",0x" << readRdramProbeU32(rdram, root + 4u)
                      << ",0x" << readRdramProbeU32(rdram, root + 8u)
                      << ",0x" << readRdramProbeU32(rdram, root + 0xCu)
                      << ",0x" << readRdramProbeU32(rdram, root + 0x28u)
                      << "] key=[0x" << readRdramProbeU32(rdram, key)
                      << ",0x" << readRdramProbeU32(rdram, key + 4u)
                      << ",0x" << readRdramProbeU32(rdram, key + 8u)
                      << "]" << std::dec << std::endl;
        }
    }
    if (kind == GuestBranchKind::Return && sourcePc == 0x0030B0ECu)
    {
        static std::atomic<uint32_t> s_xmenFontChildLookupReturnCount{0u};
        const uint32_t traceIndex = s_xmenFontChildLookupReturnCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 32u)
        {
            const uint32_t result = GPR_U32(ctx, 2);
            std::cerr << "[xmen-font-child-lookup:return] index=" << traceIndex
                      << " target=0x" << std::hex << targetPc
                      << " result=0x" << result
                      << " fields=[0x" << readRdramProbeU32(rdram, result)
                      << ",0x" << readRdramProbeU32(rdram, result + 4u)
                      << ",0x" << readRdramProbeU32(rdram, result + 8u)
                      << ",0x" << readRdramProbeU32(rdram, result + 0xCu)
                      << ",0x" << readRdramProbeU32(rdram, result + 0x28u)
                      << "]" << std::dec << std::endl;
        }
    }
    if (isXmenGraphicsBootstrapCall)
    {
        std::cerr << "[xmen-graphics-bootstrap:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " return=0x" << fallthroughPc
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << std::dec << std::endl;
    }
    if (isXmenPlatformInitCall)
    {
        std::cerr << "[xmen-platform-init:enter] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " return=0x" << fallthroughPc
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << std::dec << std::endl;
    }
    if (isXmenFontPoolAllocation)
    {
        const uint32_t allocator = GPR_U32(ctx, 4);
        std::cerr << "[xmen-font-pool-alloc:enter] target=0x" << std::hex << targetPc
                  << " allocator=0x" << allocator
                  << " allocatorVtable=0x" << readRdramProbeU32(rdram, allocator)
                  << " size=0x" << GPR_U32(ctx, 5)
                  << " alignment=0x" << GPR_U32(ctx, 6)
                  << " fields=[0x" << readRdramProbeU32(rdram, allocator + 4u)
                  << ",0x" << readRdramProbeU32(rdram, allocator + 8u)
                  << ",0x" << readRdramProbeU32(rdram, allocator + 12u)
                  << ",0x" << readRdramProbeU32(rdram, allocator + 16u)
                  << "]" << std::dec << std::endl;
    }
    if (isCall && (sourcePc == 0x0014CB28u || sourcePc == 0x0014CB3Cu))
    {
        static std::atomic<uint32_t> s_xmenConcatDispatchTraceCount{0u};
        const uint32_t count = s_xmenConcatDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-concat-dispatch:enter] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << readRdramProbeU32(rdram, object)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (isXmenFontPoolAllocation && targetPc == 0x0030E400u)
    {
        const uint32_t size = GPR_U32(ctx, 5);
        const uint32_t requestedAlignment = GPR_U32(ctx, 6);
        const uint32_t alignment =
            requestedAlignment != 0u && (requestedAlignment & (requestedAlignment - 1u)) == 0u
                ? requestedAlignment
                : 16u;
        const uint32_t result = ps2xGuestBumpAlloc(rdram, size, alignment);
        SET_GPR_U32(ctx, 2, result);
        ctx->pc = fallthroughPc;
        std::cerr << "[xmen-font-pool-alloc:compat] size=0x" << std::hex << size
                  << " alignment=0x" << alignment
                  << " result=0x" << result
                  << std::dec << std::endl;
        return true;
    }
    if (isXmenFrameTimingCall)
    {
        static std::atomic<uint32_t> s_xmenFrameTimingEnterCount{0u};
        const uint32_t count = s_xmenFrameTimingEnterCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 96u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-frame-timing:enter] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << readRdramProbeU32(rdram, object)
                      << " object4=0x" << readRdramProbeU32(rdram, object + 4u)
                      << " object8=0x" << readRdramProbeU32(rdram, object + 8u)
                      << " v0=0x" << GPR_U64(ctx, 2)
                      << " f0=" << std::dec << ctx->f[0]
                      << " f1=" << ctx->f[1]
                      << " f2=" << ctx->f[2]
                      << " f20=" << ctx->f[20]
                      << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x0014B884u || sourcePc == 0x0014B89Cu ||
                   sourcePc == 0x0014B8F8u))
    {
        static std::atomic<uint32_t> s_xmenFrameClockSourceTraceCount{0u};
        const uint32_t count = s_xmenFrameClockSourceTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u || targetPc == 0u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            const uint32_t inner = readRdramProbeU32(rdram, object + 4u);
            const uint32_t innerVtable = readRdramProbeU32(rdram, inner);
            std::cerr << "[xmen-frame-clock-source] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << vtable
                      << " slot20=0x" << readRdramProbeU32(rdram, vtable + 0x20u)
                      << " inner=0x" << inner
                      << " innerVtable=0x" << innerVtable
                      << " innerSlot68=0x" << readRdramProbeU32(rdram, innerVtable + 0x68u)
                      << " singletonVtable=0x" << readRdramProbeU32(rdram, 0x00742FC0u)
                      << " singletonInner=0x" << readRdramProbeU32(rdram, 0x00742FC4u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x0014AE50u && sourcePc < 0x0014B200u)
    {
        static std::atomic<uint32_t> s_xmenBootstrapCallTraceCount{0u};
        const uint32_t count = s_xmenBootstrapCallTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            std::cerr << "[xmen-bootstrap-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x0014B920u && sourcePc < 0x0014BFA0u)
    {
        static std::atomic<uint32_t> s_xmenMainLoopCallTraceCount{0u};
        const uint32_t count = s_xmenMainLoopCallTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 96u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-main-loop-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " object=0x" << object
                      << " vtable=0x" << readRdramProbeU32(rdram, object)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " f0=" << std::dec << ctx->f[0]
                      << " f12=" << ctx->f[12]
                      << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x0031F000u && sourcePc < 0x0031F600u)
    {
        static std::atomic<uint32_t> s_xmenLegalMenuUpdateTraceCount{0u};
        const uint32_t count = s_xmenLegalMenuUpdateTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 768u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[xmen-legal-menu-update-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " object=0x" << object
                      << " vtable=0x" << vtable
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << std::dec << std::endl;
        }
    }
    if (isCall && ((sourcePc >= 0x005C1AA0u && sourcePc < 0x005C1B80u) ||
                   (sourcePc >= 0x0034AB00u && sourcePc < 0x0034AC10u)))
    {
        static std::atomic<uint32_t> s_xmenLegalViewerTraversalTraceCount{0u};
        const uint32_t count = s_xmenLegalViewerTraversalTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 1024u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-legal-viewer-traversal-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " object=0x" << object
                      << " vtable=0x" << readRdramProbeU32(rdram, object)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x0034AB28u)
    {
        static std::atomic<uint32_t> s_xmenLegalViewerContainerTraceCount{0u};
        const uint32_t count = s_xmenLegalViewerContainerTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 4u)
        {
            const uint32_t viewer = GPR_U32(ctx, 17);
            const uint32_t container = viewer + 0x290u;
            std::cerr << "[xmen-legal-viewer-container] index=" << std::dec << count
                      << " viewer=0x" << std::hex << viewer
                      << " container=0x" << container;
            for (uint32_t offset = 0u; offset < 0x40u; offset += 4u)
            {
                std::cerr << " c" << std::dec << offset
                          << "=0x" << std::hex << readRdramProbeU32(rdram, container + offset);
            }
            for (uint32_t offset = 0xEC0u; offset < 0xEE0u; offset += 4u)
            {
                std::cerr << " o" << std::dec << offset
                          << "=0x" << std::hex << readRdramProbeU32(rdram, viewer + offset);
            }
            std::cerr << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x005C1470u ||
                   (sourcePc >= 0x005C1470u && sourcePc < 0x005C1770u)))
    {
        static std::atomic<uint32_t> s_xmenImageViewerParseTraceCount{0u};
        const uint32_t count = s_xmenImageViewerParseTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 96u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-image-viewer-parse-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " object=0x" << object
                      << " objectWord0=0x" << readRdramProbeU32(rdram, object)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x005C1900u ||
                   (sourcePc >= 0x005C1900u && sourcePc < 0x005C1AA0u)))
    {
        static std::atomic<uint32_t> s_xmenImageLoadTraceCount{0u};
        const uint32_t count = s_xmenImageLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 512u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            std::cerr << "[xmen-image-load-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " object=0x" << object
                      << " objectWord0=0x" << readRdramProbeU32(rdram, object)
                      << " a1=0x" << a1
                      << " a1Text=\"" << readGuestPrintableString(rdram, a1, 96u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x002029E0u ||
                   sourcePc == 0x00202A98u ||
                   sourcePc == 0x00202B40u ||
                   sourcePc == 0x00202B74u ||
                   sourcePc == 0x00202BC0u ||
                   sourcePc == 0x00202C40u ||
                   sourcePc == 0x00202CA4u))
    {
        static std::atomic<uint32_t> s_xmenIgbDataCopyTraceCount{0u};
        const uint32_t count = s_xmenIgbDataCopyTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 512u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t a2 = GPR_U32(ctx, 6);
            const uint32_t s1 = GPR_U32(ctx, 17);
            const uint32_t s5 = GPR_U32(ctx, 21);
            std::cerr << "[xmen-igb-data-copy] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << a0
                      << " a1=0x" << a1
                      << " a2=0x" << a2
                      << " a0w0=0x" << readRdramProbeU32(rdram, a0)
                      << " a1w0=0x" << readRdramProbeU32(rdram, a1)
                      << " a2w0=0x" << readRdramProbeU32(rdram, a2)
                      << " s1=0x" << s1
                      << " s1vtable=0x" << readRdramProbeU32(rdram, s1)
                      << " s1slotC8=0x" << readRdramProbeU32(rdram, readRdramProbeU32(rdram, s1) + 0xC8u)
                      << " s5=0x" << s5
                      << " sourceBase=0x" << readRdramProbeU32(rdram, s5 + 0x10Cu)
                      << " sourceOffset=0x" << readRdramProbeU32(rdram, s5 + 0x104u)
                      << " sourceRemaining=0x" << readRdramProbeU32(rdram, s5 + 0x108u)
                      << " destinationOffset=0x" << readRdramProbeU32(rdram, s5 + 0x110u)
                      << " destinationRemaining=0x" << readRdramProbeU32(rdram, s5 + 0x114u)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x00203100u ||
                   sourcePc == 0x001FDB5Cu ||
                   sourcePc == 0x0020318Cu ||
                   sourcePc == 0x002031B8u))
    {
        static std::atomic<uint32_t> s_xmenIgbSyncCopyTraceCount{0u};
        const uint32_t count = s_xmenIgbSyncCopyTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t a2 = GPR_U32(ctx, 6);
            const uint32_t descriptor =
                targetPc == 0x00203100u || sourcePc == 0x001FDB5Cu
                    ? a0
                    : 0u;
            const uint32_t context =
                targetPc == 0x00203100u || sourcePc == 0x001FDB5Cu
                    ? a1
                    : GPR_U32(ctx, 21);
            std::cerr << "[xmen-igb-sync-copy] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << a0
                      << " a1=0x" << a1
                      << " a2=0x" << a2
                      << " a0w0=0x" << readRdramProbeU32(rdram, a0)
                      << " a0w1=0x" << readRdramProbeU32(rdram, a0 + 4u)
                      << " a1w0=0x" << readRdramProbeU32(rdram, a1)
                      << " a1w1=0x" << readRdramProbeU32(rdram, a1 + 4u)
                      << " descriptor=0x" << descriptor
                      << " descriptorData=0x" << readRdramProbeU32(rdram, descriptor + 0x18u)
                      << " descriptorSize=0x" << readRdramProbeU32(rdram, descriptor + 0x1Cu)
                      << " context=0x" << context
                      << " contextSourceOffset=0x" << readRdramProbeU32(rdram, context + 0x104u)
                      << " contextSourceRemaining=0x" << readRdramProbeU32(rdram, context + 0x108u)
                      << " contextSource=0x" << readRdramProbeU32(rdram, context + 0x10Cu)
                      << " contextDestOffset=0x" << readRdramProbeU32(rdram, context + 0x110u)
                      << " contextDestRemaining=0x" << readRdramProbeU32(rdram, context + 0x114u)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << " s5=0x" << GPR_U32(ctx, 21)
                      << " s6=0x" << GPR_U32(ctx, 22)
                      << " s7=0x" << GPR_U32(ctx, 23)
                      << " fp=0x" << GPR_U32(ctx, 30)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x001FC0D0u ||
                   sourcePc == 0x001FC10Cu ||
                   sourcePc == 0x001FC15Cu ||
                   sourcePc == 0x001FC16Cu ||
                   sourcePc == 0x001FC180u ||
                   sourcePc == 0x001FC1C4u ||
                   sourcePc == 0x001FC1D4u ||
                   sourcePc == 0x001FC1E8u ||
                   sourcePc == 0x001FC224u ||
                   sourcePc == 0x001FC234u ||
                   sourcePc == 0x001FC264u))
    {
        static std::atomic<uint32_t> s_xmenIgbCopyPassTraceCount{0u};
        const uint32_t count = s_xmenIgbCopyPassTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t s0 = GPR_U32(ctx, 16);
            const uint32_t s2 = GPR_U32(ctx, 18);
            const uint32_t pass = targetPc == 0x001FC0D0u ? a1 : s2;
            const uint32_t candidate =
                sourcePc == 0x001FC15Cu ||
                sourcePc == 0x001FC1C4u ||
                sourcePc == 0x001FC224u
                    ? a0
                    : 0u;
            const uint32_t candidateVtable = readRdramProbeU32(rdram, candidate);
            std::cerr << "[xmen-igb-copy-pass] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << a0
                      << " a1=0x" << a1
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << s0
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << s2
                      << " pass=0x" << pass
                      << " passCount=0x" << readRdramProbeU32(rdram, pass + 0x08u)
                      << " passItems=0x" << readRdramProbeU32(rdram, pass + 0x10u)
                      << " passMode=0x" << readRdramProbeU32(rdram, pass + 0x64u)
                      << " passSource=0x" << readRdramProbeU32(rdram, pass + 0x10Cu)
                      << " passSourceOffset=0x" << readRdramProbeU32(rdram, pass + 0x104u)
                      << " passSourceRemaining=0x" << readRdramProbeU32(rdram, pass + 0x108u)
                      << " expectedType=0x" << readRdramProbeU32(rdram, 0x00745C00u)
                      << " candidate=0x" << candidate
                      << " candidateVtable=0x" << candidateVtable
                      << " candidateSlot80=0x" << readRdramProbeU32(rdram, candidateVtable + 0x80u)
                      << " candidateSlot84=0x" << readRdramProbeU32(rdram, candidateVtable + 0x84u)
                      << " candidateSlot88=0x" << readRdramProbeU32(rdram, candidateVtable + 0x88u)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x00525000u ||
                   (sourcePc >= 0x00525000u && sourcePc < 0x005257D0u)))
    {
        static std::atomic<uint32_t> s_xmenResourceLoadTraceCount{0u};
        const uint32_t count = s_xmenResourceLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t a2 = GPR_U32(ctx, 6);
            const uint32_t a3 = GPR_U32(ctx, 7);
            const uint32_t objectVtable = sourcePc == 0x00525270u
                ? readRdramProbeU32(rdram, a0)
                : 0u;
            const uint32_t objectTypeGetter = sourcePc == 0x00525270u
                ? readRdramProbeU32(rdram, objectVtable + 0x58u)
                : 0u;
            const uint32_t objectType = sourcePc == 0x00525270u
                ? readRdramProbeU32(rdram, 0x007461C8u)
                : 0u;
            const uint32_t objectTypeParent = readRdramProbeU32(rdram, objectType + 0x38u);
            const uint32_t objectTypeGrandparent = readRdramProbeU32(rdram, objectTypeParent + 0x38u);
            const uint32_t selectedObject = sourcePc == 0x00525388u
                ? readRdramProbeU32(rdram, GPR_U32(ctx, 29) + 0x4E8u)
                : a0;
            std::cerr << "[xmen-resource-load-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " a0=0x" << a0
                      << " a0Text=\"" << readGuestPrintableString(rdram, a0, 96u) << "\""
                      << " a1=0x" << a1
                      << " a1Text=\"" << readGuestPrintableString(rdram, a1, 96u) << "\""
                      << " a2=0x" << a2
                      << " a2Text=\"" << readGuestPrintableString(rdram, a2, 96u) << "\""
                      << " a3=0x" << a3
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " v1=0x" << GPR_U32(ctx, 3)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << " s5=0x" << GPR_U32(ctx, 21)
                      << " s6=0x" << GPR_U32(ctx, 22)
                      << " s7=0x" << GPR_U32(ctx, 23)
                      << " fp=0x" << GPR_U32(ctx, 30)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " objectVtable=0x" << objectVtable
                      << " objectTypeGetter=0x" << objectTypeGetter
                      << " objectType=0x" << objectType
                      << " objectTypeParent=0x" << objectTypeParent
                      << " objectTypeGrandparent=0x" << objectTypeGrandparent
                      << " selectedObject=0x" << selectedObject;
            if (sourcePc == 0x00525270u || sourcePc == 0x00525388u)
            {
                for (uint32_t offset = 0u; offset < 0x40u; offset += 4u)
                {
                    std::cerr << " object" << std::dec << offset
                              << "=0x" << std::hex
                              << readRdramProbeU32(rdram, selectedObject + offset);
                }
            }
            std::cerr
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x004B9AA0u && sourcePc < 0x004B9E60u)
    {
        static std::atomic<uint32_t> s_xmenPackageFindTraceCount{0u};
        const uint32_t count = s_xmenPackageFindTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            std::cerr << "[xmen-package-find-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " a0=0x" << a0
                      << " a0Text=\"" << readGuestPrintableString(rdram, a0, 96u) << "\""
                      << " a1=0x" << a1
                      << " a1Text=\"" << readGuestPrintableString(rdram, a1, 96u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x00525388u)
    {
        static std::atomic<uint32_t> s_xmenPackageCollectionTraceCount{0u};
        const uint32_t count = s_xmenPackageCollectionTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t sp = GPR_U32(ctx, 29);
            const uint32_t packageHolder = readRdramProbeU32(rdram, sp + 0xC0u);
            const uint32_t collection = GPR_U32(ctx, 18);
            const uint32_t collectionItems = readRdramProbeU32(rdram, collection + 0x10u);
            std::cerr << "[xmen-package-collection] index=" << std::dec << count
                      << " holder=0x" << std::hex << packageHolder
                      << " collection=0x" << collection;
            for (uint32_t offset = 0u; offset < 0x30u; offset += 4u)
            {
                std::cerr << " h" << std::dec << offset
                          << "=0x" << std::hex << readRdramProbeU32(rdram, packageHolder + offset)
                          << " c" << std::dec << offset
                          << "=0x" << std::hex << readRdramProbeU32(rdram, collection + offset);
            }
            std::cerr << " item0=0x" << std::hex << readRdramProbeU32(rdram, collectionItems)
                      << " item1=0x" << readRdramProbeU32(rdram, collectionItems + 4u)
                      << " item2=0x" << readRdramProbeU32(rdram, collectionItems + 8u)
                      << " item3=0x" << readRdramProbeU32(rdram, collectionItems + 12u)
                      << " result=0x" << readRdramProbeU32(rdram, sp + 0x4ECu)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x0021D220u && sourcePc < 0x0021D570u)
    {
        static std::atomic<uint32_t> s_xmenProviderLoadTraceCount{0u};
        const uint32_t count = s_xmenProviderLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            const uint32_t provider = GPR_U32(ctx, 17);
            std::cerr << "[xmen-provider-load-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " a0=0x" << a0
                      << " a0Text=\"" << readGuestPrintableString(rdram, a0, 96u) << "\""
                      << " a1=0x" << a1
                      << " a1Text=\"" << readGuestPrintableString(rdram, a1, 96u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << provider
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " providerMode=0x" << readRdramProbeU32(rdram, provider + 0x1Cu)
                      << " providerMethod=0x" << readRdramProbeU32(
                             rdram, readRdramProbeU32(rdram, readRdramProbeU32(rdram, provider + 0x20u)) + 0x68u)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x0021D570u && sourcePc < 0x0021D670u)
    {
        static std::atomic<uint32_t> s_xmenPackageLoadTraceCount{0u};
        const uint32_t count = s_xmenPackageLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t a0 = GPR_U32(ctx, 4);
            const uint32_t a1 = GPR_U32(ctx, 5);
            std::cerr << "[xmen-package-load-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " a0=0x" << a0
                      << " a0Text=\"" << readGuestPrintableString(rdram, a0, 96u) << "\""
                      << " a1=0x" << a1
                      << " a1Text=\"" << readGuestPrintableString(rdram, a1, 96u) << "\""
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc >= 0x001FD0B0u && sourcePc < 0x001FD6E4u)
    {
        const uint32_t path = GPR_U32(ctx, 17);
        const std::string pathText = readGuestPrintableString(rdram, path, 128u);
        if (pathText.find("maps/menu/main_back.igb") != std::string::npos)
        {
            const uint32_t package = GPR_U32(ctx, 18);
            g_xmenMainBackIgbPackage.store(package, std::memory_order_relaxed);
            const uint32_t objectTable = readRdramProbeU32(rdram, package + 0x2Cu);
            const uint32_t objectEntries = readRdramProbeU32(rdram, objectTable + 0x10u);
            g_xmenMainBackIgbObjectEntries.store(objectEntries, std::memory_order_relaxed);
            static std::atomic<uint32_t> s_xmenMainBackIgbLoadTraceCount{0u};
            const uint32_t count =
                s_xmenMainBackIgbLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 96u)
            {
                const uint32_t entries = readRdramProbeU32(rdram, package + 0x10u);
                std::cout << "[xmen-main-back-igb-load] index=" << std::dec << count
                          << " tick=" << xmenBranchTick
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " path=\"" << pathText << "\""
                          << " package=0x" << package
                          << " entries=0x" << entries
                          << " objectTable=0x" << objectTable
                          << " objectEntries=0x" << objectEntries
                          << " state=0x" << readRdramProbeU32(rdram, package + 0x64u)
                          << " object31=0x" << readRdramProbeU32(rdram, entries + 31u * 4u)
                          << " object116=0x" << readRdramProbeU32(rdram, entries + 116u * 4u)
                          << " object179=0x" << readRdramProbeU32(rdram, entries + 179u * 4u)
                          << " object210=0x" << readRdramProbeU32(rdram, entries + 210u * 4u)
                          << " savedRa=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 29) + 0x30u)
                          << " wrapperRa=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 29) + 0x60u)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << std::dec << std::endl;
            }
        }
        if (pathText.find("textures/legal/legal_ps2") != std::string::npos)
        {
            static std::atomic<uint32_t> s_xmenIgbLoadTraceCount{0u};
            const uint32_t count = s_xmenIgbLoadTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 128u)
            {
                const uint32_t a0 = GPR_U32(ctx, 4);
                const uint32_t package = GPR_U32(ctx, 18);
                const uint32_t buffer = readRdramProbeU32(rdram, package + 0x10Cu);
                std::cerr << "[xmen-igb-load-call] index=" << std::dec << count
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " return=0x" << fallthroughPc
                          << " kind=" << describeGuestBranchKind(kind)
                          << " a0=0x" << a0
                          << " a0Text=\"" << readGuestPrintableString(rdram, a0, 96u) << "\""
                          << " a1=0x" << GPR_U32(ctx, 5)
                          << " a2=0x" << GPR_U32(ctx, 6)
                          << " a3=0x" << GPR_U32(ctx, 7)
                          << " v0=0x" << GPR_U32(ctx, 2)
                          << " v1=0x" << GPR_U32(ctx, 3)
                          << " s0=0x" << GPR_U32(ctx, 16)
                          << " s1=0x" << path
                          << " s1Text=\"" << pathText << "\""
                          << " s2=0x" << package
                          << " package60=0x" << readRdramProbeU32(rdram, package + 0x60u)
                          << " buffer=0x" << buffer
                          << " buffer00=0x" << readRdramProbeU32(rdram, buffer + 0x00u)
                          << " buffer04=0x" << readRdramProbeU32(rdram, buffer + 0x04u)
                          << " buffer08=0x" << readRdramProbeU32(rdram, buffer + 0x08u)
                          << " buffer0c=0x" << readRdramProbeU32(rdram, buffer + 0x0Cu)
                          << " buffer10=0x" << readRdramProbeU32(rdram, buffer + 0x10u)
                          << " buffer14=0x" << readRdramProbeU32(rdram, buffer + 0x14u)
                          << " buffer18=0x" << readRdramProbeU32(rdram, buffer + 0x18u)
                          << " buffer1c=0x" << readRdramProbeU32(rdram, buffer + 0x1Cu)
                          << " buffer20=0x" << readRdramProbeU32(rdram, buffer + 0x20u)
                          << " buffer24=0x" << readRdramProbeU32(rdram, buffer + 0x24u)
                          << " buffer28=0x" << readRdramProbeU32(rdram, buffer + 0x28u)
                          << " buffer2c=0x" << readRdramProbeU32(rdram, buffer + 0x2Cu)
                          << " sp=0x" << GPR_U32(ctx, 29)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << std::dec << std::endl;
            }
        }
    }
    if (isCall && ((sourcePc >= 0x001F9990u && sourcePc < 0x001F9C30u) ||
                   (sourcePc >= 0x001FDA00u && sourcePc < 0x001FDB10u)))
    {
        const uint32_t package = sourcePc < 0x001FDA00u ? GPR_U32(ctx, 18) : GPR_U32(ctx, 17);
        const uint32_t path = readRdramProbeU32(rdram, package + 0x14u);
        const std::string pathText = readGuestPrintableString(rdram, path, 128u);
        if (pathText.find("textures/legal/legal_ps2") != std::string::npos)
        {
            static std::atomic<uint32_t> s_xmenIgbPrepareTraceCount{0u};
            const uint32_t count = s_xmenIgbPrepareTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 128u)
            {
                const uint32_t a0 = GPR_U32(ctx, 4);
                std::cerr << "[xmen-igb-prepare-call] index=" << std::dec << count
                          << " source=0x" << std::hex << sourcePc
                          << " target=0x" << targetPc
                          << " return=0x" << fallthroughPc
                          << " kind=" << describeGuestBranchKind(kind)
                          << " package=0x" << package
                          << " path=\"" << pathText << "\""
                          << " a0=0x" << a0
                          << " a0Text=\"" << readGuestPrintableString(rdram, a0, 96u) << "\""
                          << " a1=0x" << GPR_U32(ctx, 5)
                          << " a2=0x" << GPR_U32(ctx, 6)
                          << " a3=0x" << GPR_U32(ctx, 7)
                          << " v0=0x" << GPR_U32(ctx, 2)
                          << " v1=0x" << GPR_U32(ctx, 3)
                          << " s0=0x" << GPR_U32(ctx, 16)
                          << " s1=0x" << GPR_U32(ctx, 17)
                          << " s2=0x" << GPR_U32(ctx, 18)
                          << " s3=0x" << GPR_U32(ctx, 19)
                          << " sp=0x" << GPR_U32(ctx, 29)
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << std::dec << std::endl;
            }
        }
    }
    if (isCall && targetPc == 0x001EB930u)
    {
        const uint32_t table = GPR_U32(ctx, 4);
        const uint32_t index = GPR_U32(ctx, 5);
        const uint32_t objectTable = readRdramProbeU32(rdram, table + 0x2Cu);
        if (table == g_xmenMainBackIgbPackage.load(std::memory_order_relaxed) &&
            objectTable != 0u &&
            (index == 10u || index == 31u || index == 92u || index == 116u ||
             index == 119u || index == 129u || index == 167u || index == 179u ||
             index == 183u || index == 210u || index == 213u || index == 233u ||
             index == 301u || index == 318u || index == 334u || index == 336u ||
             index == 341u || index == 366u || index == 367u || index == 385u ||
             index == 390u))
        {
            static std::atomic<uint32_t> s_xmenMainBackObjectTraceCount{0u};
            const uint32_t traceIndex =
                s_xmenMainBackObjectTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (traceIndex < 256u)
            {
                const uint32_t entries = readRdramProbeU32(rdram, table + 0x10u);
                const uint32_t element = readRdramProbeU32(rdram, entries + index * 4u);
                const uint32_t objectEntries = readRdramProbeU32(rdram, objectTable + 0x10u);
                const uint32_t object = readRdramProbeU32(rdram, objectEntries + index * 4u);
                if (objectEntries != 0u)
                {
                    g_xmenMainBackIgbObjectEntries.store(objectEntries, std::memory_order_relaxed);
                }
                std::cout << "[xmen-main-back-object] index=" << traceIndex
                          << " tick=" << xmenBranchTick
                          << " request=" << index
                          << " source=0x" << std::hex << sourcePc
                          << " table=0x" << table
                          << " entries=0x" << entries
                          << " element=0x" << element
                          << " descriptorVtable=0x" << readRdramProbeU32(rdram, element)
                          << " objectTable=0x" << objectTable
                          << " objectEntries=0x" << objectEntries
                          << " object=0x" << object
                          << " objectVtable=0x" << readRdramProbeU32(rdram, object);
                for (uint32_t offset = 0u; offset < 0xA0u; offset += 4u)
                {
                    std::cout << " object" << std::dec << offset
                              << "=0x" << std::hex
                              << readRdramProbeU32(rdram, object + offset);
                }
                std::cout << " ra=0x" << GPR_U32(ctx, 31)
                          << std::dec << std::endl;
            }
        }
        static std::atomic<uint32_t> s_xmenIgbObjectTraceCount{0u};
        const uint32_t count = s_xmenIgbObjectTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t entries = readRdramProbeU32(rdram, table + 0x10u);
            const uint32_t element = index < 0x10000u
                                         ? readRdramProbeU32(rdram, entries + index * 4u)
                                         : 0u;
            std::cerr << "[xmen-igb-object-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " table=0x" << table
                      << " request=0x" << index
                      << " table10=0x" << entries
                      << " table2c=0x" << readRdramProbeU32(rdram, table + 0x2Cu)
                      << " element=0x" << element
                      << " elementVtable=0x" << readRdramProbeU32(rdram, element)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && xmenBranchTick >= 120u && xmenBranchTick <= 180u)
    {
        constexpr std::array<uint32_t, 9> imageIndices = {
            129u, 167u, 183u, 233u, 301u, 336u, 366u, 367u, 385u,
        };
        const uint32_t imageObjectEntries =
            g_xmenMainBackIgbObjectEntries.load(std::memory_order_relaxed);
        if (imageObjectEntries != 0u)
        {
            const std::array<uint32_t, 8> candidateRegisters = {
                GPR_U32(ctx, 4), GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7),
                GPR_U32(ctx, 16), GPR_U32(ctx, 17), GPR_U32(ctx, 18), GPR_U32(ctx, 19),
            };
            constexpr std::array<const char *, 8> candidateNames = {
                "a0", "a1", "a2", "a3", "s0", "s1", "s2", "s3",
            };
            for (const uint32_t imageIndex : imageIndices)
            {
                const uint32_t imageObject =
                    readRdramProbeU32(rdram, imageObjectEntries + imageIndex * 4u);
                if (imageObject == 0u)
                    continue;

                for (size_t registerIndex = 0u;
                     registerIndex < candidateRegisters.size();
                     ++registerIndex)
                {
                    if (candidateRegisters[registerIndex] != imageObject)
                        continue;

                    static std::atomic<uint32_t> s_xmenImageCallTraceCount{0u};
                    const uint32_t traceIndex =
                        s_xmenImageCallTraceCount.fetch_add(1u, std::memory_order_relaxed);
                    if (traceIndex < 512u)
                    {
                        std::cout << "[xmen-main-back-image-call] index=" << std::dec
                                  << traceIndex
                                  << " tick=" << xmenBranchTick
                                  << " image=" << imageIndex
                                  << " register=" << candidateNames[registerIndex]
                                  << " source=0x" << std::hex << sourcePc
                                  << " target=0x" << targetPc
                                  << " return=0x" << fallthroughPc
                                  << " object=0x" << imageObject
                                  << " vtable=0x" << readRdramProbeU32(rdram, imageObject)
                                  << " a0=0x" << GPR_U32(ctx, 4)
                                  << " a1=0x" << GPR_U32(ctx, 5)
                                  << " a2=0x" << GPR_U32(ctx, 6)
                                  << " a3=0x" << GPR_U32(ctx, 7)
                                  << " ra=0x" << GPR_U32(ctx, 31)
                                  << std::dec << std::endl;
                    }
                    break;
                }
            }
        }
    }
    if (isCall && (targetPc == 0x00219750u || sourcePc == 0x0021979Cu))
    {
        static std::atomic<uint32_t> s_xmenIgbReferenceTraceCount{0u};
        const uint32_t count = s_xmenIgbReferenceTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t input = GPR_U32(ctx, 6);
            std::cerr << "[xmen-igb-reference-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << input
                      << " inputWord=0x" << readRdramProbeU32(rdram, input)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " t0=0x" << GPR_U32(ctx, 8)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (targetPc == 0x00202890u || targetPc == 0x00203280u ||
                   sourcePc == 0x0020297Cu || sourcePc == 0x002029ACu ||
                   sourcePc == 0x0020298Cu || sourcePc == 0x002029BCu ||
                   sourcePc == 0x001EB898u))
    {
        static std::atomic<uint32_t> s_xmenIgbDataAllocationTraceCount{0u};
        const uint32_t count = s_xmenIgbDataAllocationTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t descriptor = (targetPc == 0x00202890u || targetPc == 0x00203280u ||
                                         sourcePc == 0x001EB898u)
                                            ? GPR_U32(ctx, 4)
                                            : GPR_U32(ctx, 18);
            std::cerr << "[xmen-igb-data-allocation] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " descriptor=0x" << descriptor
                      << " d12=0x" << readRdramProbeU32(rdram, descriptor + 0x0Cu)
                      << " d24=0x" << readRdramProbeU32(rdram, descriptor + 0x18u)
                      << " d28=0x" << readRdramProbeU32(rdram, descriptor + 0x1Cu)
                      << " d36=0x" << readRdramProbeU32(rdram, descriptor + 0x24u)
                      << " d40=0x" << readRdramProbeU32(rdram, descriptor + 0x28u)
                      << " d44=0x" << readRdramProbeU32(rdram, descriptor + 0x2Cu)
                      << " d48=0x" << readRdramProbeU32(rdram, descriptor + 0x30u)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x00217650u)
    {
        static std::atomic<uint32_t> s_xmenIgbReferenceCallerTraceCount{0u};
        const uint32_t count = s_xmenIgbReferenceCallerTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t input = GPR_U32(ctx, 6);
            std::cerr << "[xmen-igb-reference-caller] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << input
                      << " inputWord=0x" << readRdramProbeU32(rdram, input)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " t0=0x" << GPR_U32(ctx, 8)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x002134ACu)
    {
        static std::atomic<uint32_t> s_xmenObjectTypeCodeTraceCount{0u};
        const uint32_t count = s_xmenObjectTypeCodeTraceCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t object = GPR_U32(ctx, 16);
        if (count < 16u || object == 0x00A38570u)
        {
            const uint32_t objectType = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, objectType);
            std::cerr << "[xmen-object-type-code-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " objectType=0x" << objectType
                      << " vtable=0x" << vtable
                      << " slot168=0x" << readRdramProbeU32(rdram, vtable + 0x168u)
                      << " object=0x" << object
                      << " objectWord4=0x" << readRdramProbeU32(rdram, object + 4u)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x002346ECu && targetPc == 0x0022C710u)
    {
        static std::atomic<uint32_t> s_xmenAllocatorCorruptingFreeTraceCount{0u};
        const uint32_t count = s_xmenAllocatorCorruptingFreeTraceCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t pointer = GPR_U32(ctx, 5);
        if (count < 16u || pointer < 0x00900000u)
        {
            const uint32_t allocator = GPR_U32(ctx, 4);
            std::cerr << "[xmen-allocator-corrupting-free] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " allocator=0x" << allocator
                      << " allocatorVtable=0x" << readRdramProbeU32(rdram, allocator)
                      << " pointer=0x" << pointer
                      << " pointerM12=0x" << readRdramProbeU32(rdram, pointer - 12u)
                      << " pointerM8=0x" << readRdramProbeU32(rdram, pointer - 8u)
                      << " pointerM4=0x" << readRdramProbeU32(rdram, pointer - 4u)
                      << " pointer0=0x" << readRdramProbeU32(rdram, pointer)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s4=0x" << GPR_U32(ctx, 20)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x001EAB68u || sourcePc == 0x001EAB7Cu ||
                   sourcePc == 0x001EAB90u || sourcePc == 0x001EABB0u ||
                   sourcePc == 0x001EABC8u || sourcePc == 0x001EABECu ||
                   sourcePc == 0x001EABF8u))
    {
        const uint32_t object = GPR_U32(ctx, 20);
        if (object == 0x00A38550u)
        {
            std::cerr << "[xmen-vector-reserve-s1] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s4=0x" << object
                      << " object16=0x" << readRdramProbeU32(rdram, object + 0x10u)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x001EABF8u && GPR_U32(ctx, 5) == 0x00898EF0u)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        std::cerr << "[xmen-vector-reserve-self-free] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " object=0x" << object
                  << " object0=0x" << readRdramProbeU32(rdram, object)
                  << " object4=0x" << readRdramProbeU32(rdram, object + 4u)
                  << " object8=0x" << readRdramProbeU32(rdram, object + 8u)
                  << " object12=0x" << readRdramProbeU32(rdram, object + 12u)
                  << " object16=0x" << readRdramProbeU32(rdram, object + 16u)
                  << " oldBuffer=0x" << GPR_U32(ctx, 5)
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s1=0x" << GPR_U32(ctx, 17)
                  << " s2=0x" << GPR_U32(ctx, 18)
                  << " s3=0x" << GPR_U32(ctx, 19)
                  << " s4=0x" << GPR_U32(ctx, 20)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << std::dec << std::endl;
    }
    if (isCall && GPR_U32(ctx, 5) == 0x00898EF0u &&
        (targetPc == 0x002151B0u || targetPc == 0x002336F0u))
    {
        const uint32_t object = GPR_U32(ctx, 4);
        std::cerr << "[xmen-deallocate-allocator-object] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " a0=0x" << object
                  << " a0word0=0x" << readRdramProbeU32(rdram, object)
                  << " a0word4=0x" << readRdramProbeU32(rdram, object + 4u)
                  << " a0word8=0x" << readRdramProbeU32(rdram, object + 8u)
                  << " a0word12=0x" << readRdramProbeU32(rdram, object + 12u)
                  << " a0word16=0x" << readRdramProbeU32(rdram, object + 16u)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " a3=0x" << GPR_U32(ctx, 7)
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s1=0x" << GPR_U32(ctx, 17)
                  << " s2=0x" << GPR_U32(ctx, 18)
                  << " s3=0x" << GPR_U32(ctx, 19)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << std::dec << std::endl;
    }
    if (isCall && sourcePc == 0x002BD4D0u && GPR_U32(ctx, 18) == 0x00A38520u)
    {
        const uint32_t objectType = GPR_U32(ctx, 18);
        const uint32_t vtable = readRdramProbeU32(rdram, objectType);
        std::cerr << "[xmen-resource-reference-virtual] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " objectType=0x" << objectType
                  << " vtable=0x" << vtable
                  << " slot60=0x" << readRdramProbeU32(rdram, vtable + 0x60u)
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s1=0x" << GPR_U32(ctx, 17)
                  << " s2=0x" << GPR_U32(ctx, 18)
                  << " s3=0x" << GPR_U32(ctx, 19)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << std::dec << std::endl;
    }
    if (isCall && sourcePc == 0x002BD4B8u && GPR_U32(ctx, 18) == 0x00A38520u)
    {
        const uint32_t resource = GPR_U32(ctx, 4);
        const uint32_t vtable = readRdramProbeU32(rdram, resource);
        std::cerr << "[xmen-resource-lookup-virtual] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " resource=0x" << resource
                  << " vtable=0x" << vtable
                  << " slot94=0x" << readRdramProbeU32(rdram, vtable + 0x94u)
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " a2word0=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 6))
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s1=0x" << GPR_U32(ctx, 17)
                  << " s2=0x" << GPR_U32(ctx, 18)
                  << " s3=0x" << GPR_U32(ctx, 19)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << std::dec << std::endl;
    }
    if (isCall && targetPc == 0x00215260u)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t typeWord = readRdramProbeU32(rdram, object + 4u);
        const uint32_t typeCode = (typeWord >> 24u) & 0xFFu;
        const uint32_t registryGlobal = (typeCode & 1u) != 0u ? 0x007471E0u : 0x007471D8u;
        const uint32_t registry = readRdramProbeU32(rdram, registryGlobal);
        const uint32_t registryItems = readRdramProbeU32(rdram, registry);
        const uint32_t registryIndex = typeCode >> 1u;
        const uint32_t registryEntry = readRdramProbeU32(rdram, registryItems + registryIndex * 4u);
        if (registryEntry == 0u || sourcePc == 0x001EABB0u)
        {
            static std::atomic<uint32_t> s_xmenMetaFieldDispatchTraceCount{0u};
            const uint32_t count = s_xmenMetaFieldDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 256u)
            {
                std::cerr << "[xmen-meta-field-dispatch] index=" << std::dec << count
                          << " source=0x" << std::hex << sourcePc
                          << " object=0x" << object
                          << " object0=0x" << readRdramProbeU32(rdram, object)
                          << " object4=0x" << typeWord
                          << " typeCode=0x" << typeCode
                          << " registryGlobal=0x" << registryGlobal
                          << " registry=0x" << registry
                          << " registryItems=0x" << registryItems
                          << " registryCount=0x" << readRdramProbeU32(rdram, registry + 4u)
                          << " registryIndex=0x" << registryIndex
                          << " registryEntry=0x" << registryEntry
                          << " registryEntryVtable=0x" << readRdramProbeU32(rdram, registryEntry)
                          << std::dec << std::endl;
            }
        }
    }
    if (isCall && targetPc == 0x001ED190u && sourcePc >= 0x00273BBCu && sourcePc <= 0x00273C0Cu)
    {
        const uint32_t container = GPR_U32(ctx, 4);
        const uint32_t index = GPR_U32(ctx, 5);
        const uint32_t list = readRdramProbeU32(rdram, container + 8u);
        const uint32_t items = readRdramProbeU32(rdram, list + 16u);
        std::cerr << "[xmen-render-property-selection] source=0x" << std::hex << sourcePc
                  << " container=0x" << container
                  << " index=0x" << index
                  << " text=0x" << GPR_U32(ctx, 6)
                  << " list=0x" << list
                  << " count=0x" << readRdramProbeU32(rdram, list + 8u)
                  << " items=0x" << items
                  << " selected=0x" << readRdramProbeU32(rdram, items + index * 4u)
                  << std::dec << std::endl;
    }
    if (isCall && targetPc == 0x00211C10u &&
        (sourcePc == 0x001CDF6Cu || sourcePc == 0x00217714u))
    {
        const uint32_t type = GPR_U32(ctx, 4);
        const uint32_t type1c = readRdramProbeU32(rdram, type + 0x1Cu);
        const uint32_t type28 = readRdramProbeU32(rdram, type + 0x28u);
        const uint32_t type38 = readRdramProbeU32(rdram, type + 0x38u);
        const uint32_t type54 = readRdramProbeU32(rdram, type + 0x54u);
        const uint32_t typeVtable = readRdramProbeU32(rdram, type + 0x5Cu);
        static std::atomic<uint32_t> s_xmenReflectionReferenceTypeTraceCount{0u};
        const uint32_t count = s_xmenReflectionReferenceTypeTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-reflection-reference-type] source=0x" << std::hex << sourcePc
                  << " type=0x" << type
                  << " type0=0x" << readRdramProbeU32(rdram, type)
                  << " type1c=0x" << type1c
                  << " type20=0x" << readRdramProbeU32(rdram, type + 0x20u)
                  << " type24=0x" << readRdramProbeU32(rdram, type + 0x24u)
                  << " type28=0x" << type28
                  << " type38=0x" << type38
                  << " type3c=0x" << readRdramProbeU32(rdram, type + 0x3Cu)
                  << " type40=0x" << readRdramProbeU32(rdram, type + 0x40u)
                  << " type48=0x" << readRdramProbeU32(rdram, type + 0x48u)
                  << " type54=0x" << type54
                  << " type5c=0x" << typeVtable
                  << " vtable24=0x" << readRdramProbeU32(rdram, typeVtable + 0x24u)
                  << " vtable2c=0x" << readRdramProbeU32(rdram, typeVtable + 0x2Cu)
                  << " vtable58=0x" << readRdramProbeU32(rdram, typeVtable + 0x58u)
                  << " type1cWords=[0x" << readRdramProbeU32(rdram, type1c)
                  << ",0x" << readRdramProbeU32(rdram, type1c + 4u)
                  << ",0x" << readRdramProbeU32(rdram, type1c + 8u) << ']'
                  << " type28Words=[0x" << readRdramProbeU32(rdram, type28)
                  << ",0x" << readRdramProbeU32(rdram, type28 + 4u)
                  << ",0x" << readRdramProbeU32(rdram, type28 + 8u) << ']'
                  << " type38Words=[0x" << readRdramProbeU32(rdram, type38)
                  << ",0x" << readRdramProbeU32(rdram, type38 + 4u)
                  << ",0x" << readRdramProbeU32(rdram, type38 + 8u) << ']'
                  << " type1cText=\"" << readGuestPrintableString(rdram, type1c, 96u) << '\"'
                  << " type28Text=\"" << readGuestPrintableString(rdram, type28, 96u) << '\"'
                  << " type38Text=\"" << readGuestPrintableString(rdram, type38, 96u) << '\"'
                  << " type1cNestedText=\""
                  << readGuestPrintableString(rdram, readRdramProbeU32(rdram, type1c), 96u) << '\"'
                  << " type28NestedText=\""
                  << readGuestPrintableString(rdram, readRdramProbeU32(rdram, type28), 96u) << '\"'
                  << " type38NestedText=\""
                  << readGuestPrintableString(rdram, readRdramProbeU32(rdram, type38), 96u) << '\"'
                  << " allocator=0x" << GPR_U32(ctx, 5)
                  << " field=0x" << GPR_U32(ctx, 17)
                  << " sourceObject=0x" << GPR_U32(ctx, 16)
                  << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x00213430u)
    {
        const uint32_t type = GPR_U32(ctx, 5);
        const uint32_t typeVtable = readRdramProbeU32(rdram, type + 0x5Cu);
        if (typeVtable == 0x006FC120u)
        {
            const uint32_t typeName = readRdramProbeU32(rdram, type + 0x1Cu);
            std::cerr << "[xmen-reflection-target-parent] source=0x" << std::hex << sourcePc
                      << " object=0x" << GPR_U32(ctx, 4)
                      << " type=0x" << type
                      << " typeName=0x" << typeName
                      << " typeNameText=\"" << readGuestPrintableString(rdram, typeName, 96u) << '\"'
                      << " typeSize=0x" << readRdramProbeU32(rdram, type + 0x48u)
                      << " vtable=0x" << typeVtable
                      << " vtable24=0x" << readRdramProbeU32(rdram, typeVtable + 0x24u)
                      << " vtable2c=0x" << readRdramProbeU32(rdram, typeVtable + 0x2Cu)
                      << " vtable58=0x" << readRdramProbeU32(rdram, typeVtable + 0x58u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && sourcePc == 0x00217728u && targetPc == 0x00217500u)
    {
        static std::atomic<uint32_t> s_xmenReflectionReferenceObjectTraceCount{0u};
        const uint32_t count = s_xmenReflectionReferenceObjectTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t object = GPR_U32(ctx, 6);
            std::cerr << "[xmen-reflection-reference-object] field=0x" << std::hex << GPR_U32(ctx, 4)
                      << " destination=0x" << GPR_U32(ctx, 5)
                      << " object=0x" << object
                      << " objectVtable=0x" << readRdramProbeU32(rdram, object)
                      << " object4=0x" << readRdramProbeU32(rdram, object + 4u)
                      << " object8=0x" << readRdramProbeU32(rdram, object + 8u)
                      << " object12=0x" << readRdramProbeU32(rdram, object + 12u)
                      << " object16=0x" << readRdramProbeU32(rdram, object + 16u)
                      << std::dec << std::endl;
        }
    }
    if (isCall && targetPc == 0x002144B0u)
    {
        static std::atomic<uint32_t> s_xmenIgbObjectReaderTraceCount{0u};
        const uint32_t count = s_xmenIgbObjectReaderTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            std::cerr << "[xmen-igb-object-reader] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " t0=0x" << GPR_U32(ctx, 8)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (isCall && (sourcePc == 0x00215C40u || sourcePc == 0x00215BC0u))
    {
        static std::atomic<uint32_t> s_xmenIgbDataBaseTraceCount{0u};
        const uint32_t count = s_xmenIgbDataBaseTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[xmen-igb-data-base-call] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " object=0x" << object
                      << " vtable=0x" << vtable
                      << " slot6c=0x" << readRdramProbeU32(rdram, vtable + 0x6Cu)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    static thread_local bool s_xmenHostClockActive = false;
    static thread_local uint64_t s_xmenHostClockBaseNanoseconds = 0u;
    static thread_local std::chrono::steady_clock::time_point s_xmenHostClockOrigin;
    if (!s_xmenHostClockActive && isCall && sourcePc == 0x0014B89Cu)
    {
        const double guestSeconds = std::max(0.0, static_cast<double>(ctx->f[0]));
        s_xmenHostClockBaseNanoseconds = static_cast<uint64_t>(guestSeconds * 1'000'000'000.0);
        s_xmenHostClockOrigin = std::chrono::steady_clock::now();
        s_xmenHostClockActive = true;
        std::cerr << "[xmen-host-clock:activated] baseNanoseconds="
                  << s_xmenHostClockBaseNanoseconds << std::endl;
    }
    if (s_xmenHostClockActive && isCall && targetPc == 0x00158CA0u &&
        (sourcePc == 0x0014B884u || sourcePc == 0x0014B8F8u))
    {
        const uint64_t elapsedNanoseconds = s_xmenHostClockBaseNanoseconds + static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - s_xmenHostClockOrigin)
                .count());
        ctx->f[0] = static_cast<float>(static_cast<double>(elapsedNanoseconds) / 1'000'000'000.0);
        ctx->pc = fallthroughPc;
        return true;
    }
    if (s_xmenHostClockActive && isCall && sourcePc == 0x00245198u && targetPc == 0x002FC000u)
    {
        const uint64_t elapsedNanoseconds = s_xmenHostClockBaseNanoseconds + static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - s_xmenHostClockOrigin)
                .count());
        const uint32_t resultAddress = GPR_U32(ctx, 4);
        if (resultAddress <= PS2_RAM_SIZE - sizeof(elapsedNanoseconds))
        {
            writeRdramProbeU64(rdram, resultAddress, elapsedNanoseconds);
        }

        static std::atomic<uint32_t> s_xmenHostClockLogCount{0u};
        const uint32_t count = s_xmenHostClockLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            std::cerr << "[xmen-host-clock] index=" << count
                      << " nanoseconds=" << elapsedNanoseconds
                      << " result=0x" << std::hex << resultAddress
                      << std::dec << std::endl;
        }

        ctx->pc = fallthroughPc;
        return true;
    }
    if (isCall && targetPc == 0x003887F0u)
    {
        const uint32_t guestStream = GPR_U32(ctx, 4);
        const uint32_t nextIn = readRdramProbeU32(rdram, guestStream + 0x00u);
        const uint32_t availIn = readRdramProbeU32(rdram, guestStream + 0x04u);
        const uint64_t totalIn = readRdramProbeU64(rdram, guestStream + 0x08u);
        const uint32_t nextOut = readRdramProbeU32(rdram, guestStream + 0x10u);
        const uint32_t availOut = readRdramProbeU32(rdram, guestStream + 0x14u);
        const uint64_t totalOut = readRdramProbeU64(rdram, guestStream + 0x18u);
        const bool validBuffers =
            guestStream <= PS2_RAM_SIZE - 0x40u &&
            nextIn < PS2_RAM_SIZE && availIn <= PS2_RAM_SIZE - nextIn &&
            nextOut < PS2_RAM_SIZE && availOut <= PS2_RAM_SIZE - nextOut;

        if (validBuffers)
        {
            std::lock_guard<std::mutex> lock(g_xmenHostInflateMutex);
            auto [it, inserted] = g_xmenHostInflateStates.try_emplace(guestStream);
            XmenHostInflateState &state = it->second;
            if (state.initialized && totalIn == 0u && state.stream.total_in != 0u)
            {
                inflateEnd(&state.stream);
                state = {};
            }

            int result = Z_OK;
            if (!state.initialized)
            {
                result = inflateInit2(&state.stream, -MAX_WBITS);
                state.initialized = result == Z_OK;
            }

            if (state.initialized)
            {
                constexpr uint32_t kXmenSavedReturnSlot = 0x01F12560u;
                const uint64_t outputEnd = static_cast<uint64_t>(nextOut) + availOut;
                const bool touchesSavedReturn =
                    nextOut < kXmenSavedReturnSlot + sizeof(uint32_t) &&
                    outputEnd > kXmenSavedReturnSlot;
                const uint32_t savedReturnBefore = touchesSavedReturn
                                                       ? readRdramProbeU32(rdram, kXmenSavedReturnSlot)
                                                       : 0u;
                state.stream.next_in = reinterpret_cast<Bytef *>(rdram + nextIn);
                state.stream.avail_in = availIn;
                state.stream.next_out = reinterpret_cast<Bytef *>(rdram + nextOut);
                state.stream.avail_out = availOut;
                result = inflate(&state.stream, static_cast<int>(GPR_U32(ctx, 5)));

                const uint32_t consumed = availIn - state.stream.avail_in;
                const uint32_t produced = availOut - state.stream.avail_out;
                writeRdramProbeU32(rdram, guestStream + 0x00u, nextIn + consumed);
                writeRdramProbeU32(rdram, guestStream + 0x04u, state.stream.avail_in);
                writeRdramProbeU64(rdram, guestStream + 0x08u, totalIn + consumed);
                writeRdramProbeU32(rdram, guestStream + 0x10u, nextOut + produced);
                writeRdramProbeU32(rdram, guestStream + 0x14u, state.stream.avail_out);
                writeRdramProbeU64(rdram, guestStream + 0x18u, totalOut + produced);
                writeRdramProbeU64(rdram, guestStream + 0x38u, state.stream.adler);

                static std::atomic<uint32_t> xmenHostInflateLogCount{0u};
                if (xmenHostInflateLogCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
                {
                    std::cerr << "[xmen-host-inflate] input=" << consumed << "/" << availIn
                              << " output=" << produced << "/" << availOut
                              << " result=" << result
                              << " stream=0x" << std::hex << guestStream
                              << " nextIn=0x" << nextIn
                              << " nextOut=0x" << nextOut
                              << " source=0x" << sourcePc
                              << " return=0x" << fallthroughPc
                              << " pc=0x" << ctx->pc
                              << " ra=0x" << GPR_U32(ctx, 31)
                              << " sp=0x" << GPR_U32(ctx, 29)
                              << std::dec << std::endl;
                }

                if (touchesSavedReturn)
                {
                    const GuestThread *owner = m_eeScheduler ? m_eeScheduler->currentThread() : nullptr;
                    const R5900Context *active = owner ? &owner->activeContext() : nullptr;
                    std::cerr << "[xmen-host-inflate:saved-return-overwrite] slot=0x" << std::hex
                              << kXmenSavedReturnSlot
                              << " before=0x" << savedReturnBefore
                              << " after=0x" << readRdramProbeU32(rdram, kXmenSavedReturnSlot)
                              << " thread=" << std::dec << (owner ? owner->id : 0)
                              << " invocations=" << (owner ? owner->invocations.size() : 0u)
                              << " same-context=" << (active == ctx ? 1 : 0)
                              << " active-pc=0x" << std::hex << (active ? active->pc : 0u)
                              << " active-sp=0x" << (active ? GPR_U32(active, 29) : 0u)
                              << " base-pc=0x" << (owner ? owner->context.pc : 0u)
                              << " base-sp=0x" << (owner ? getRegU32(&owner->context, 29) : 0u)
                              << std::dec << std::endl;
                }

                const uint64_t cumulativeInput = totalIn + consumed;
                const uint64_t cumulativeOutput = totalOut + produced;
                if (result == Z_STREAM_END &&
                    cumulativeInput == 142320u && cumulativeOutput == 312838u)
                {
                    s_xmenPostLegalInflateTraceIndex.store(0u, std::memory_order_relaxed);
                    s_xmenPostLegalInflateTraceBudget.store(48u, std::memory_order_relaxed);
                    std::cerr << "[xmen-post-legal-trace:armed] stream=0x" << std::hex
                              << guestStream << " input=0x" << cumulativeInput
                              << " output=0x" << cumulativeOutput
                              << std::dec << std::endl;
                }

                if (result == Z_STREAM_END || result < Z_OK)
                {
                    inflateEnd(&state.stream);
                    g_xmenHostInflateStates.erase(it);
                }
            }

            SET_GPR_S32(ctx, 2, result);
            ctx->pc = fallthroughPc;
            return true;
        }
    }
    if (isCall && (targetPc == 0x0010AC00u || targetPc == 0x0010AC30u))
    {
        std::cerr << "[xmen-irq-install-call] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " return=0x" << fallthroughPc
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " a3=0x" << GPR_U32(ctx, 7)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (isCall && targetPc == 0x00244EA0u && GPR_U32(ctx, 6) == 0x2u)
    {
        std::cerr << "[xmen-file-open-call] source=0x" << std::hex << sourcePc
                  << " return=0x" << fallthroughPc
                  << " client=0x" << GPR_U32(ctx, 4)
                  << " rpc=0x" << GPR_U32(ctx, 6)
                  << " send=0x" << GPR_U32(ctx, 7)
                  << " sendSize=0x" << GPR_U32(ctx, 8)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if ((kind == GuestBranchKind::IndirectCall || kind == GuestBranchKind::IndirectJump) &&
        targetPc >= 0x0026CA00u && targetPc < 0x00274000u)
    {
        static std::atomic<uint32_t> xmenPlatformIndirectLogCount{0u};
        if (xmenPlatformIndirectLogCount.fetch_add(1u, std::memory_order_relaxed) < 256u)
        {
            std::cerr << "[xmen-platform-indirect] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }
    if (isCall &&
        (targetPc == 0x0026DBA0u || targetPc == 0x00219EC0u ||
         targetPc == 0x001EDC70u || targetPc == 0x002DE9F0u ||
         (targetPc == 0x001FEA40u && sourcePc == 0x001EDD38u)))
    {
        std::cerr << "[xmen-render-init-path] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " return=0x" << fallthroughPc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (targetPc == 0x003B1160u && rdram)
    {
        const uint32_t sourceAddress = GPR_U32(ctx, 2);
        std::string sourcePath;
        if (sourceAddress < PS2_RAM_SIZE)
        {
            for (uint32_t address = sourceAddress;
                 address < PS2_RAM_SIZE && sourcePath.size() < 512u;
                 ++address)
            {
                const char ch = static_cast<char>(rdram[address]);
                if (ch == '\0')
                {
                    break;
                }
                sourcePath.push_back(ch);
            }
        }
        if (!sourcePath.empty() && sourcePath.find(':') == std::string::npos)
        {
            const std::string normalizedPath = "cdrom0:" + sourcePath;
            const uint32_t normalizedAddress =
                ps2xGuestBumpAlloc(rdram, static_cast<uint32_t>(normalizedPath.size() + 1u), 16u);
            if (normalizedAddress != 0u)
            {
                std::memcpy(rdram + normalizedAddress, normalizedPath.c_str(), normalizedPath.size() + 1u);
                SET_GPR_U32(ctx, 2, normalizedAddress);
                std::cerr << "[xmen-path-prefix:3b1160] source=0x" << std::hex << sourceAddress
                          << " normalized=0x" << normalizedAddress
                          << " path=\"" << normalizedPath << "\""
                          << std::dec << std::endl;
            }
        }
    }
    if (targetPc == 0x0014C470u)
    {
        const uint32_t messageAddress = GPR_U32(ctx, 4);
        std::string message;
        if (rdram && messageAddress < PS2_RAM_SIZE)
        {
            for (uint32_t address = messageAddress;
                 address < PS2_RAM_SIZE && message.size() < 512u;
                 ++address)
            {
                const char ch = static_cast<char>(rdram[address]);
                if (ch == '\0')
                {
                    break;
                }
                message.push_back((ch >= 0x20 && ch < 0x7f) ? ch : '.');
            }
        }
        std::cerr << "[xmen-fatal-wrapper] source=0x" << std::hex << sourcePc
                  << " fallthrough=0x" << fallthroughPc
                  << " messageAddress=0x" << messageAddress
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " message=\"" << message << "\""
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (sourcePc == 0x00213C24u && targetPc == 0u)
    {
        static std::atomic<uint32_t> s_xmenNullComponentAttachCount{0u};
        const uint32_t traceIndex = s_xmenNullComponentAttachCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex >= 8u)
        {
            goto xmen_component_attach_trace_done;
        }
        const uint32_t vector = GPR_U32(ctx, 17);
        const uint32_t data = readRdramProbeU32(rdram, vector + 0x8u);
        const uint32_t index = GPR_U32(ctx, 19);
        const uint32_t owner = GPR_U32(ctx, 18);
        const uint32_t ownerVtable = readRdramProbeU32(rdram, owner);
        const uint32_t engineState = readRdramProbeU32(rdram, 0x00747048u);
        std::cerr << "[xmen-component-attach-null] target=0x" << std::hex << targetPc
                  << " owner=0x" << owner
                  << " ownerVtable=0x" << ownerVtable
                  << " ownerSlot58=0x" << readRdramProbeU32(rdram, ownerVtable + 0x58u)
                  << " engineState=0x" << engineState
                  << " engineFlags=0x" << readRdramProbeU32(rdram, engineState)
                  << " engineByte14=0x" << (readRdramProbeU32(rdram, engineState + 0x14u) & 0xFFu)
                  << " vector=0x" << vector
                  << " data=0x" << data
                  << " count=0x" << readRdramProbeU32(rdram, vector + 0xCu)
                  << " loopEnd=0x" << GPR_U32(ctx, 16)
                  << " index=0x" << index
                  << " item[-1]=0x" << (index != 0u ? readRdramProbeU32(rdram, data + (index - 1u) * 4u) : 0u)
                  << " item=0x" << readRdramProbeU32(rdram, data + index * 4u)
                  << " item[1]=0x" << readRdramProbeU32(rdram, data + (index + 1u) * 4u)
                  << std::dec << std::endl;
    }
xmen_component_attach_trace_done:
    if (sourcePc == 0x00211C34u && kind == GuestBranchKind::IndirectCall)
    {
        static std::atomic<uint32_t> s_xmenTreeCallbackTraceCount{0u};
        const uint32_t traceIndex = s_xmenTreeCallbackTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 8u || !m_memory.isCodeAddress(targetPc))
        {
            const uint32_t object = GPR_U32(ctx, 4);
            std::cerr << "[xmen-tree-callback] target=0x" << std::hex << targetPc
                      << " object=0x" << object
                      << " allocationSize=0x" << ps2xGuestBumpAllocationSize(object)
                      << " parent=0x" << readRdramProbeU32(rdram, object + 0x34u)
                      << " child=0x" << readRdramProbeU32(rdram, object + 0x38u)
                      << " callback=0x" << readRdramProbeU32(rdram, object + 0x3Cu)
                      << " next=0x" << readRdramProbeU32(rdram, object + 0x40u)
                      << " field44=0x" << readRdramProbeU32(rdram, object + 0x44u)
                      << " field48=0x" << readRdramProbeU32(rdram, object + 0x48u)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    const bool isXmenFontBranch =
        (sourcePc >= 0x0030C4B0u && sourcePc < 0x0030CA00u) ||
        (sourcePc >= 0x0031A380u && sourcePc <= 0x0031C918u) ||
        (sourcePc >= 0x003B2000u && sourcePc <= 0x003B21B4u) ||
        (sourcePc >= 0x003B2510u && sourcePc <= 0x003B25F4u);
    if (isXmenFontBranch)
    {
        static std::atomic<uint32_t> s_xmenFontBranchLogCount{0u};
        const uint32_t count = s_xmenFontBranchLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 32u)
        {
            std::cerr << "[xmen-font-branch] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " ra=0x" << GPR_U32(ctx, 31) << std::dec << std::endl;
        }
    }
    static thread_local std::array<uint32_t, 256> s_guestReturnTargets{};
    static thread_local std::array<uint32_t, 256> s_guestCallSources{};
    static thread_local uint32_t s_guestReturnDepth = 0u;
    static std::atomic<bool> s_xmenSteadyFrameCoverageEnabled{false};

    if (targetPc == 0x00235140u || sourcePc == 0x002351A4u)
    {
        const uint32_t stackPointer = GPR_U32(ctx, 29);
        const uint32_t savedReturnAddress =
            stackPointer >= 16u ? readRdramProbeU32(rdram, stackPointer - 16u) : 0u;
        std::cerr << "[xmen-235140-control] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " fallthrough=0x" << fallthroughPc
                  << " kind=" << describeGuestBranchKind(kind)
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " sp=0x" << stackPointer
                  << " savedRaAtSpMinus10=0x" << savedReturnAddress
                  << " depth=0x" << s_guestReturnDepth
                  << std::dec << std::endl;
    }

    if (targetPc == 0x00212660u || targetPc == 0x001FE740u || targetPc == 0x001FE3C0u)
    {
        static std::atomic<uint32_t> s_xmenClassAttachTraceCount{0u};
        const uint32_t count = s_xmenClassAttachTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 96u)
        {
            const uint32_t vector = GPR_U32(ctx, 4);
            const uint32_t storage = readRdramProbeU32(rdram, vector + 0x8u);
            std::cerr << "[xmen-class-attach] target=0x" << std::hex << targetPc
                      << " source=0x" << sourcePc
                      << " a0=0x" << vector
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " vectorData=0x" << storage
                      << " vectorCount=0x" << readRdramProbeU32(rdram, vector + 0xCu)
                      << " data0=0x" << readRdramProbeU32(rdram, storage)
                      << " data1=0x" << readRdramProbeU32(rdram, storage + 4u)
                      << " kind=" << describeGuestBranchKind(kind)
                      << std::dec << std::endl;
        }
    }

    if (sourcePc == 0x002122FCu)
    {
        static std::atomic<uint32_t> s_xmenClassLookupTraceCount{0u};
        const uint32_t count = s_xmenClassLookupTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            const uint32_t collection = GPR_U32(ctx, 21);
            const uint32_t vector = readRdramProbeU32(rdram, collection + 0x28u);
            const uint32_t storage = readRdramProbeU32(rdram, vector + 0x8u);
            std::cerr << "[xmen-class-lookup] collection=0x" << std::hex << collection
                      << " index=0x" << GPR_U32(ctx, 18)
                      << " item=0x" << GPR_U32(ctx, 4)
                      << " vector=0x" << vector
                      << " vectorData=0x" << storage
                      << " vectorCount=0x" << readRdramProbeU32(rdram, vector + 0xCu)
                      << " data0=0x" << readRdramProbeU32(rdram, storage)
                      << " data1=0x" << readRdramProbeU32(rdram, storage + 4u)
                      << std::dec << std::endl;
        }
    }

    // Compatibility allocations must be returned to the same heap. Passing
    // them to Alchemy's native free-list code corrupts neighboring live blocks.
    if (targetPc == 0x0022C710u &&
        (isCall || kind == GuestBranchKind::IndirectJump))
    {
        const uint32_t address = GPR_U32(ctx, 5);
        if (ps2xGuestBumpAllocationSize(address) != 0u)
        {
            (void)ps2xGuestBumpFree(address);
            if (isCall)
            {
                ctx->pc = fallthroughPc;
                return true;
            }

            ctx->pc = GPR_U32(ctx, 31);
            return false;
        }
    }

    // Alchemy's vector growth code asks the active allocator for the usable
    // size of its backing block. The guest allocator cannot see blocks owned
    // by the compatibility heap, so answer those queries from host tracking.
    if (isCall && targetPc == 0x00200FA0u)
    {
        const uint32_t address = GPR_U32(ctx, 4);
        const uint32_t allocationSize = ps2xGuestBumpAllocationSize(address);
        if (allocationSize != 0u)
        {
            SET_GPR_U32(ctx, 2, allocationSize);
            ctx->pc = fallthroughPc;

            static std::atomic<uint32_t> s_xmenAllocationSizeLogCount{0u};
            const uint32_t count = s_xmenAllocationSizeLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 128u)
            {
                std::cerr << "[xmen-alloc-compat:usable-size] source=0x" << std::hex << sourcePc
                          << " address=0x" << address
                          << " size=0x" << allocationSize
                          << " return=0x" << fallthroughPc
                          << std::dec << std::endl;
            }
            return true;
        }
    }

    // Compatibility-heap blocks are intentionally invisible to Alchemy's
    // native allocator registry. Handle its outer realloc wrapper before it
    // asks that registry to identify the block owner.
    if (isCall && (targetPc == 0x00200E90u || targetPc == 0x00200F30u))
    {
        const uint32_t oldAddress = GPR_U32(ctx, 4);
        const uint32_t oldSize = ps2xGuestBumpAllocationSize(oldAddress);
        if (oldSize != 0u)
        {
            const uint32_t newSize = GPR_U32(ctx, 5);
            const uint32_t requestedAlignment = GPR_U32(ctx, 6);
            const uint32_t alignment =
                requestedAlignment != 0u && (requestedAlignment & (requestedAlignment - 1u)) == 0u
                    ? requestedAlignment
                    : 16u;
            const uint32_t result = ps2xGuestBumpAlloc(rdram, newSize, alignment);
            const uint32_t copySize = std::min(oldSize, newSize);
            if (result != 0u && copySize != 0u &&
                oldAddress <= PS2_RAM_SIZE - copySize && result <= PS2_RAM_SIZE - copySize)
            {
                std::memmove(rdram + result, rdram + oldAddress, copySize);
            }
            if (result != 0u)
            {
                (void)ps2xGuestBumpFree(oldAddress);
            }
            SET_GPR_U32(ctx, 2, result);
            ctx->pc = fallthroughPc;

            static std::atomic<uint32_t> s_xmenOuterReallocationLogCount{0u};
            const uint32_t count =
                s_xmenOuterReallocationLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 128u || result == 0u)
            {
                std::cerr << "[xmen-alloc-compat:outer-realloc] source=0x" << std::hex << sourcePc
                          << " old=0x" << oldAddress
                          << " oldSize=0x" << oldSize
                          << " newSize=0x" << newSize
                          << " alignment=0x" << alignment
                          << " copied=0x" << copySize
                          << " result=0x" << result
                          << " return=0x" << fallthroughPc
                          << std::dec << std::endl;
            }
            return true;
        }
    }

    if (isCall && sourcePc == 0x00200EECu)
    {
        const uint32_t allocator = GPR_U32(ctx, 4);
        const uint32_t vtable = readRdramProbeU32(rdram, allocator);
        std::cerr << "[xmen-dma-buffer-realloc] target=0x" << std::hex << targetPc
                  << " allocator=0x" << allocator
                  << " vtable=0x" << vtable
                  << " slotE8=0x" << readRdramProbeU32(rdram, vtable + 0xE8u)
                  << " old=0x" << GPR_U32(ctx, 5)
                  << " size=0x" << GPR_U32(ctx, 6)
                  << " alignment=0x" << GPR_U32(ctx, 7)
                  << " return=0x" << fallthroughPc
                  << std::dec << std::endl;
    }

    // The reconstructed bootstrap heap can return ranges beyond its backing
    // block. Route Alchemy's allocation wrapper to the compatibility heap
    // before it reaches that allocator implementation.
    if (targetPc == 0x00231EB0u &&
        (isCall || kind == GuestBranchKind::IndirectJump))
    {
        const uint32_t allocator = GPR_U32(ctx, 4);
        const uint32_t size = GPR_U32(ctx, 5);
        const uint32_t requestedAlignment = readRdramProbeU32(rdram, allocator + 0xACu) & 0xFFFFu;
        const uint32_t alignment =
            requestedAlignment != 0u && (requestedAlignment & (requestedAlignment - 1u)) == 0u
                ? requestedAlignment
                : 16u;
        const uint32_t classSizeOperand = GPR_U32(ctx, 2);
        const uint32_t result = ps2xGuestBumpAlloc(rdram, size, alignment);
        SET_GPR_U32(ctx, 2, result);

        if (sourcePc == 0x00211CCCu &&
            result >= 0x00B6FF00u && result < 0x00B70100u)
        {
            const uint32_t classInfo = GPR_U32(ctx, 16);
            std::cerr << "[xmen-overlap-class-allocation] class=0x" << std::hex << classInfo
                      << " baseOffset=0x" << GPR_U32(ctx, 17)
                      << " classSize=0x" << classSizeOperand
                      << " totalSize=0x" << size
                      << " result=0x" << result
                      << " classVtable=0x" << readRdramProbeU32(rdram, classInfo)
                      << " field20=0x" << readRdramProbeU32(rdram, classInfo + 0x20u)
                      << " field24=0x" << readRdramProbeU32(rdram, classInfo + 0x24u)
                      << " field3c=0x" << readRdramProbeU32(rdram, classInfo + 0x3Cu)
                      << " field40=0x" << readRdramProbeU32(rdram, classInfo + 0x40u)
                      << " field44=0x" << readRdramProbeU32(rdram, classInfo + 0x44u)
                      << " field48=0x" << readRdramProbeU32(rdram, classInfo + 0x48u)
                      << " field4c=0x" << readRdramProbeU32(rdram, classInfo + 0x4Cu)
                      << std::dec << std::endl;
        }

        static std::atomic<uint32_t> s_xmenAllocationWrapperLogCount{0u};
        const uint32_t count = s_xmenAllocationWrapperLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 128u)
        {
            std::cerr << "[xmen-alloc-compat:alchemy-wrapper] source=0x" << std::hex << sourcePc
                      << " allocator=0x" << allocator
                      << " size=0x" << size
                      << " alignment=0x" << alignment
                      << " result=0x" << result
                      << " kind=" << describeGuestBranchKind(kind)
                      << " return=0x" << (isCall ? fallthroughPc : GPR_U32(ctx, 31))
                      << std::dec << std::endl;
        }

        if (isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        ctx->pc = GPR_U32(ctx, 31);
        return false;
    }

    // Alchemy exposes adjacent wrappers for explicitly aligned allocations
    // and zero-initialized arrays. Keep those on the same compatibility heap
    // so their allocations cannot fall back into the undersized guest arena.
    if (targetPc == 0x00231ED0u &&
        (isCall || kind == GuestBranchKind::IndirectJump))
    {
        const uint32_t allocator = GPR_U32(ctx, 4);
        const uint32_t size = GPR_U32(ctx, 5);
        const uint32_t requestedAlignment = GPR_U32(ctx, 6);
        const uint32_t alignment =
            requestedAlignment != 0u && (requestedAlignment & (requestedAlignment - 1u)) == 0u
                ? requestedAlignment
                : 16u;
        const uint32_t result = ps2xGuestBumpAlloc(rdram, size, alignment);
        SET_GPR_U32(ctx, 2, result);

        static std::atomic<uint32_t> s_xmenAlignedAllocationWrapperLogCount{0u};
        const uint32_t count = s_xmenAlignedAllocationWrapperLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-alloc-compat:alchemy-aligned-wrapper] source=0x" << std::hex << sourcePc
                      << " allocator=0x" << allocator
                      << " size=0x" << size
                      << " alignment=0x" << alignment
                      << " result=0x" << result
                      << " kind=" << describeGuestBranchKind(kind)
                      << " return=0x" << (isCall ? fallthroughPc : GPR_U32(ctx, 31))
                      << std::dec << std::endl;
        }

        if (isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        ctx->pc = GPR_U32(ctx, 31);
        return false;
    }

    if (isCall && targetPc == 0x00231EF0u)
    {
        const uint32_t allocator = GPR_U32(ctx, 4);
        const uint32_t elementCount = GPR_U32(ctx, 5);
        const uint32_t elementSize = GPR_U32(ctx, 6);
        const uint64_t totalSize64 = static_cast<uint64_t>(elementCount) * elementSize;
        const uint32_t totalSize = totalSize64 <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(totalSize64)
            : 0u;
        const uint32_t requestedAlignment = readRdramProbeU32(rdram, allocator + 0xACu) & 0xFFFFu;
        const uint32_t alignment =
            requestedAlignment != 0u && (requestedAlignment & (requestedAlignment - 1u)) == 0u
                ? requestedAlignment
                : 16u;
        const uint32_t allocatorFlags = readRdramProbeU32(rdram, allocator + 0xC0u);
        const bool clearAllocation = (allocatorFlags & 0x2u) != 0u || (allocatorFlags & 0x4u) == 0u;
        const uint32_t result = ps2xGuestBumpAlloc(rdram, totalSize, alignment);
        if (clearAllocation && result != 0u && totalSize != 0u)
        {
            std::memset(rdram + result, 0, totalSize);
        }
        SET_GPR_U32(ctx, 2, result);
        ctx->pc = fallthroughPc;

        static std::atomic<uint32_t> s_xmenArrayAllocationWrapperLogCount{0u};
        const uint32_t count = s_xmenArrayAllocationWrapperLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 128u || totalSize == 0x40u ||
            (result >= 0x0094AA00u && result < 0x0094AC00u))
        {
            std::cerr << "[xmen-alloc-compat:alchemy-array-wrapper] source=0x" << std::hex << sourcePc
                      << " allocator=0x" << allocator
                      << " count=0x" << elementCount
                      << " elementSize=0x" << elementSize
                      << " totalSize=0x" << totalSize
                      << " alignment=0x" << alignment
                      << " flags=0x" << allocatorFlags
                      << " clear=" << static_cast<uint32_t>(clearAllocation)
                      << " result=0x" << result
                      << " return=0x" << fallthroughPc
                      << std::dec << std::endl;
        }
        return true;
    }

    if (targetPc == 0x00233ED0u &&
        (isCall || kind == GuestBranchKind::IndirectJump))
    {
        const uint32_t address = GPR_U32(ctx, 5);
        const uint32_t allocationSize = ps2xGuestBumpAllocationSize(address);
        if (allocationSize != 0u)
        {
            (void)ps2xGuestBumpFree(address);

            static std::atomic<uint32_t> s_xmenOuterCompatibilityFreeLogCount{0u};
            const uint32_t count =
                s_xmenOuterCompatibilityFreeLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 128u)
            {
                std::cerr << "[xmen-alloc-compat:outer-free] source=0x" << std::hex << sourcePc
                          << " address=0x" << address
                          << " size=0x" << allocationSize
                          << " kind=" << describeGuestBranchKind(kind)
                          << " return=0x" << (isCall ? fallthroughPc : GPR_U32(ctx, 31))
                          << std::dec << std::endl;
            }

            if (isCall)
            {
                ctx->pc = fallthroughPc;
                return true;
            }

            ctx->pc = GPR_U32(ctx, 31);
            return false;
        }
    }

    // X-Men Legends' allocator vtable can contain a plausible but incorrect
    // realloc entry while the Alchemy class tables are still being rebuilt.
    // Intercept the wrappers before their lookups so the pointer and size
    // arguments are still intact.
    if (isCall && (targetPc == 0x002150B0u || targetPc == 0x00232020u ||
                   targetPc == 0x00232040u))
    {
        const uint32_t oldAddress = GPR_U32(ctx, 5);
        const uint32_t newSize = GPR_U32(ctx, 6);
        uint32_t oldSize = ps2xGuestBumpAllocationSize(oldAddress);
        if (oldSize == 0u)
        {
            oldSize = guestAllocationRemainingSize(oldAddress);
        }

        const uint32_t result = ps2xGuestBumpAlloc(rdram, newSize, 16u);
        const uint32_t copySize = std::min(oldSize, newSize);
        if (result != 0u && oldAddress != 0u && copySize != 0u &&
            oldAddress <= PS2_RAM_SIZE - copySize && result <= PS2_RAM_SIZE - copySize)
        {
            std::memmove(rdram + result, rdram + oldAddress, copySize);
        }
        if (ps2xGuestBumpAllocationSize(oldAddress) != 0u)
        {
            (void)ps2xGuestBumpFree(oldAddress);
        }
        SET_GPR_U32(ctx, 2, result);
        ctx->pc = fallthroughPc;

        static std::atomic<uint32_t> s_xmenReallocationLogCount{0u};
        const uint32_t count = s_xmenReallocationLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[xmen-alloc-compat:realloc-wrapper] source=0x" << std::hex << sourcePc
                      << " old=0x" << oldAddress
                      << " oldAvailable=0x" << oldSize
                      << " newSize=0x" << newSize
                      << " copied=0x" << copySize
                      << " result=0x" << result
                      << " return=0x" << fallthroughPc
                      << std::dec << std::endl;
        }
        return true;
    }

    if (isCall && targetPc == 0x00245D20u)
    {
        static std::atomic<uint32_t> s_xmenFileCloseTraceCount{0u};
        const uint32_t count = s_xmenFileCloseTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 16u)
        {
            std::cerr << "[xmen-file-close-call] index=" << count
                      << " source=0x" << std::hex << sourcePc
                      << " object=0x" << GPR_U32(ctx, 4)
                      << " slot=0x" << GPR_U32(ctx, 5)
                      << " return=0x" << fallthroughPc
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }

    if (isCall && targetPc == 0x003835B0u)
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t vtable = readRdramProbeU32(rdram, object);
        std::cerr << "[xmen-zip-stream-vtable] object=0x" << std::hex << object
                  << " vtable=0x" << vtable
                  << " handle=0x" << readRdramProbeU32(rdram, object + 0x18u)
                  << " mode2c=0x" << (readRdramProbeU32(rdram, object + 0x2Cu) & 0xFFu)
                  << " open2d=0x" << (readRdramProbeU32(rdram, object + 0x2Du) & 0xFFu)
                  << " buffer=0x" << readRdramProbeU32(rdram, object + 0x30u)
                  << " size=0x" << readRdramProbeU32(rdram, object + 0x34u)
                  << " cursor=0x" << readRdramProbeU32(rdram, object + 0x38u)
                  << " allocation=0x" << readRdramProbeU32(rdram, object + 0x3Cu)
                  << " manager=0x" << readRdramProbeU32(rdram, object + 0x40u)
                  << " slot60=0x" << readRdramProbeU32(rdram, vtable + 0x60u)
                  << " slot68=0x" << readRdramProbeU32(rdram, vtable + 0x68u)
                  << " slot6c=0x" << readRdramProbeU32(rdram, vtable + 0x6Cu)
                  << " slot98=0x" << readRdramProbeU32(rdram, vtable + 0x98u)
                  << " slot9c=0x" << readRdramProbeU32(rdram, vtable + 0x9Cu)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }

    static std::atomic<uint32_t> s_xmenFramePrepareSequence{0u};
    if (isCall && (sourcePc == 0x00274B20u || sourcePc == 0x00274B3Cu))
    {
        const uint32_t renderer = GPR_U32(ctx, 16);
        const uint32_t owner = readRdramProbeU32(rdram, renderer + 0x38u);
        const uint32_t ownerVtable = readRdramProbeU32(rdram, owner);
        std::cerr << "[xmen-frame-render-call] frame="
                  << s_xmenFramePrepareSequence.load(std::memory_order_relaxed)
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " renderer=0x" << renderer
                  << " active=0x" << (readRdramProbeU32(rdram, renderer + 0x3Cu) & 0xFFu)
                  << " owner=0x" << owner
                  << " ownerVtable=0x" << ownerVtable
                  << " slot64=0x" << readRdramProbeU32(rdram, ownerVtable + 0x64u)
                  << " slot74=0x" << readRdramProbeU32(rdram, ownerVtable + 0x74u)
                  << " queueHead=0x" << readRdramProbeU32(rdram, 0x00750774u)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x00274B44u && targetPc == 0x002F6130u)
    {
        const uint32_t frame =
            s_xmenFramePrepareSequence.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t queue = readRdramProbeU32(rdram, 0x00750774u);
        const uint32_t renderer = GPR_U32(ctx, 16);
        const uint32_t slotManager = readRdramProbeU32(rdram, renderer + 0x148u);
        constexpr uint32_t xmenLevelManager = 0x00B1A220u;
        const uint32_t xmenActiveLevel =
            readRdramProbeU32(rdram, xmenLevelManager + 0x0C08u);
        const uint32_t xmenActiveLevelVtable = xmenActiveLevel != 0u
            ? readRdramProbeU32(rdram, xmenActiveLevel + 0x0EC8u)
            : 0u;
        std::cerr << "[xmen-frame-prepare] frame=" << frame
                  << " renderer=0x" << std::hex << renderer
                  << " active=0x" << (readRdramProbeU32(rdram, renderer + 0x3Cu) & 0xFFu)
                  << " slotManager=0x" << slotManager
                  << " slotBase=0x" << readRdramProbeU32(rdram, slotManager + 0x10u)
                  << " slotCount=0x" << readRdramProbeU32(rdram, slotManager + 0x14u)
                  << " queue=0x" << queue
                  << " read=0x" << readRdramProbeU32(rdram, queue + 0x20u)
                  << " write=0x" << readRdramProbeU32(rdram, queue + 0x30u)
                  << " flags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                  << " completed=0x" << readRdramProbeU32(rdram, 0x007537E0u)
                  << " submitted=0x" << readRdramProbeU32(rdram, 0x007537E8u)
                  << " finishPending=0x" << readRdramProbeU32(rdram, 0x007537D0u)
                  << std::dec << std::endl;
        if ((frame & 0x3Fu) == 0u)
        {
            std::cerr << "[xmen-level-state] frame=" << std::dec << frame
                      << " manager=0x" << std::hex << xmenLevelManager
                      << " flags=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x0948u)
                      << " pathA=\"" << readGuestPrintableString(rdram, xmenLevelManager + 0x09CCu, 128u) << "\""
                      << " pathB=\"" << readGuestPrintableString(rdram, xmenLevelManager + 0x0A4Cu, 128u) << "\""
                      << " objectHead=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x0BD4u)
                      << " objectCount=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x0BE0u)
                      << " active=0x" << xmenActiveLevel
                      << " activeFlags=0x" << readRdramProbeU32(rdram, xmenActiveLevel)
                      << " activeVtable=0x" << xmenActiveLevelVtable
                      << " update=0x" << readRdramProbeU32(rdram, xmenActiveLevelVtable + 0x3Cu)
                      << " render=0x" << readRdramProbeU32(rdram, xmenActiveLevelVtable + 0x38u)
                      << " nextA=\"" << readGuestPrintableString(rdram, xmenLevelManager + 0x17A8u, 128u) << "\""
                      << " nextB=\"" << readGuestPrintableString(rdram, xmenLevelManager + 0x1828u, 128u) << "\""
                      << " state1798=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x1798u)
                      << " state179c=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x179Cu)
                      << " state17a0=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x17A0u)
                      << " state17a4=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x17A4u)
                      << " state18a8=0x" << readRdramProbeU32(rdram, xmenLevelManager + 0x18A8u)
                      << std::dec << std::endl;
        }
    }

    if (isCall && sourcePc == 0x002F6158u && targetPc == 0x002F6E90u)
    {
        const uint32_t queue = readRdramProbeU32(rdram, 0x00750774u);
        std::cerr << "[xmen-frame-flush-check] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " queue=0x" << std::hex << queue
                  << " read=0x" << readRdramProbeU32(rdram, queue + 0x20u)
                  << " write=0x" << readRdramProbeU32(rdram, queue + 0x30u)
                  << " flags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                  << " callbackHead=0x" << readRdramProbeU32(rdram, 0x007537B8u)
                  << " callbackTail=0x" << readRdramProbeU32(rdram, 0x007537B0u)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002F6EC4u && targetPc == 0x002DDE20u)
    {
        const uint32_t mode = GPR_U32(ctx, 4);
        const uint32_t queue = readRdramProbeU32(rdram, 0x00750770u + mode * 4u);
        const uint32_t list = readRdramProbeU32(rdram, 0x007507D0u + mode * 4u);
        std::cerr << "[xmen-frame-flush-submit] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << mode
                  << " ready=0x" << (readRdramProbeU32(rdram, 0x00750928u + mode) & 0xFFu)
                  << " queue=0x" << queue
                  << " read=0x" << readRdramProbeU32(rdram, queue + 0x20u)
                  << " write=0x" << readRdramProbeU32(rdram, queue + 0x30u)
                   << " flags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                   << " list=0x" << list
                   << " listNext=0x" << readRdramProbeU32(rdram, list + 0x10u)
                   << " caller=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 29))
                   << " issued=0x" << readRdramProbeU64(rdram, 0x00750800u + mode * 8u)
                   << " completedMode=0x" << readRdramProbeU64(rdram, 0x00750850u + mode * 8u)
                   << " waitTarget=0x" << readRdramProbeU64(rdram, 0x007508B0u + mode * 8u)
                   << " waitFlags=0x" << readRdramProbeU32(rdram, 0x00754EE0u)
                   << " vif1Chcr=0x" << m_memory.readIORegister(0x10009000u)
                   << " vif1Tadr=0x" << m_memory.readIORegister(0x10009030u)
                   << " dstat=0x" << m_memory.readIORegister(0x1000E010u)
                   << std::dec << std::endl;
    }

    if (isCall && targetPc == 0x002DF450u)
    {
        const uint32_t mode = GPR_U32(ctx, 4);
        const uint32_t queue = readRdramProbeU32(rdram, 0x00750770u + mode * 4u);
        std::cerr << "[xmen-dma-sync-enter] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " source=0x" << std::hex << sourcePc
                  << " mode=0x" << mode
                  << " target=0x" << GPR_U64(ctx, 5)
                  << " issued=0x" << readRdramProbeU64(rdram, 0x00750800u + mode * 8u)
                  << " completedMode=0x" << readRdramProbeU64(rdram, 0x00750850u + mode * 8u)
                  << " waitTarget=0x" << readRdramProbeU64(rdram, 0x007508B0u + mode * 8u)
                  << " waitFlags=0x" << readRdramProbeU32(rdram, 0x00754EE0u)
                  << " queue=0x" << queue
                  << " queueSequence=0x" << readRdramProbeU64(rdram, queue)
                  << " queueFlags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                  << std::dec << std::endl;
    }

    if (isCall &&
        ((sourcePc == 0x00271F08u && targetPc == 0x002D92E0u) ||
         (sourcePc == 0x00271F14u && targetPc == 0x002FB570u) ||
         (sourcePc == 0x00271F1Cu && targetPc == 0x002F9FE0u)))
    {
        const uint32_t object = GPR_U32(ctx, 4);
        const uint32_t fence = sourcePc == 0x00271F08u
                                   ? readRdramProbeU32(rdram, object + 0x0Cu)
                                   : object;
        const uint32_t slot = GPR_U32(ctx, 16);
        const uint32_t renderer = GPR_U32(ctx, 18);
        const uint32_t slotManager = readRdramProbeU32(rdram, renderer + 0x148u);
        std::cerr << "[xmen-gif-fence-object] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " root=0x" << renderer
                  << " slotManager=0x" << slotManager
                  << " slotBase=0x" << readRdramProbeU32(rdram, slotManager + 0x10u)
                  << " slotCount=0x" << readRdramProbeU32(rdram, slotManager + 0x14u)
                  << " slot=0x" << slot
                  << " slotOwner=0x" << readRdramProbeU32(rdram, slot + 4u)
                  << " slotObject=0x" << readRdramProbeU32(rdram, slot + 8u)
                  << " object=0x" << object
                  << " objectFence=0x" << readRdramProbeU32(rdram, object + 0x0Cu)
                  << " fence=0x" << fence
                  << " fenceValue=0x" << readRdramProbeU64(rdram, fence)
                  << " fenceFlags=0x" << (readRdramProbeU32(rdram, fence + 0x44u) & 0xFFFFFFu)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DF4D4u && targetPc == 0x002F6E90u)
    {
        const uint32_t mode = GPR_U32(ctx, 16) >> 3u;
        const uint32_t queue = readRdramProbeU32(rdram, 0x00750770u + mode * 4u);
        std::cerr << "[xmen-dma-sync-flush] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << mode
                  << " target=0x" << GPR_U64(ctx, 18)
                  << " issued=0x" << readRdramProbeU64(rdram, 0x00750800u + mode * 8u)
                  << " completedMode=0x" << readRdramProbeU64(rdram, 0x00750850u + mode * 8u)
                  << " waitTarget=0x" << readRdramProbeU64(rdram, 0x007508B0u + mode * 8u)
                  << " waitFlags=0x" << readRdramProbeU32(rdram, 0x00754EE0u)
                  << " queue=0x" << queue
                  << " queueSequence=0x" << readRdramProbeU64(rdram, queue)
                  << " queueFlags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                  << std::dec << std::endl;
    }

    if (isCall &&
        (sourcePc == 0x002DE3ACu || sourcePc == 0x002DE51Cu || sourcePc == 0x002DE69Cu) &&
        targetPc == 0x002DE130u)
    {
        const uint32_t mode = GPR_U32(ctx, 4);
        const uint32_t queue = GPR_U32(ctx, 5);
        std::cerr << "[xmen-dma-complete-enter] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << mode
                  << " queue=0x" << queue
                  << " queueSequence=0x" << readRdramProbeU64(rdram, queue)
                  << " queueFlags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                  << " issued=0x" << readRdramProbeU64(rdram, 0x00750800u + mode * 8u)
                  << " completedMode=0x" << readRdramProbeU64(rdram, 0x00750850u + mode * 8u)
                  << " waitTarget=0x" << readRdramProbeU64(rdram, 0x007508B0u + mode * 8u)
                  << " waitFlags=0x" << readRdramProbeU32(rdram, 0x00754EE0u)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DE268u && targetPc == 0x0010AFA0u)
    {
        const uint32_t mode = GPR_U32(ctx, 17);
        std::cerr << "[xmen-dma-complete-target-reached] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << mode
                  << " completedMode=0x" << readRdramProbeU64(rdram, 0x00750850u + mode * 8u)
                  << " waitTarget=0x" << readRdramProbeU64(rdram, 0x007508B0u + mode * 8u)
                  << " sema=0x" << GPR_U32(ctx, 4)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DE284u && targetPc == 0x0010AF50u)
    {
        std::cerr << "[xmen-dma-complete-signal] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " sema=0x" << std::hex << GPR_U32(ctx, 4)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DE000u && targetPc == 0x002DED70u)
    {
        const uint32_t mode = GPR_U32(ctx, 4);
        const uint32_t list = readRdramProbeU32(rdram, 0x007507D0u + mode * 4u);
        std::cerr << "[xmen-vif1-ready-check] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << mode
                  << " ready=0x" << (readRdramProbeU32(rdram, 0x00750928u + mode) & 0xFFu)
                  << " list=0x" << list
                  << " listNext=0x" << readRdramProbeU32(rdram, list + 0x10u)
                  << " vif1Chcr=0x" << m_memory.readIORegister(0x10009000u)
                  << " dstat=0x" << m_memory.readIORegister(0x1000E010u)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DE01Cu && targetPc == 0x002DEF90u)
    {
        std::cerr << "[xmen-vif1-launch] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << GPR_U32(ctx, 4)
                  << " ready=0x" << (readRdramProbeU32(rdram, 0x00750928u + GPR_U32(ctx, 4)) & 0xFFu)
                  << " vif1Chcr=0x" << m_memory.readIORegister(0x10009000u)
                  << " dstat=0x" << m_memory.readIORegister(0x1000E010u)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DEDA4u &&
        s_xmenFramePrepareSequence.load(std::memory_order_relaxed) >= 80u)
    {
        static std::atomic<uint32_t> s_xmenVifFenceTraceCount{0u};
        const uint32_t traceIndex = s_xmenVifFenceTraceCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t node = GPR_U32(ctx, 17);
        if (traceIndex < 64u || (traceIndex & 0xFFu) == 0u)
        {
            std::cerr << "[xmen-vif1-fence-call] frame="
                      << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                      << " node=0x" << std::hex << node
                      << " target=0x" << targetPc
                      << " argument=0x" << GPR_U32(ctx, 4)
                      << " next=0x" << readRdramProbeU32(rdram, node + 0x0Cu)
                      << " completed=0x" << readRdramProbeU32(rdram, 0x007537E0u)
                      << " submitted=0x" << readRdramProbeU32(rdram, 0x007537E8u)
                      << " gifIssued=0x" << readRdramProbeU64(rdram, 0x00750810u)
                      << " gifCompleted=0x" << readRdramProbeU64(rdram, 0x00750860u)
                      << " gifChcr=0x" << m_memory.readIORegister(0x1000A000u)
                      << " gifTadr=0x" << m_memory.readIORegister(0x1000A030u)
                      << std::dec << std::endl;
        }
    }

    if (isCall && sourcePc == 0x002F6ACCu && targetPc == 0x002DF1C0u &&
        s_xmenFramePrepareSequence.load(std::memory_order_relaxed) >= 80u)
    {
        static std::atomic<uint32_t> s_xmenVsyncRetryTraceCount{0u};
        const uint32_t traceIndex = s_xmenVsyncRetryTraceCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t mode = GPR_U32(ctx, 4);
        const uint32_t queue = readRdramProbeU32(rdram, 0x007507D0u + mode * 4u);
        const uint32_t node = readRdramProbeU32(rdram, queue + 0x10u);
        if (traceIndex < 64u || (traceIndex & 0xFFu) == 0u)
        {
            std::cerr << "[xmen-vif1-vsync-retry] frame="
                      << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                      << " mode=0x" << std::hex << mode
                      << " ready=0x" << (readRdramProbeU32(rdram, 0x00750928u + mode) & 0xFFu)
                      << " queue=0x" << queue
                      << " flags=0x" << (readRdramProbeU32(rdram, queue + 0x40u) & 0xFFu)
                      << " node=0x" << node
                      << " nodeArg=0x" << readRdramProbeU32(rdram, node)
                      << " nodeFn=0x" << readRdramProbeU32(rdram, node + 0x10u)
                      << " completed=0x" << readRdramProbeU32(rdram, 0x007537E0u)
                      << " submitted=0x" << readRdramProbeU32(rdram, 0x007537E8u)
                      << " gifIssued=0x" << readRdramProbeU64(rdram, 0x00750810u)
                      << " gifCompleted=0x" << readRdramProbeU64(rdram, 0x00750860u)
                      << " gifChcr=0x" << m_memory.readIORegister(0x1000A000u)
                      << " gifTadr=0x" << m_memory.readIORegister(0x1000A030u)
                      << " gp=0x" << GPR_U32(ctx, 28)
                      << " guard=0x" << (readRdramProbeU32(rdram, GPR_U32(ctx, 28) - 0x7B04u) & 0xFFu)
                      << std::dec << std::endl;
        }
    }

    if (isCall && sourcePc == 0x002DF208u && targetPc == 0x002DED70u &&
        s_xmenFramePrepareSequence.load(std::memory_order_relaxed) >= 80u)
    {
        std::cerr << "[xmen-vif1-vsync-ready-check] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << GPR_U32(ctx, 4)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x002DF224u && targetPc == 0x002DEF90u)
    {
        std::cerr << "[xmen-vif1-vsync-launch] frame="
                  << (s_xmenFramePrepareSequence.load(std::memory_order_relaxed) - 1u)
                  << " mode=0x" << std::hex << GPR_U32(ctx, 4)
                  << std::dec << std::endl;
    }

    if (isCall && sourcePc == 0x00274B4Cu && targetPc == 0x002F62D0u)
    {
        static const uint32_t xmenFrameWindowOverride = [] {
            const char *value = std::getenv("PS2X_XMEN_FRAME_WINDOW");
            return value && *value
                ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
                : 0u;
        }();
        if (xmenFrameWindowOverride >= 2u)
        {
            writeRdramProbeU32(rdram, 0x00753838u, xmenFrameWindowOverride);
        }

        static std::atomic<uint32_t> s_xmenFrameProbeCount{0u};
        const uint32_t probeIndex = s_xmenFrameProbeCount.fetch_add(1u, std::memory_order_relaxed);
        if (probeIndex == 2u)
            s_xmenSteadyFrameCoverageEnabled.store(true, std::memory_order_release);
        const uint32_t completed = readRdramProbeU32(rdram, 0x007537E0u);
        const uint32_t submitted = readRdramProbeU32(rdram, 0x007537E8u);
        if (probeIndex < 128u || submitted >= 0x50u)
        {
            const uint32_t renderer = GPR_U32(ctx, 16);
            std::cerr << "[xmen-frame-entry] index=" << probeIndex
                      << " renderer=0x" << std::hex << renderer
                      << " vtable=0x" << readRdramProbeU32(rdram, renderer)
                      << " owner=0x" << readRdramProbeU32(rdram, renderer + 0x38u)
                      << " active=0x" << readRdramProbeU32(rdram, renderer + 0x3Cu)
                      << " pending=0x" << readRdramProbeU32(rdram, renderer + 0x40u)
                      << " clock=0x" << readRdramProbeU32(rdram, renderer + 0x44u)
                      << " queue=0x" << readRdramProbeU32(rdram, renderer + 0x610u)
                      << " surface=0x" << readRdramProbeU32(rdram, renderer + 0x3ACu)
                      << " aux=0x" << readRdramProbeU32(rdram, renderer + 0x3B0u)
                      << " mode=0x" << readRdramProbeU32(rdram, renderer + 0x5ACu)
                      << " state=0x" << readRdramProbeU32(rdram, renderer + 0x5E8u)
                      << " completed=0x" << completed
                      << " submittedBefore=0x" << submitted
                      << " window=0x" << readRdramProbeU32(rdram, 0x00753838u)
                      << " finishPending=0x" << readRdramProbeU32(rdram, 0x007537D0u)
                      << " depth=" << std::dec << s_guestReturnDepth
                      << " calls=[";
            for (uint32_t i = 0u; i < s_guestReturnDepth; ++i)
            {
                std::cerr << "0x" << std::hex << s_guestCallSources[i]
                          << "->0x" << s_guestReturnTargets[i] << ',';
            }
            std::cerr << "]" << std::dec << std::endl;
        }
    }

    if (isCall && s_xmenSteadyFrameCoverageEnabled.load(std::memory_order_acquire))
    {
        static std::mutex s_xmenSteadyFrameCoverageMutex;
        static std::array<uint32_t, 2048> s_xmenSteadyFrameCoverageTargets{};
        static uint32_t s_xmenSteadyFrameCoverageCount = 0u;
        std::lock_guard<std::mutex> lock(s_xmenSteadyFrameCoverageMutex);
        bool seen = false;
        for (uint32_t i = 0u; i < s_xmenSteadyFrameCoverageCount; ++i)
        {
            if (s_xmenSteadyFrameCoverageTargets[i] == targetPc)
            {
                seen = true;
                break;
            }
        }
        if (!seen && s_xmenSteadyFrameCoverageCount < s_xmenSteadyFrameCoverageTargets.size())
        {
            s_xmenSteadyFrameCoverageTargets[s_xmenSteadyFrameCoverageCount++] = targetPc;
            std::cerr << "[xmen-steady-call] index=" << (s_xmenSteadyFrameCoverageCount - 1u)
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << std::dec << std::endl;
        }
    }

    if ((isCall &&
         (sourcePc == 0x0014B7C0u || sourcePc == 0x0014B838u ||
          sourcePc == 0x0042268Cu || sourcePc == 0x004226ACu ||
          targetPc == 0x003771F0u || targetPc == 0x00377D60u)) ||
        (!isCall && (targetPc == 0x00422694u || targetPc == 0x004226B4u)))
    {
        std::cerr << "[xmen-intro-call] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " return=0x" << fallthroughPc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " a0=0x" << GPR_U32(ctx, 4)
                  << " a1=0x" << GPR_U32(ctx, 5)
                  << " a2=0x" << GPR_U32(ctx, 6)
                  << " kind=" << describeGuestBranchKind(kind)
                  << std::dec << std::endl;
    }

    // X-Men Legends' reconstructed Alchemy allocator table routes both the
    // static allocation helper and its virtual allocate slot through 0x200ce0.
    // Running that guest wrapper recursively asks for allocator class 2 before
    // the class exists, so provide the allocation while preserving the two
    // calling conventions used by the game.
    if (isCall && targetPc == 0x00200CE0u)
    {
        const bool virtualCall = kind == GuestBranchKind::IndirectCall;
        const uint32_t size = GPR_U32(ctx, virtualCall ? 5 : 4);
        const uint32_t result = ps2xGuestBumpAlloc(rdram, size, 16u);
        SET_GPR_U32(ctx, 2, result);
        ctx->pc = fallthroughPc;

        static std::atomic<uint32_t> s_xmenAllocationLogCount{0u};
        const uint32_t count = s_xmenAllocationLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            std::cerr << "[xmen-alloc-compat] source=0x" << std::hex << sourcePc
                      << " size=0x" << size
                      << " result=0x" << result
                      << " virtual=" << static_cast<uint32_t>(virtualCall)
                      << " return=0x" << fallthroughPc
                      << std::dec << std::endl;
        }
        return true;
    }

    const uint32_t sourceLoadInstruction =
        sourcePc >= sizeof(uint32_t) ? readRdramProbeU32(rdram, sourcePc - sizeof(uint32_t)) : 0u;
    const bool isNullAlchemyAllocateSlot =
        targetPc == 0u && (sourceLoadInstruction & 0xFFFF0000u) == 0x8F390000u &&
        (sourceLoadInstruction & 0xFFFFu) == 0x00D4u;
    if (isNullAlchemyAllocateSlot)
    {
        const uint32_t size = GPR_U32(ctx, 5);
        const uint32_t result = ps2xGuestBumpAlloc(rdram, size, 16u);
        SET_GPR_U32(ctx, 2, result);

        static std::atomic<uint32_t> s_xmenNullAllocationLogCount{0u};
        const uint32_t count = s_xmenNullAllocationLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 256u)
        {
            std::cerr << "[xmen-alloc-compat:null-slot] source=0x" << std::hex << sourcePc
                      << " size=0x" << size
                      << " result=0x" << result
                      << " kind=" << describeGuestBranchKind(kind)
                      << " return=0x" << (isCall ? fallthroughPc : GPR_U32(ctx, 31))
                      << std::dec << std::endl;
        }

        if (isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        // The allocation trampolines end in JR $t9. Yield to the scheduler at
        // the caller's return address so their generated post-JR code cannot
        // overwrite the completed tail call.
        ctx->pc = GPR_U32(ctx, 31);
        return false;
    }

    const bool isNullAlchemyAlignedAllocateSlot =
        targetPc == 0u && (sourceLoadInstruction & 0xFFFF0000u) == 0x8F390000u &&
        (sourceLoadInstruction & 0xFFFFu) == 0x00DCu;
    if (isNullAlchemyAlignedAllocateSlot)
    {
        const uint32_t size = GPR_U32(ctx, 5);
        const uint32_t requestedAlignment = GPR_U32(ctx, 6);
        const uint32_t alignment =
            requestedAlignment != 0u && (requestedAlignment & (requestedAlignment - 1u)) == 0u
                ? requestedAlignment
                : 16u;
        const uint32_t result = ps2xGuestBumpAlloc(rdram, size, alignment);
        SET_GPR_U32(ctx, 2, result);

        static std::atomic<uint32_t> s_xmenNullAlignedAllocationLogCount{0u};
        const uint32_t logCount = s_xmenNullAlignedAllocationLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (logCount < 64u)
        {
            std::cerr << "[xmen-alloc-compat:null-aligned-slot] source=0x" << std::hex << sourcePc
                      << " size=0x" << size
                      << " alignment=0x" << alignment
                      << " result=0x" << result
                      << " kind=" << describeGuestBranchKind(kind)
                      << " return=0x" << (isCall ? fallthroughPc : GPR_U32(ctx, 31))
                      << std::dec << std::endl;
        }

        if (isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        ctx->pc = GPR_U32(ctx, 31);
        return false;
    }

    const bool isNullAlchemyReallocateSlot =
        targetPc == 0u && (sourceLoadInstruction & 0xFFFF0000u) == 0x8F390000u &&
        (sourceLoadInstruction & 0xFFFFu) == 0x00E4u;
    if (isNullAlchemyReallocateSlot)
    {
        const uint32_t oldAddress = GPR_U32(ctx, 5);
        const uint32_t newSize = GPR_U32(ctx, 6);
        uint32_t oldSize = ps2xGuestBumpAllocationSize(oldAddress);
        if (oldSize == 0u)
        {
            oldSize = guestAllocationRemainingSize(oldAddress);
        }

        const uint32_t result = ps2xGuestBumpAlloc(rdram, newSize, 16u);
        const uint32_t copySize = std::min(oldSize, newSize);
        if (result != 0u && oldAddress != 0u && copySize != 0u &&
            oldAddress <= PS2_RAM_SIZE - copySize && result <= PS2_RAM_SIZE - copySize)
        {
            std::memmove(rdram + result, rdram + oldAddress, copySize);
        }
        if (ps2xGuestBumpAllocationSize(oldAddress) != 0u)
        {
            (void)ps2xGuestBumpFree(oldAddress);
        }
        SET_GPR_U32(ctx, 2, result);

        static std::atomic<uint32_t> s_xmenNullReallocationLogCount{0u};
        const uint32_t logCount = s_xmenNullReallocationLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (logCount < 64u)
        {
            std::cerr << "[xmen-alloc-compat:null-realloc-slot] source=0x" << std::hex << sourcePc
                      << " old=0x" << oldAddress
                      << " oldAvailable=0x" << oldSize
                      << " newSize=0x" << newSize
                      << " copied=0x" << copySize
                      << " result=0x" << result
                      << " kind=" << describeGuestBranchKind(kind)
                      << " return=0x" << (isCall ? fallthroughPc : GPR_U32(ctx, 31))
                      << std::dec << std::endl;
        }

        if (isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        ctx->pc = GPR_U32(ctx, 31);
        return false;
    }

    if (isCall && targetPc == 0u)
    {
        static std::atomic<bool> s_reportedNullCall{false};
        const bool firstReport = !s_reportedNullCall.exchange(true, std::memory_order_acq_rel);
        if (firstReport)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[guest-branch:null-call] source=0x" << std::hex << sourcePc
                      << " fallthrough=0x" << fallthroughPc
                      << " pc=0x" << ctx->pc
                      << " v0=0x" << GPR_U64(ctx, 2)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " a3=0x" << GPR_U32(ctx, 7)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " s3=0x" << GPR_U32(ctx, 19)
                      << " s3_8=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 19) + 8u)
                      << " list_8=0x" << readRdramProbeU32(rdram, readRdramProbeU32(rdram, GPR_U32(ctx, 19) + 8u) + 8u)
                      << " list_10=0x" << readRdramProbeU32(rdram, readRdramProbeU32(rdram, GPR_U32(ctx, 19) + 8u) + 16u)
                      << " item0=0x" << readRdramProbeU32(rdram, readRdramProbeU32(rdram, readRdramProbeU32(rdram, GPR_U32(ctx, 19) + 8u) + 16u))
                      << " item1=0x" << readRdramProbeU32(rdram, readRdramProbeU32(rdram, readRdramProbeU32(rdram, GPR_U32(ctx, 19) + 8u) + 16u) + 4u)
                      << " item0vt=0x" << readRdramProbeU32(rdram, readRdramProbeU32(rdram, readRdramProbeU32(rdram, readRdramProbeU32(rdram, GPR_U32(ctx, 19) + 8u) + 16u)))
                      << " objectVtable=0x" << vtable
                      << " objectSlot=0x" << readRdramProbeU32(rdram, vtable + (sourceLoadInstruction & 0xffffu))
                      << " sourceLoad=0x" << sourceLoadInstruction
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
        ctx->pc = fallthroughPc;
        return true;
    }

    if (kind == GuestBranchKind::IndirectCall && targetPc < g_ps2RecompiledFunctionTableBase)
    {
        static std::atomic<bool> s_reportedLowCall{false};
        const bool firstReport = !s_reportedLowCall.exchange(true, std::memory_order_acq_rel);
        if (firstReport)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[guest-branch:low-call] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " tableBase=0x" << g_ps2RecompiledFunctionTableBase
                      << " pc=0x" << ctx->pc
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << object
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " objectVtable=0x" << vtable
                      << " objectSlot=0x" << readRdramProbeU32(rdram, vtable + (sourceLoadInstruction & 0xffffu))
                      << " sourceLoad=0x" << sourceLoadInstruction
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
        ctx->pc = fallthroughPc;
        return true;
    }

    if (kind == GuestBranchKind::IndirectCall && !m_memory.isCodeAddress(targetPc))
    {
        static std::atomic<uint32_t> s_reportedNonCodeCallCount{0u};
        const uint32_t count = s_reportedNonCodeCallCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            const uint32_t object = GPR_U32(ctx, 4);
            const uint32_t vtable = readRdramProbeU32(rdram, object);
            std::cerr << "[guest-branch:noncode-call] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " pc=0x" << ctx->pc
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << object
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " s0=0x" << GPR_U32(ctx, 16)
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " objectVtable=0x" << vtable
                      << " objectSlot=0x" << readRdramProbeU32(rdram, vtable + (sourceLoadInstruction & 0xffffu))
                      << " sourceLoad=0x" << sourceLoadInstruction
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
        ctx->pc = fallthroughPc;
        return true;
    }

    // Every inter-function transfer is also a deterministic EE safe point.
    // Backward edges inside generated functions use eeCheckpointDue(), while
    // this charge bounds straight-line call chains that have no local loop.
    if (m_eeScheduler && m_eeScheduler->checkpointDue(EeScheduler::kGuestDispatchCycles))
    {
        ++g_guestDispatchYieldGeneration;
        if (isXmenFontBranch)
        {
            std::cerr << "[xmen-font-branch:checkpoint-yield] source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " sp=0x" << GPR_U32(ctx, 29) << std::dec << std::endl;
        }
        return false;
    }

    if (!isCall)
    {
        if (kind == GuestBranchKind::Return && s_guestReturnDepth > 0u &&
            !hasFunction(targetPc))
        {
            const uint32_t invalidTarget = targetPc;
            const uint32_t repairedTarget = s_guestReturnTargets[s_guestReturnDepth - 1u];
            static std::atomic<uint32_t> s_returnRepairLogCount{0u};
            const uint32_t count = s_returnRepairLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (count < 128u)
            {
                std::cerr << "[guest-return:invalid-repair] source=0x" << std::hex << sourcePc
                          << " invalid=0x" << invalidTarget
                          << " repaired=0x" << repairedTarget
                          << " callSource=0x" << s_guestCallSources[s_guestReturnDepth - 1u]
                          << " depth=0x" << s_guestReturnDepth
                          << " op=" << (debugName ? debugName : "")
                          << " sp=0x" << GPR_U32(ctx, 29)
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            }
            targetPc = repairedTarget;
            ctx->pc = repairedTarget;
        }

        const bool isRootReturn =
            kind == GuestBranchKind::Return && targetPc == 0u && s_guestReturnDepth == 0u;

        if (targetPc == 0u)
        {
            static std::atomic<bool> s_reportedNullNonCall{false};
            const bool firstReport = !s_reportedNullNonCall.exchange(true, std::memory_order_acq_rel);
            if (firstReport)
            {
                std::cerr << "[guest-branch:null-noncall] kind=" << describeGuestBranchKind(kind)
                          << " op=" << (debugName ? debugName : "")
                          << " source=0x" << std::hex << sourcePc
                          << " fallthrough=0x" << fallthroughPc
                          << " trace=" << formatDispatchHistory()
                          << std::dec << std::endl;
            }
        }

        if (!isRootReturn && !hasFunction(targetPc))
        {
            reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);
        }

        ctx->pc = targetPc;
        return false;
    }

    if (!hasFunction(targetPc))
    {
        reportMissingFunction(rdram, ctx, targetPc, sourcePc, kind, debugName);

        const MissingFunctionPolicy policy = missingFunctionPolicy();

        if (policy == MissingFunctionPolicy::SkipCallDebug && isCall)
        {
            ctx->pc = fallthroughPc;
            return true;
        }

        if (policy == MissingFunctionPolicy::ContinueToTarget)
        {
            ctx->pc = targetPc;
            return true;
        }

        return false;
    }

    static thread_local uint32_t s_nativeDispatchDepth = 0u;
    if (s_nativeDispatchDepth >= 32u)
    {
        ++g_guestDispatchYieldGeneration;
        static std::atomic<uint32_t> s_dispatchDepthLogCount{0u};
        const uint32_t count = s_dispatchDepthLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 64u)
        {
            std::cerr << "[dispatch-depth-yield] depth=0x" << std::hex << s_nativeDispatchDepth
                      << " source=0x" << sourcePc
                      << " target=0x" << targetPc
                      << " fallthrough=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
        ctx->pc = targetPc;
        return false;
    }

    struct NativeDispatchDepthGuard
    {
        uint32_t &depth;
        ~NativeDispatchDepthGuard() { --depth; }
    };
    ++s_nativeDispatchDepth;
    NativeDispatchDepthGuard dispatchDepthGuard{s_nativeDispatchDepth};

    RecompiledFunction targetFn = lookupFunction(targetPc);
    const uint32_t entryPc = ctx->pc;
    const uint64_t yieldGeneration = g_guestDispatchYieldGeneration;
    const bool pushedGuestReturn = fallthroughPc != 0u && s_guestReturnDepth < s_guestReturnTargets.size();
    if (pushedGuestReturn)
    {
        s_guestReturnTargets[s_guestReturnDepth] = fallthroughPc;
        s_guestCallSources[s_guestReturnDepth] = sourcePc;
        ++s_guestReturnDepth;
    }
    struct GuestReturnDepthGuard
    {
        bool armed = false;
        uint32_t &depth;
        ~GuestReturnDepthGuard()
        {
            if (armed && depth > 0u)
            {
                --depth;
            }
        }
    };
    GuestReturnDepthGuard guestReturnDepthGuard{pushedGuestReturn, s_guestReturnDepth};

    const uint32_t previousLiveChainSlot = g_xmenLiveChainSavedReturnSlot;
    const uint32_t previousLiveChainReturn = g_xmenLiveChainExpectedReturn;
    const uint64_t previousLiveChainObserved = g_xmenLiveChainLastObserved;
    if (traceXmenChainBuilderCall)
    {
        g_xmenLiveChainSavedReturnSlot = xmenChainSavedReturnSlot;
        g_xmenLiveChainExpectedReturn = fallthroughPc;
        g_xmenLiveChainLastObserved = fallthroughPc;
        ++g_xmenLiveChainSequence;
    }
    struct XmenLiveChainGuard
    {
        bool armed;
        uint32_t previousSlot;
        uint32_t previousReturn;
        uint64_t previousObserved;
        ~XmenLiveChainGuard()
        {
            if (armed)
            {
                g_xmenLiveChainSavedReturnSlot = previousSlot;
                g_xmenLiveChainExpectedReturn = previousReturn;
                g_xmenLiveChainLastObserved = previousObserved;
            }
        }
    };
    XmenLiveChainGuard xmenLiveChainGuard{
        traceXmenChainBuilderCall,
        previousLiveChainSlot,
        previousLiveChainReturn,
        previousLiveChainObserved};

    targetFn(rdram, ctx, this);
    if (traceXmenChainBuilderCall && logXmenChainDispatch)
    {
        const GuestThread *owner = m_eeScheduler ? m_eeScheduler->currentThread() : nullptr;
        const R5900Context *active = owner ? &owner->activeContext() : nullptr;
        std::cerr << "[xmen-chain-dispatch:exit] thread=" << std::dec
                  << (owner ? owner->id : 0)
                  << " invocations=" << (owner ? owner->invocations.size() : 0u)
                  << " same-context=" << (active == ctx ? 1 : 0)
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " sp=0x" << GPR_U32(ctx, 29)
                  << " slot=0x" << xmenChainSavedReturnSlot
                  << " slotValue=0x" << readRdramProbeU64(rdram, xmenChainSavedReturnSlot)
                  << " yielded="
                  << (g_guestDispatchYieldGeneration != yieldGeneration ? 1 : 0)
                  << " basePc=0x" << (owner ? owner->context.pc : 0u)
                  << " baseRa=0x" << (owner ? getRegU32(&owner->context, 31) : 0u)
                  << " baseSp=0x" << (owner ? getRegU32(&owner->context, 29) : 0u)
                  << std::dec << std::endl;
    }
    if (isXmenMovieStateCall && xmenMovieStateTraceIndex < 64u)
    {
        const uint32_t owner = xmenMovieStateRecord != 0u
                                   ? readRdramProbeU32(
                                         rdram, xmenMovieStateRecord + 0x40u)
                                   : 0u;
        std::cerr << "[xmen-movie-state:exit] targetIndex=" << std::dec
                  << xmenMovieStateIndex
                  << " call=" << xmenMovieStateTraceIndex
                  << " thread=" << eeScheduler().currentThreadId()
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " record=0x" << xmenMovieStateRecord
                  << " control70=0x"
                  << (xmenMovieStateRecord != 0u
                          ? readRdramProbeU32(
                                rdram, xmenMovieStateRecord + 0x70u)
                          : 0u)
                  << " owner=0x" << owner
                  << " owner44=0x" << readRdramProbeU32(rdram, owner + 0x44u)
                  << " owner48=0x" << readRdramProbeU32(rdram, owner + 0x48u)
                  << " owner4C=0x" << readRdramProbeU32(rdram, owner + 0x4Cu)
                  << " manager24=0x"
                  << readRdramProbeU32(rdram, 0x00666DA4u)
                  << std::dec << std::endl;
    }
    if (isXmenMoviePipelineCall && xmenMoviePipelineTraceIndex < 64u)
    {
        const uint32_t record = GPR_U32(ctx, 16);
        const uint32_t owner = readRdramProbeU32(rdram, record + 0x40u);
        const uint32_t stack = GPR_U32(ctx, 29);
        std::cerr << "[xmen-movie-pipeline:exit] stage=" << std::dec
                  << xmenMoviePipelineIndex
                  << " call=" << xmenMoviePipelineTraceIndex
                  << " thread=" << eeScheduler().currentThreadId()
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " record=0x" << record
                  << " recordState=0x" << readRdramProbeU32(rdram, record + 4u)
                  << " recordId=0x" << readRdramProbeU32(rdram, record + 0xCu)
                  << " owner=0x" << owner
                  << " owner48=0x" << readRdramProbeU32(rdram, owner + 0x48u)
                  << " owner4C=0x" << readRdramProbeU32(rdram, owner + 0x4Cu)
                  << " sp0=0x" << readRdramProbeU32(rdram, stack)
                  << " manager24=0x" << readRdramProbeU32(rdram, 0x00666DA4u)
                  << std::dec << std::endl;
    }
    if (isXmenSofdecServerCall && xmenSofdecServerTraceIndex < 128u)
    {
        std::cerr << "[xmen-sofdec-server:exit] targetIndex=" << std::dec
                  << xmenSofdecServerIndex
                  << " call=" << xmenSofdecServerTraceIndex
                  << " thread=" << eeScheduler().currentThreadId()
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " state0=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9A0u)
                  << " state1=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9A4u)
                  << " state2=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9A8u)
                  << " state3=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9ACu)
                  << " state4=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9B0u)
                  << " state5=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x9B4u)
                  << " decoder=0x" << readRdramProbeU32(rdram, xmenSofdecServerOwner + 0x1C68u)
                  << std::dec << std::endl;
    }
    if (isXmenMpegPathCall && xmenMpegPathTraceIndex < 64u)
    {
        std::cerr << "[xmen-mpeg-path:exit] targetIndex=" << std::dec << xmenMpegPathIndex
                  << " call=" << xmenMpegPathTraceIndex
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " decoder=0x" << xmenMpegDecoderBase
                  << " globalOwner=0x" << readRdramProbeU32(rdram, 0x006680E8u)
                  << " ready=0x" << readRdramProbeU32(rdram, xmenMpegDecoderBase + 0xDA8u)
                  << " state=0x" << xmenMpegIpuState
                  << " state0=0x" << readRdramProbeU32(rdram, xmenMpegIpuState)
                  << " state4=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 4u)
                  << " state8=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 8u)
                  << " stateC=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0xCu)
                  << " state10=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x10u)
                  << " state14=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x14u)
                  << " state18=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x18u)
                  << " state20=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x20u)
                  << " state44=0x" << readRdramProbeU32(rdram, xmenMpegIpuState + 0x44u)
                  << std::dec << std::endl;
    }
    if (isXmenMovieWorkerDispatch && xmenMovieWorkerTraceIndex < 8u)
    {
        std::cerr << "[xmen-movie-worker:exit] thread=" << std::dec
                  << eeScheduler().currentThreadId()
                  << " category=" << xmenMovieWorkerCategory
                  << " slot=" << xmenMovieWorkerSlot
                  << " call=" << xmenMovieWorkerTraceIndex
                  << " target=0x" << std::hex << targetPc
                  << " result=0x" << GPR_U32(ctx, 2)
                  << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                  << " streamC=0x" << readRdramProbeU32(rdram, 0x00678804u)
                  << " stream20=0x" << readRdramProbeU32(rdram, 0x00678818u)
                  << " stream24=0x" << readRdramProbeU32(rdram, 0x0067881Cu)
                  << " stream28=0x" << readRdramProbeU32(rdram, 0x00678820u)
                  << " stream34=0x" << readRdramProbeU32(rdram, 0x0067882Cu)
                  << " stream58=0x" << readRdramProbeU32(rdram, 0x00678850u)
                  << " wrapper0=0x" << readRdramProbeU32(rdram, 0x0066A168u)
                  << " wrapper14=0x" << readRdramProbeU32(rdram, 0x0066A17Cu)
                  << " wrapper20=0x" << readRdramProbeU32(rdram, 0x0066A188u)
                  << std::dec << std::endl;
    }
    if (isXmenMovieStreamCallback && xmenMovieStreamCallbackTraceIndex < 128u)
    {
        std::cerr << "[xmen-movie-stream-callback:exit] sourceIndex=" << std::dec
                  << xmenMovieStreamCallbackIndex
                  << " call=" << xmenMovieStreamCallbackTraceIndex
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " output=0x" << xmenMovieStreamCallbackOutput
                  << " out0=0x" << readRdramProbeU32(rdram, xmenMovieStreamCallbackOutput)
                  << " out4=0x" << readRdramProbeU32(rdram, xmenMovieStreamCallbackOutput + 4u)
                  << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                  << std::dec << std::endl;
    }
    if (isXmenMovieCallbackGateCall && xmenMovieCallbackGateTraceIndex < 128u)
    {
        std::cerr << "[xmen-movie-callback:exit] index=" << std::dec
                  << xmenMovieCallbackGateTraceIndex
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " s0=0x" << GPR_U32(ctx, 16)
                  << " s1=0x" << GPR_U32(ctx, 17)
                  << " s2=0x" << GPR_U32(ctx, 18)
                  << " active=0x" << readRdramProbeU32(rdram, 0x0066DCF4u)
                  << " initialized=0x" << readRdramProbeU32(rdram, 0x00666D7Cu)
                  << " manager10=0x" << readRdramProbeU32(rdram, 0x00666D90u)
                  << " manager48=0x" << readRdramProbeU32(rdram, 0x00666DC8u)
                  << " manager4C=0x" << readRdramProbeU32(rdram, 0x00666DCCu)
                  << " manager54=0x" << readRdramProbeU32(rdram, 0x00666DD4u)
                  << " manager60=0x" << readRdramProbeU32(rdram, 0x00666DE0u)
                  << " manager64=0x" << readRdramProbeU32(rdram, 0x00666DE4u)
                  << " manager68=0x" << readRdramProbeU32(rdram, 0x00666DE8u)
                  << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                  << std::dec << std::endl;
    }
    if (isXmenMovieCall && xmenMovieCallTraceIndex < 64u)
    {
        std::cerr << "[xmen-movie-call:exit] targetIndex=" << std::dec
                  << xmenMovieCallIndex
                  << " call=" << xmenMovieCallTraceIndex
                  << " source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " object=0x" << xmenMovieCallObject
                  << " stream=0x" << xmenMovieStreamObject
                  << " wrapper0=0x" << readRdramProbeU32(rdram, xmenMovieCallObject)
                  << " wrapper1C=0x" << readRdramProbeU32(rdram, xmenMovieCallObject + 0x1Cu)
                  << " stream0=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject)
                  << " stream8=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 8u)
                  << " stream10=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x10u)
                  << " stream14=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x14u)
                  << " stream44=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x44u)
                  << " stream48=0x" << readRdramProbeU32(rdram, xmenMovieStreamObject + 0x48u)
                  << std::dec << std::endl;
    }
    if (targetPc == 0x004A32E0u &&
        (sourcePc == 0x0014CB0Cu || sourcePc == 0x0014CBB4u || sourcePc == 0x0014CC5Cu))
    {
        static std::atomic<uint32_t> s_xmenFactoryLookupExitTraceCount{0u};
        const uint32_t count = s_xmenFactoryLookupExitTraceCount.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t callerStack = GPR_U32(ctx, 29);
        const uint32_t outputValue = readRdramProbeU32(rdram, callerStack + 0x4Cu);
        if (count < 32u || GPR_U32(ctx, 2) == 0u || outputValue > 0x20u)
        {
            std::cerr << "[xmen-factory-lookup:exit] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " pc=0x" << ctx->pc
                      << " result=0x" << GPR_U32(ctx, 2)
                      << " callerStack=0x" << callerStack
                      << " outputValue=0x" << outputValue
                      << " s1=0x" << GPR_U32(ctx, 17)
                      << " s2=0x" << GPR_U32(ctx, 18)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (isXmenLegalPackageLookupCall || isXmenLegalMetadataExtractCall)
    {
        const uint32_t result = GPR_U32(ctx, 2);
        std::cerr << "[xmen-legal-metadata:exit] source=0x" << std::hex << sourcePc
                  << " pc=0x" << ctx->pc
                  << " result=0x" << result
                  << " resultVtable=0x" << readRdramProbeU32(rdram, result)
                  << " result8=0x" << readRdramProbeU32(rdram, result + 8u)
                  << std::dec << std::endl;
    }
    if (isXmenObjectAcquireCall)
    {
        std::cerr << "[xmen-object-acquire:exit] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " outValue=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 4))
                  << " ownerField=0x" << GPR_U32(ctx, 6)
                  << " ownerValue=0x" << readRdramProbeU32(rdram, GPR_U32(ctx, 6))
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " trace=" << formatDispatchHistory()
                  << std::dec << std::endl;
    }
    if (isXmenObjectMethodCall)
    {
        std::cerr << "[xmen-object-method:exit] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << " f0=" << std::dec << ctx->f[0]
                  << " trace=" << formatDispatchHistory()
                  << std::endl;
    }
    if (isXmenLevelActivationCall)
    {
        std::cout << "[xmen-level-activation:exit] source=0x" << std::hex << sourcePc
                  << " pc=0x" << ctx->pc
                  << " manager=0x" << xmenActivationManager
                  << " active=0x" << readRdramProbeU32(rdram, xmenActivationManager + 0x0C08u)
                  << " requested=0x" << xmenActivationObject
                  << std::dec << std::endl;
    }
    if (isXmenLevelObjectLookupCall)
    {
        const uint32_t result = GPR_U32(ctx, 2);
        std::cout << "[xmen-level-object-lookup:exit] source=0x" << std::hex << sourcePc
                  << " result=0x" << result
                  << " word0=0x" << readRdramProbeU32(rdram, result)
                  << " name=\"" << readGuestPrintableString(rdram, result + 0x74u, 160u) << "\""
                  << std::dec << std::endl;
    }
    if (isXmenLevelLoadEntryCall)
    {
        std::cout << "[xmen-level-load:exit] source=0x" << std::hex << sourcePc
                  << " manager=0x" << xmenLevelLoadManager
                  << " active=0x" << readRdramProbeU32(rdram, xmenLevelLoadManager + 0x0C08u)
                  << std::dec << std::endl;
    }
    if (isXmenGraphicsBootstrapCall)
    {
        std::cerr << "[xmen-graphics-bootstrap:exit] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << std::dec << std::endl;
    }
    if (isXmenPlatformInitCall)
    {
        std::cerr << "[xmen-platform-init:exit] source=0x" << std::hex << sourcePc
                  << " target=0x" << targetPc
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << " v0=0x" << GPR_U32(ctx, 2)
                  << std::dec << std::endl;
    }
    if (isXmenFontPoolAllocation)
    {
        std::cerr << "[xmen-font-pool-alloc:exit] result=0x" << std::hex << GPR_U32(ctx, 2)
                  << " pc=0x" << ctx->pc
                  << " ra=0x" << GPR_U32(ctx, 31)
                  << std::dec << std::endl;
    }
    if (isXmenFrameTimingCall)
    {
        static std::atomic<uint32_t> s_xmenFrameTimingExitCount{0u};
        const uint32_t count = s_xmenFrameTimingExitCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 96u)
        {
            std::cerr << "[xmen-frame-timing:exit] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " pc=0x" << ctx->pc
                      << " f0=" << std::dec << ctx->f[0]
                      << " f1=" << ctx->f[1]
                      << " f2=" << ctx->f[2]
                      << " f20=" << ctx->f[20]
                      << std::endl;
        }
    }
    logXmenVtableTripwire(rdram, ctx, "after-target", sourcePc, targetPc, kind);

    if (ctx->pc == 0u)
    {
        static std::atomic<uint32_t> s_xmenZeroPcDispatchTraceCount{0u};
        const uint32_t count = s_xmenZeroPcDispatchTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (count < 128u)
        {
            std::cerr << "[xmen-zero-pc-dispatch] index=" << std::dec << count
                      << " source=0x" << std::hex << sourcePc
                      << " target=0x" << targetPc
                      << " return=0x" << fallthroughPc
                      << " kind=" << describeGuestBranchKind(kind)
                      << " entry=0x" << entryPc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " sp=0x" << GPR_U32(ctx, 29)
                      << " v0=0x" << GPR_U32(ctx, 2)
                      << " a0=0x" << GPR_U32(ctx, 4)
                      << " a1=0x" << GPR_U32(ctx, 5)
                      << " a2=0x" << GPR_U32(ctx, 6)
                      << " depth=0x" << s_nativeDispatchDepth
                      << " trace=" << formatDispatchHistory()
                      << std::dec << std::endl;
        }
    }

    if (isStopRequested() || ctx->pc == 0u)
    {
        return false;
    }

    if (g_guestDispatchYieldGeneration != yieldGeneration)
    {
        return false;
    }

    if (ctx->pc == entryPc)
    {
        ctx->pc = fallthroughPc;
    }

    return ctx->pc == fallthroughPc;
}

void PS2Runtime::SignalException(R5900Context *ctx, PS2Exception exception)
{
    if (exception == EXCEPTION_INTEGER_OVERFLOW)
    {
        HandleIntegerOverflow(ctx);
        return;
    }

    raiseCop0Exception(ctx, static_cast<uint32_t>(exception),
                       exception == EXCEPTION_TLB_REFILL);
}

void PS2Runtime::executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    (void)rdram;

    uint8_t *const vu0Code = m_memory.getVU0Code();
    uint8_t *const vu0Data = m_memory.getVU0Data();
    const uint32_t startPC = address & ~0x7u;

    if (!vu0Code || !vu0Data || startPC + 8u > PS2_VU0_CODE_SIZE)
    {
        seedVu0IdleSuccess(ctx);
        return;
    }

    m_vu0.reset();
    copyVu0ContextToState(ctx, m_vu0.state());
    m_vu0.execute(vu0Code, PS2_VU0_CODE_SIZE,
                  vu0Data, PS2_VU0_DATA_SIZE,
                  m_gs, &m_memory,
                  startPC, 0u, ctx->vu0_itop, 4096);
    copyVu0StateToContext(m_vu0.state(), ctx);
}

void PS2Runtime::vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address)
{
    // VCALLMS and VCALLMSR both route here.
    executeVU0Microprogram(rdram, ctx, address);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx)
{
    handleSyscall(rdram, ctx, 0);
}

void PS2Runtime::handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
{
    if (ctx->in_delay_slot)
    {
        throw std::runtime_error("Attempted to execute a syscall inside a branch delay slot! "
                                 "This breaks the atomic basic block model and is structurally unsupported by the emulator.");
    }

    const uint32_t syscallId = (encodedSyscallId != 0u)
                                   ? encodedSyscallId
                                   : getRegU32(ctx, 3); // $v1 / $3 is the EE kernel syscall number

    if (ps2_syscalls::dispatchNumericSyscall(syscallId, rdram, ctx, this))
    {
        return;
    }

    // God help you
    ps2_syscalls::TODO(rdram, ctx, this, encodedSyscallId);
}

void PS2Runtime::handleBreak(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_BREAKPOINT);
}

void PS2Runtime::drainCompletedDmacHandlers(uint8_t *rdram)
{
    for (const PS2Memory::DmacCompletion completion : m_memory.consumeCompletedDmacCauses())
    {
        ps2_syscalls::dispatchDmacHandlersForCause(rdram, this, completion.cause,
                                                   completion.delayCycles);
    }
}

void PS2Runtime::handleTrap(uint8_t *rdram, R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_TRAP);
}

void PS2Runtime::handleTLBR(uint8_t *rdram, R5900Context *ctx)
{
    uint32_t vpn = 0;
    uint32_t pfn = 0;
    uint32_t mask = 0;
    bool valid = false;

    const uint32_t index = ctx->cop0_index & 0x3Fu;
    if (!m_memory.tlbRead(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Preserve low ASID bits in EntryHi.
    ctx->cop0_entryhi = (ctx->cop0_entryhi & 0x00000FFFu) | (vpn & 0xFFFFF000u);
    ctx->cop0_entrylo0 = (ctx->cop0_entrylo0 & ~0x03FFFFC2u) |
                         ((pfn & 0x000FFFFFu) << 6) |
                         (valid ? 0x2u : 0u);
    ctx->cop0_pagemask = mask & 0x01FFE000u;
}

void PS2Runtime::handleTLBWI(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t index = ctx->cop0_index & 0x3Fu;
    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(index, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
    }
}

void PS2Runtime::handleTLBWR(uint8_t *rdram, R5900Context *ctx)
{
    const uint32_t entryCount = static_cast<uint32_t>(m_memory.tlbEntryCount());
    if (entryCount == 0)
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    const uint32_t wired = std::min(ctx->cop0_wired, entryCount - 1);
    uint32_t random = ctx->cop0_random % entryCount;
    if (random < wired)
    {
        random = wired;
    }

    const uint32_t vpn = ctx->cop0_entryhi & 0xFFFFF000u;
    const uint32_t pfn = (ctx->cop0_entrylo0 >> 6) & 0x000FFFFFu;
    const uint32_t mask = ctx->cop0_pagemask & 0x01FFE000u;
    const bool valid = (ctx->cop0_entrylo0 & 0x2u) != 0u;

    if (!m_memory.tlbWrite(random, vpn, pfn, mask, valid))
    {
        raiseCop0Exception(ctx, EXCEPTION_RESERVED_INSTRUCTION);
        return;
    }

    // Keep COP0 bookkeeping in sync with the selected slot.
    ctx->cop0_index = (ctx->cop0_index & ~0x3Fu) | (random & 0x3Fu);
    ctx->cop0_random = (random <= wired) ? (entryCount - 1) : (random - 1);
}

void PS2Runtime::handleTLBP(uint8_t *rdram, R5900Context *ctx)
{
    const int32_t index = m_memory.tlbProbe(ctx->cop0_entryhi & 0xFFFFF000u);
    if (index >= 0)
    {
        ctx->cop0_index = (ctx->cop0_index & ~0x8000003Fu) |
                          (static_cast<uint32_t>(index) & 0x3Fu);
    }
    else
    {
        // MIPS sets probe failure bit (P) in Index[31].
        ctx->cop0_index |= 0x80000000u;
    }
}

void PS2Runtime::clearLLBit(R5900Context *ctx)
{
    // LL/SC reservation is tracked separately from COP0 Status.
    ctx->llbit = 0;
    ctx->lladdr = 0;
}

uint32_t PS2Runtime::alignGuestHeapValue(uint32_t value, uint32_t alignment)
{
    if (alignment == 0)
    {
        return value;
    }

    const uint32_t mask = alignment - 1u;
    if (value > (std::numeric_limits<uint32_t>::max() - mask))
    {
        return std::numeric_limits<uint32_t>::max();
    }
    return (value + mask) & ~mask;
}

bool PS2Runtime::isGuestHeapAlignmentValid(uint32_t alignment)
{
    return alignment != 0u && (alignment & (alignment - 1u)) == 0u;
}

uint32_t PS2Runtime::normalizeGuestHeapAlignment(uint32_t alignment)
{
    if (!isGuestHeapAlignmentValid(alignment))
    {
        return kGuestHeapDefaultAlignment;
    }
    return std::max(alignment, kGuestHeapDefaultAlignment);
}

uint32_t PS2Runtime::clampGuestHeapBase(uint32_t guestBase) const
{
    uint32_t normalized = guestBase;
    if (normalized >= PS2_RAM_SIZE)
    {
        normalized &= PS2_RAM_MASK;
    }
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    return std::min(normalized, hardLimit);
}

uint32_t PS2Runtime::clampGuestHeapLimit(uint32_t guestLimit) const
{
    const uint32_t hardLimit = std::min(kGuestHeapHardLimit, PS2_RAM_SIZE);
    if (guestLimit == 0u || guestLimit > hardLimit)
    {
        return hardLimit;
    }
    return guestLimit;
}

void PS2Runtime::resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit)
{
    uint32_t base = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    uint32_t limit = clampGuestHeapLimit(guestLimit);
    if (base == 0u)
    {
        const uint32_t fallbackBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
        base = alignGuestHeapValue(clampGuestHeapBase(fallbackBase), kGuestHeapDefaultAlignment);
    }

    if (limit <= base)
    {
        base = alignGuestHeapValue(clampGuestHeapBase(m_guestHeapSuggestedBase), kGuestHeapDefaultAlignment);
        limit = clampGuestHeapLimit(0u);
    }

    if (limit <= base)
    {
        base = 0u;
        limit = 0u;
    }

    m_guestHeapBlocks.clear();
    if (limit > base)
    {
        m_guestHeapBlocks.push_back({base, limit - base, true});
    }

    m_guestHeapBase = base;
    m_guestHeapEnd = base;
    m_guestHeapLimit = limit;
    m_guestHeapConfigured = true;
}

void PS2Runtime::ensureGuestHeapInitializedLocked()
{
    if (m_guestHeapConfigured)
    {
        return;
    }

    const uint32_t suggested = (m_guestHeapSuggestedBase == 0u) ? kGuestHeapDefaultBase : m_guestHeapSuggestedBase;
    resetGuestHeapLocked(suggested, clampGuestHeapLimit(0u));
}

int32_t PS2Runtime::findGuestHeapBlockIndexLocked(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock &block = m_guestHeapBlocks[i];
        if (!block.free && block.addr == normalizedAddr)
        {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

uint32_t PS2Runtime::allocateGuestBlockLocked(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    if (size > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    for (size_t i = 0; i < m_guestHeapBlocks.size(); ++i)
    {
        const GuestHeapBlock block = m_guestHeapBlocks[i];
        if (!block.free)
        {
            continue;
        }

        const uint64_t blockStart = block.addr;
        const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
        const uint32_t alignedAddr = alignGuestHeapValue(block.addr, normalizedAlignment);
        if (alignedAddr < block.addr)
        {
            continue;
        }

        const uint64_t alignedStart = alignedAddr;
        if (alignedStart > blockEnd)
        {
            continue;
        }

        const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
        if (allocEnd > blockEnd)
        {
            continue;
        }

        const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
        const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);

        std::vector<GuestHeapBlock> replacement;
        replacement.reserve(3);
        if (prefixSize > 0u)
        {
            replacement.push_back({block.addr, prefixSize, true});
        }
        replacement.push_back({alignedAddr, allocSize, false});
        if (suffixSize > 0u)
        {
            replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
        }

        m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
        m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i),
                                 replacement.begin(),
                                 replacement.end());

        m_guestHeapEnd = std::max(m_guestHeapEnd, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    return 0u;
}

void PS2Runtime::coalesceGuestHeapLocked()
{
    if (m_guestHeapBlocks.empty())
    {
        return;
    }

    size_t i = 1;
    while (i < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &prev = m_guestHeapBlocks[i - 1];
        GuestHeapBlock &curr = m_guestHeapBlocks[i];
        const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
        if (prev.free && curr.free && prevEnd == curr.addr)
        {
            prev.size += curr.size;
            m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void PS2Runtime::freeGuestBlockLocked(uint32_t guestAddr)
{
    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return;
    }

    m_guestHeapBlocks[static_cast<size_t>(index)].free = true;
    coalesceGuestHeapLocked();
}

void PS2Runtime::configureGuestHeap(uint32_t guestBase, uint32_t guestLimit)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    uint32_t normalizedBase = alignGuestHeapValue(clampGuestHeapBase(guestBase), kGuestHeapDefaultAlignment);
    if (normalizedBase == 0u)
    {
        normalizedBase = (m_guestHeapSuggestedBase != 0u) ? m_guestHeapSuggestedBase : kGuestHeapDefaultBase;
    }
    m_guestHeapSuggestedBase = normalizedBase;
    resetGuestHeapLocked(normalizedBase, guestLimit);
}

uint32_t PS2Runtime::guestMalloc(uint32_t size, uint32_t alignment)
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    return allocateGuestBlockLocked(size, alignment);
}

uint32_t PS2Runtime::guestCalloc(uint32_t count, uint32_t size, uint32_t alignment)
{
    if (count == 0u || size == 0u)
    {
        return 0u;
    }
    if (count > (std::numeric_limits<uint32_t>::max() / size))
    {
        return 0u;
    }

    const uint32_t totalSize = count * size;
    const uint32_t guestAddr = guestMalloc(totalSize, alignment);
    if (guestAddr != 0u)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (rdram)
        {
            uint32_t physAddr = guestAddr & PS2_RAM_MASK;
            if (physAddr + totalSize <= PS2_RAM_SIZE)
                std::memset(rdram + physAddr, 0, totalSize);
        }
    }

    return guestAddr;
}

uint32_t PS2Runtime::guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
{
    if (guestAddr == 0u)
    {
        return guestMalloc(newSize, alignment);
    }
    if (newSize == 0u)
    {
        guestFree(guestAddr);
        return 0u;
    }

    if (newSize > (std::numeric_limits<uint32_t>::max() - (kGuestHeapDefaultAlignment - 1u)))
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t requestedSize = alignGuestHeapValue(newSize, kGuestHeapDefaultAlignment);

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();

    const int32_t index = findGuestHeapBlockIndexLocked(guestAddr);
    if (index < 0)
    {
        return 0u;
    }

    const size_t blockIndex = static_cast<size_t>(index);
    const uint32_t oldAddr = m_guestHeapBlocks[blockIndex].addr;
    const uint32_t oldSize = m_guestHeapBlocks[blockIndex].size;

    if (requestedSize <= oldSize)
    {
        if (requestedSize < oldSize)
        {
            const uint32_t tailAddr = oldAddr + requestedSize;
            const uint32_t tailSize = oldSize - requestedSize;
            m_guestHeapBlocks[blockIndex].size = requestedSize;
            m_guestHeapBlocks.insert(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                     GuestHeapBlock{tailAddr, tailSize, true});
            coalesceGuestHeapLocked();
        }
        return oldAddr;
    }

    if (blockIndex + 1u < m_guestHeapBlocks.size())
    {
        GuestHeapBlock &next = m_guestHeapBlocks[blockIndex + 1u];
        const uint64_t blockEnd = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].addr) +
                                  static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size);
        if (next.free && blockEnd == next.addr)
        {
            const uint64_t combined = static_cast<uint64_t>(m_guestHeapBlocks[blockIndex].size) +
                                      static_cast<uint64_t>(next.size);
            if (combined >= requestedSize)
            {
                const uint32_t extraNeeded = requestedSize - m_guestHeapBlocks[blockIndex].size;
                m_guestHeapBlocks[blockIndex].size = requestedSize;
                if (next.size == extraNeeded)
                {
                    m_guestHeapBlocks.erase(m_guestHeapBlocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                }
                else
                {
                    next.addr += extraNeeded;
                    next.size -= extraNeeded;
                }
                m_guestHeapEnd = std::max(m_guestHeapEnd, oldAddr + requestedSize);
                return oldAddr;
            }
        }
    }

    const uint32_t newAddr = allocateGuestBlockLocked(newSize, normalizedAlignment);
    if (newAddr == 0u)
    {
        return 0u;
    }

    uint8_t *rdram = m_memory.getRDRAM();
    if (rdram)
    {
        const uint32_t copyBytes = std::min(oldSize, newSize);
        uint32_t dstPhys = newAddr & PS2_RAM_MASK;
        uint32_t srcPhys = oldAddr & PS2_RAM_MASK;
        if (dstPhys + copyBytes <= PS2_RAM_SIZE && srcPhys + copyBytes <= PS2_RAM_SIZE)
            std::memmove(rdram + dstPhys, rdram + srcPhys, copyBytes);
    }

    freeGuestBlockLocked(oldAddr);
    return newAddr;
}

uint32_t PS2Runtime::guestAllocationRemainingSize(uint32_t guestAddr) const
{
    const uint32_t normalizedAddr = guestAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    for (const GuestHeapBlock &block : m_guestHeapBlocks)
    {
        const uint64_t blockEnd = static_cast<uint64_t>(block.addr) + block.size;
        if (!block.free && normalizedAddr >= block.addr && normalizedAddr < blockEnd)
        {
            return static_cast<uint32_t>(blockEnd - normalizedAddr);
        }
    }
    return 0u;
}

void PS2Runtime::guestFree(uint32_t guestAddr)
{
    if (guestAddr == 0u)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    ensureGuestHeapInitializedLocked();
    freeGuestBlockLocked(guestAddr);
}

uint32_t PS2Runtime::guestHeapBase() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapBase : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapEnd() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapEnd : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::guestHeapLimit() const
{
    std::lock_guard<std::mutex> lock(m_guestHeapMutex);
    return m_guestHeapConfigured ? m_guestHeapLimit : m_guestHeapSuggestedBase;
}

uint32_t PS2Runtime::reserveAsyncCallbackStack(uint32_t size, uint32_t alignment)
{
    if (size == 0u)
    {
        return 0u;
    }

    const uint32_t normalizedAlignment = normalizeGuestHeapAlignment(alignment);
    const uint32_t allocSize = alignGuestHeapValue(size, kGuestHeapDefaultAlignment);
    if (allocSize == 0u)
    {
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_asyncCallbackStackMutex);
    uint32_t top = m_asyncCallbackStackTop;
    if (top > PS2_RAM_SIZE)
    {
        top = PS2_RAM_SIZE;
    }
    top &= ~(kGuestHeapDefaultAlignment - 1u);

    if (top <= allocSize)
    {
        return 0u;
    }

    uint32_t base = top - allocSize;
    base &= ~(normalizedAlignment - 1u);
    if (base < m_asyncCallbackStackFloor || base >= top)
    {
        return 0u;
    }

    m_asyncCallbackStackTop = base;
    return top - 0x10u;
}

uint8_t PS2Runtime::Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read8(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint16_t PS2Runtime::Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read16(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint32_t PS2Runtime::Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        const uint32_t value = m_memory.read32(vaddr);
        if (isIpuHardwareAddress(vaddr))
        {
            static std::atomic<uint32_t> s_xmenIpuRead32Count{0u};
            uint32_t index = 0u;
            if (shouldTraceIpuHardwareAccess(s_xmenIpuRead32Count, index))
            {
                std::cerr << "[xmen-ipu:read32] index=" << std::dec << index
                          << " pc=0x" << std::hex << ctx->pc
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " vaddr=0x" << vaddr
                          << " physical=0x" << (vaddr & 0x1FFFFFFFu)
                          << " value=0x" << value
                          << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                          << std::dec << std::endl;
            }
        }
        return value;
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

uint64_t PS2Runtime::Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        const uint64_t value = m_memory.read64(vaddr);
        if (isIpuHardwareAddress(vaddr))
        {
            static std::atomic<uint32_t> s_xmenIpuRead64Count{0u};
            uint32_t index = 0u;
            if (shouldTraceIpuHardwareAccess(s_xmenIpuRead64Count, index))
            {
                std::cerr << "[xmen-ipu:read64] index=" << std::dec << index
                          << " pc=0x" << std::hex << ctx->pc
                          << " ra=0x" << GPR_U32(ctx, 31)
                          << " vaddr=0x" << vaddr
                          << " physical=0x" << (vaddr & 0x1FFFFFFFu)
                          << " value=0x" << value
                          << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                          << std::dec << std::endl;
            }
        }
        return value;
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return 0;
    }
}

__m128i PS2Runtime::Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr)
{
    try
    {
        return m_memory.read128(vaddr);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_LOAD);
        return _mm_setzero_si128();
    }
}

void PS2Runtime::Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value)
{
    traceXmenLiveChainWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    ps2TraceGuestWrite(rdram, vaddr, 1u, value, 0u, "WRITE8", ctx);
    try
    {
        m_memory.write8(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value)
{
    traceXmenLiveChainWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    ps2TraceGuestWrite(rdram, vaddr, 2u, value, 0u, "WRITE16", ctx);
    try
    {
        m_memory.write16(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value)
{
    traceXmenLiveChainWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    ps2TraceGuestWrite(rdram, vaddr, 4u, value, 0u, "WRITE32", ctx);
    if (isIpuHardwareAddress(vaddr))
    {
        static std::atomic<uint32_t> s_xmenIpuWrite32Count{0u};
        uint32_t index = 0u;
        if (shouldTraceIpuHardwareAccess(s_xmenIpuWrite32Count, index))
        {
            std::cerr << "[xmen-ipu:write32] index=" << std::dec << index
                      << " pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " vaddr=0x" << vaddr
                      << " physical=0x" << (vaddr & 0x1FFFFFFFu)
                      << " value=0x" << value
                      << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                      << std::dec << std::endl;
        }
    }
    try
    {
        m_memory.write32(vaddr, value);
        drainCompletedDmacHandlers(rdram);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value)
{
    traceXmenLiveChainWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    ps2TraceGuestWrite(rdram, vaddr, 8u, value, 0u, "WRITE64", ctx);
    if (isIpuHardwareAddress(vaddr))
    {
        static std::atomic<uint32_t> s_xmenIpuWrite64Count{0u};
        uint32_t index = 0u;
        if (shouldTraceIpuHardwareAccess(s_xmenIpuWrite64Count, index))
        {
            std::cerr << "[xmen-ipu:write64] index=" << std::dec << index
                      << " pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " vaddr=0x" << vaddr
                      << " physical=0x" << (vaddr & 0x1FFFFFFFu)
                      << " value=0x" << value
                      << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                      << std::dec << std::endl;
        }
    }
    try
    {
        m_memory.write64(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value)
{
    alignas(16) uint64_t _parts[2];
    _mm_storeu_si128(reinterpret_cast<__m128i *>(_parts), value);
    traceXmenLiveChainWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    ps2TraceGuestWrite(rdram, vaddr, 16u, _parts[0], _parts[1], "WRITE128", ctx);
    if (isIpuHardwareAddress(vaddr))
    {
        static std::atomic<uint32_t> s_xmenIpuWrite128Count{0u};
        uint32_t index = 0u;
        if (shouldTraceIpuHardwareAccess(s_xmenIpuWrite128Count, index))
        {
            std::cerr << "[xmen-ipu:write128] index=" << std::dec << index
                      << " pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << GPR_U32(ctx, 31)
                      << " vaddr=0x" << vaddr
                      << " physical=0x" << (vaddr & 0x1FFFFFFFu)
                      << " lo=0x" << _parts[0]
                      << " hi=0x" << _parts[1]
                      << " stream0=0x" << readRdramProbeU32(rdram, 0x006787F8u)
                      << std::dec << std::endl;
        }
    }
    try
    {
        m_memory.write128(vaddr, value);
    }
    catch (const std::exception &)
    {
        SignalException(ctx, EXCEPTION_ADDRESS_ERROR_STORE);
    }
}

void PS2Runtime::kickGifDmaChainFromMMIO(uint8_t *rdram,
                                         R5900Context *ctx,
                                         uint32_t dPcrValue,
                                         uint32_t dStatValue,
                                         uint32_t tadr,
                                         uint32_t chcr)
{
    constexpr uint32_t D_PCR = 0x1000E020u;
    constexpr uint32_t D_STAT = 0x1000E010u;
    constexpr uint32_t GIF_TADR = 0x1000A030u;
    constexpr uint32_t GIF_CHCR = 0x1000A000u;

    ps2TraceGuestWrite(rdram, D_PCR, 4u, dPcrValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_PCR, dPcrValue);
    ps2TraceGuestWrite(rdram, D_STAT, 4u, dStatValue, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(D_STAT, dStatValue);
    ps2TraceGuestWrite(rdram, GIF_TADR, 4u, tadr, 0u, "WRITE32", ctx);
    m_memory.writeIORegister(GIF_TADR, tadr);
    ps2TraceGuestWrite(rdram, GIF_CHCR, 4u, chcr, 0u, "WRITE32", ctx);
    if (m_memory.tryProcessNativeGifImageUploadChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    if (m_memory.tryProcessNativeGifPackedChain(m_gs, tadr, chcr))
    {
        drainCompletedDmacHandlers(rdram);
        return;
    }
    m_memory.writeIORegister(GIF_CHCR, chcr);
    m_memory.processPendingTransfers();
    drainCompletedDmacHandlers(rdram);
}

void PS2Runtime::requestStop()
{
    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_eeScheduler)
    {
        m_eeScheduler->requestStop();
    }
}

bool PS2Runtime::isStopRequested() const
{
    return m_stopRequested.load(std::memory_order_relaxed);
}

EeScheduler &PS2Runtime::eeScheduler()
{
    return *m_eeScheduler;
}

const EeScheduler &PS2Runtime::eeScheduler() const
{
    return *m_eeScheduler;
}

void PS2Runtime::postEeEvent(EeEvent event)
{
    m_eeScheduler->postEvent(event);
}

bool PS2Runtime::eeCheckpointDue(uint32_t cycles) noexcept
{
    const bool due = m_eeScheduler->checkpointDue(cycles);
    if (due)
    {
        ++g_guestDispatchYieldGeneration;
    }
    return due;
}

[[noreturn]] void PS2Runtime::eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc)
{
    const uint64_t currentTick = m_eeScheduler->currentVSyncTick();
    const uint64_t waitTicks = std::max<uint64_t>(1u, ticks);
    m_eeScheduler->waitVSync(currentTick + waitTicks - 1u,
                             0,
                             [resumePc](R5900Context &context)
                             {
                                 context.pc = resumePc;
                             });
}

void PS2Runtime::addEeExitHandler(int threadId, uint32_t function, uint32_t argument)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers[threadId].push_back({function, argument});
}

std::vector<PS2Runtime::EeExitHandlerRegistration> PS2Runtime::takeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    auto it = m_eeExitHandlers.find(threadId);
    if (it == m_eeExitHandlers.end())
    {
        return {};
    }
    auto handlers = std::move(it->second);
    m_eeExitHandlers.erase(it);
    return handlers;
}

void PS2Runtime::removeEeExitHandlers(int threadId)
{
    std::lock_guard lock(m_eeKernelStateMutex);
    m_eeExitHandlers.erase(threadId);
}

bool PS2Runtime::findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const
{
    std::lock_guard lock(m_eeKernelStateMutex);
    const auto it = m_eeSyscallOverrides.find(syscallNumber);
    if (it == m_eeSyscallOverrides.end())
    {
        return false;
    }
    handler = it->second;
    return true;
}

void PS2Runtime::setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler)
{
    constexpr uint32_t kTableBase = 0x80011F80u & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
    const int64_t address = static_cast<int64_t>(kTableBase) + offset;

    std::lock_guard lock(m_eeKernelStateMutex);
    if (handler == 0u)
    {
        m_eeSyscallOverrides.erase(syscallNumber);
    }
    else
    {
        m_eeSyscallOverrides[syscallNumber] = handler;
    }
    if (!rdram || address < 0 || address + 4 > kMirrorLimit)
    {
        return;
    }
    const uint32_t guestAddress = static_cast<uint32_t>(address);
    std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
    if (handler == 0u)
    {
        m_eeSyscallMirrorAddresses.erase(guestAddress);
    }
    else
    {
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::initializeEeKernelState(uint8_t *rdram)
{
    if (!rdram)
    {
        return;
    }
    constexpr uint32_t kTableGuestBase = 0x80011F80u;
    constexpr uint32_t kTableBase = kTableGuestBase & 0x1FFFFFFFu;
    constexpr uint32_t kMirrorLimit = 0x00080000u;
    constexpr uint32_t kProbeBase = 0x000002F0u;

    std::lock_guard lock(m_eeKernelStateMutex);
    for (const uint32_t address : m_eeSyscallMirrorAddresses)
    {
        const uint32_t zero = 0u;
        std::memcpy(rdram + address, &zero, sizeof(zero));
    }
    m_eeSyscallMirrorAddresses.clear();
    const uint32_t high = kTableGuestBase >> 16;
    const uint32_t low = kTableGuestBase & 0xFFFFu;
    std::memcpy(rdram + kProbeBase, &high, sizeof(high));
    std::memcpy(rdram + kProbeBase + 8u, &low, sizeof(low));
    m_eeSyscallMirrorAddresses.insert(kProbeBase);
    m_eeSyscallMirrorAddresses.insert(kProbeBase + 8u);

    for (const auto &[syscallNumber, handler] : m_eeSyscallOverrides)
    {
        const int64_t offset = static_cast<int64_t>(static_cast<int32_t>(syscallNumber)) * 4;
        const int64_t address = static_cast<int64_t>(kTableBase) + offset;
        if (address < 0 || address + 4 > kMirrorLimit)
        {
            continue;
        }
        const uint32_t guestAddress = static_cast<uint32_t>(address);
        std::memcpy(rdram + guestAddress, &handler, sizeof(handler));
        m_eeSyscallMirrorAddresses.insert(guestAddress);
    }
}

void PS2Runtime::HandleIntegerOverflow(R5900Context *ctx)
{
    raiseCop0Exception(ctx, EXCEPTION_INTEGER_OVERFLOW);
}

void PS2Runtime::run()
{
    auto logXmenRunProbe = [this](const char *tag)
    {
        uint8_t *rdram = m_memory.getRDRAM();
        if (!rdram)
        {
            return;
        }
        constexpr uint32_t kProbe = 0x006F2B50u;
        std::cerr << tag;
        for (uint32_t offset = 0u; offset < 0x30u; offset += 4u)
        {
            uint32_t word = 0u;
            std::memcpy(&word, rdram + kProbe + offset, sizeof(word));
            std::cerr << " +0x" << std::hex << offset << "=0x" << word;
        }
        constexpr uint32_t kDispatchTable = 0x006AC600u;
        std::cerr << " dispatch=";
        for (uint32_t offset = 0u; offset < 0x38u; offset += 4u)
        {
            uint32_t word = 0u;
            std::memcpy(&word, rdram + kDispatchTable + offset, sizeof(word));
            std::cerr << (offset == 0u ? "" : ",") << "0x" << std::hex << word;
        }
        std::cerr << std::dec << std::endl;
    };

    m_stopRequested.store(false, std::memory_order_relaxed);
    logXmenRunProbe("[xmen-run-probe:before-resetIop]");
    ps2_stubs::resetSifState();
    resetIop();
    logXmenRunProbe("[xmen-run-probe:after-resetIop]");
    ps2_stubs::resetAudioStubState();
    ps2_stubs::resetMpegStubState();
    initializeEeKernelState(m_memory.getRDRAM());
    logXmenRunProbe("[xmen-run-probe:after-ee-kernel]");
    m_cpuContext.r[4] = _mm_setzero_si128();
    m_cpuContext.r[5] = _mm_setzero_si128();
    m_cpuContext.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(PS2_RAM_SIZE - 0x10u));
    m_debugPc.store(m_cpuContext.pc, std::memory_order_relaxed);
    m_debugRa.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)), std::memory_order_relaxed);
    m_debugSp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[29], 0)), std::memory_order_relaxed);
    m_debugGp.store(static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[28], 0)), std::memory_order_relaxed);

    RUNTIME_LOG("Starting execution at address 0x" << std::hex << m_cpuContext.pc << std::dec);

    // A blank image to use as a framebuffer
    Image blank = GenImageColor(FB_WIDTH, FB_HEIGHT, BLANK);
    Texture2D frameTex = LoadTextureFromImage(blank);
    UnloadImage(blank);

    std::atomic<bool> gameThreadFinished{false};

    std::thread gameThread([&]()
                           {
        ThreadNaming::SetCurrentThreadName("GameThread");
        try
        {
            logXmenRunProbe("[xmen-run-probe:game-thread-entry]");
            m_eeScheduler->reset(m_memory.getRDRAM(), m_cpuContext);
            logXmenRunProbe("[xmen-run-probe:after-scheduler-reset]");
            m_eeScheduler->run();
            uint32_t pc = m_debugPc.load(std::memory_order_relaxed);
            RUNTIME_LOG("Game thread returned. PC=0x" << std::hex << pc
                      << " RA=0x" << static_cast<uint32_t>(_mm_extract_epi32(m_cpuContext.r[31], 0)) << std::dec << std::endl);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Error during program execution: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "Error during program execution: unknown exception" << std::endl;
        }
        gameThreadFinished.store(true, std::memory_order_release); });

    uint64_t tick = 0;
    bool xmenZeroPcLogged = false;
    std::ofstream xmenProgressTrace("xmen_runtime_progress.log", std::ios::out | std::ios::trunc);
    while (!isStopRequested() && !gameThreadFinished.load(std::memory_order_acquire))
    {
        tick++;
        if (xmenProgressTrace.is_open() && (tick % 120u) == 0u)
        {
            const GSRegisters &gs = m_memory.gs();
            const auto eeSnapshot = m_eeScheduler->snapshot();
            xmenProgressTrace << "[run:tick] tick=" << tick
                              << " pc=0x" << std::hex << m_debugPc.load(std::memory_order_relaxed)
                              << " ra=0x" << m_debugRa.load(std::memory_order_relaxed)
                              << " sp=0x" << m_debugSp.load(std::memory_order_relaxed)
                              << " gp=0x" << m_debugGp.load(std::memory_order_relaxed)
                              << " dispfb1=0x" << gs.dispfb1
                              << " display1=0x" << gs.display1
                              << std::dec
                              << " activeThreads=" << eeSnapshot.threads.size()
                              << " runningThread=" << eeSnapshot.runningThreadId
                              << " eeCycle=" << eeSnapshot.eeCycle
                              << " nextEvent=" << eeSnapshot.nextEventCycle
                              << " dma=" << m_memory.dmaStartCount()
                              << " gif=" << m_memory.gifCopyCount()
                              << " gsw=" << m_memory.gsWriteCount()
                              << " vif=" << m_memory.vifWriteCount()
                              << " threads=[";
            for (const auto &thread : eeSnapshot.threads)
            {
                xmenProgressTrace << thread.id << ':'
                                  << static_cast<unsigned>(thread.status) << ':'
                                  << static_cast<unsigned>(thread.waitReason) << ':'
                                  << thread.waitId << ":0x" << std::hex << thread.pc
                                  << ":0x" << thread.ra
                                  << ":0x" << thread.entry << std::dec
                                  << ':' << thread.currentPriority
                                  << ':' << thread.wakeupCount
                                  << ':' << thread.suspendCount << ',';
            }
            xmenProgressTrace << "] semaphores=[";
            for (const auto &semaphore : eeSnapshot.semaphores)
            {
                xmenProgressTrace << semaphore.id << ':'
                                  << semaphore.count << ':'
                                  << semaphore.maxCount << ':'
                                  << semaphore.waiters << ',';
            }
            xmenProgressTrace << "]\n";
            xmenProgressTrace.flush();
        }
        if (!xmenZeroPcLogged && m_debugPc.load(std::memory_order_relaxed) == 0u)
        {
            xmenZeroPcLogged = true;
            const auto eeSnapshot = m_eeScheduler->snapshot();
            std::cerr << "[run:zero-pc] tick=" << tick
                      << " ra=0x" << std::hex << m_debugRa.load(std::memory_order_relaxed)
                      << " sp=0x" << m_debugSp.load(std::memory_order_relaxed)
                      << " gp=0x" << m_debugGp.load(std::memory_order_relaxed)
                      << " activeThreads=" << std::dec << eeSnapshot.threads.size()
                      << " trace=" << formatDispatchHistory()
                      << std::endl;
            if (xmenProgressTrace.is_open())
            {
                xmenProgressTrace << "[run:zero-pc] tick=" << tick
                                  << " ra=0x" << std::hex << m_debugRa.load(std::memory_order_relaxed)
                                  << " sp=0x" << m_debugSp.load(std::memory_order_relaxed)
                                  << " gp=0x" << m_debugGp.load(std::memory_order_relaxed)
                                  << " activeThreads=" << std::dec << eeSnapshot.threads.size()
                                  << " trace=" << formatDispatchHistory()
                                  << '\n';
                xmenProgressTrace.flush();
            }
        }
        PS2_IF_AGRESSIVE_LOGS({
            if ((tick % 120) == 0)
            {
                uint64_t curDma = m_memory.dmaStartCount();
                uint64_t curGif = m_memory.gifCopyCount();
                uint64_t curGs = m_memory.gsWriteCount();
                uint64_t curVif = m_memory.vifWriteCount();
                const GSRegisters &gs = m_memory.gs();
                const uint32_t dbgPc = m_debugPc.load(std::memory_order_relaxed);
                const uint32_t dbgRa = m_debugRa.load(std::memory_order_relaxed);
                const uint32_t dbgSp = m_debugSp.load(std::memory_order_relaxed);
                const uint32_t dbgGp = m_debugGp.load(std::memory_order_relaxed);
                const auto eeSnapshot = m_eeScheduler->snapshot();

                RUNTIME_LOG("[run:tick] tick=" << tick
                                               << " pc=0x" << std::hex << dbgPc
                                               << " ra=0x" << dbgRa
                                               << " sp=0x" << dbgSp
                                               << " gp=0x" << dbgGp
                                               << " dispfb1=0x" << gs.dispfb1
                                               << " display1=0x" << gs.display1
                                               << std::dec
                                               << " activeThreads=" << eeSnapshot.threads.size()
                                               << " dma=" << curDma
                                               << " gif=" << curGif
                                               << " gsw=" << curGs
                                               << " vif=" << curVif
                                               << std::endl);
            }
        });
        uint32_t presentWidth = FB_WIDTH;
        uint32_t presentHeight = DEFAULT_DISPLAY_HEIGHT;
        UploadFrame(frameTex, this, presentWidth, presentHeight);
        m_audioBackend.update();

        BeginDrawing();
        ClearBackground(BLACK);
        const float srcWidth = static_cast<float>(std::max<uint32_t>(1u, presentWidth));
        const float srcHeight = static_cast<float>(std::max<uint32_t>(1u, presentHeight));
        const float screenWidth = static_cast<float>(GetScreenWidth());
        const float screenHeight = static_cast<float>(GetScreenHeight());
        const float scale = std::min(screenWidth / srcWidth, screenHeight / srcHeight);
        const float dstWidth = srcWidth * scale;
        const float dstHeight = srcHeight * scale;
        const Rectangle srcRect{0.0f, 0.0f, srcWidth, srcHeight};
        const Rectangle dstRect{
            (screenWidth - dstWidth) * 0.5f,
            (screenHeight - dstHeight) * 0.5f,
            dstWidth,
            dstHeight};
        DrawTexturePro(frameTex, srcRect, dstRect, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
        if (m_debugUiInitialized && m_debugUiDrawCallback)
        {
            m_debugUiDrawCallback(*this, m_debugUiUserData);
        }
        EndDrawing();

        if (WindowShouldClose())
        {
            RUNTIME_LOG("[run] window close requested, breaking out of loop");
            requestStop();
            break;
        }
    }

    requestStop();
    if (gameThread.joinable())
    {
        gameThread.join();
    }

    if (m_debugUiInitialized && m_debugUiShutdownCallback)
    {
        m_debugUiShutdownCallback(*this, m_debugUiUserData);
        m_debugUiInitialized = false;
    }
    UnloadTexture(frameTex);
    CloseWindow();

    RUNTIME_LOG("[run] exiting loop");
}
