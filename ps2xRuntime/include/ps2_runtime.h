#ifndef PS2_RUNTIME_H
#define PS2_RUNTIME_H

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <functional>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif
#include <atomic>
#include <array>
#include <mutex>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

#include "ps2_log.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_pad.h"
#include "ps2x/iop/iop_types.h"

namespace ps2x::iop
{
    class IopSubsystem;
}

class PS2IopHostAdapter;
class PS2IopTransport;
class EeScheduler;
struct EeEvent;

extern "C" uint32_t ps2xGuestBumpAlloc(uint8_t *rdram, uint32_t size, uint32_t alignment);
extern "C" uint32_t ps2xGuestBumpAllocationSize(uint32_t address);

enum PS2Exception
{
    EXCEPTION_TLB_REFILL = 0x02,          // TLB refill/load exception
    EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,  // Address error on load
    EXCEPTION_ADDRESS_ERROR_STORE = 0x05, // Address error on store
    EXCEPTION_SYSCALL = 0x08,             // SYSCALL instruction
    EXCEPTION_BREAKPOINT = 0x09,          // BREAK instruction
    EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
    EXCEPTION_INTEGER_OVERFLOW = 0x0C, // From MIPS spec
    EXCEPTION_TRAP = 0x0D,             // Trap instruction condition met
};

// PS2 CPU context (R5900)
struct alignas(16) R5900Context
{
    // General Purpose Registers (128-bit)
    __m128i r[32]; // Main registers

    // Control registers
    uint32_t pc;         // Program counter
    uint64_t insn_count; // Instruction counter
    uint64_t hi, lo;     // HI/LO registers for mult/div results
    uint64_t hi1, lo1;   // Secondary HI/LO registers for MULT1/DIV1
    uint32_t sa;         // Shift amount register

    // VU0 registers (when used in macro mode)
    __m128 vu0_vf[32];        // VU0 vector float registers
    uint16_t vi[16];          // VU0 vector integer registers
    float vu0_q;              // VU0 Q register (quotient)
    float vu0_p;              // VU0 P register (EFU result)
    float vu0_i;              // VU0 I register (integer value)
    __m128 vu0_r;             // VU0 R register
    __m128 vu0_acc;           // VU0 ACC accumulator register
    uint16_t vu0_status;      // VU0 status register
    uint32_t vu0_mac_flags;   // VU0 MAC flags
    uint32_t vu0_clip_flags;  // VU0 clipping flags
    uint32_t vu0_clip_flags2; // VU0 clipping flags
    uint32_t vu0_cmsar0;      // VU0 microprogram start address
    uint32_t vu0_cmsar1;      // VU0 microprogram start address
    uint32_t vu0_cmsar2;      // VU0 microprogram start address
    uint32_t vu0_cmsar3;      // VU0 microprogram start address
    uint32_t vu0_vpu_stat;
    uint32_t vu0_vpu_stat2; // extra VPU status (used by CR_VPU_STAT2)
    uint32_t vu0_vpu_stat3; // extra VPU status 3
    uint32_t vu0_vpu_stat4; // extra VPU status 4
    uint32_t vu0_tpc;       // TPC (VU0 PC)
    uint32_t vu0_tpc2;      // second TPC
    uint32_t vu0_fbrst;     // VIF/VU reset register
    uint32_t vu0_fbrst2;    // FBRST2
    uint32_t vu0_fbrst3;    // FBRST3
    uint32_t vu0_fbrst4;    // FBRST4
    uint32_t vu0_itop;
    uint32_t vu0_top;
    uint32_t vu0_info;
    uint32_t vu0_xitop; // VU0 XITOP - input ITOP for VIF/VU sync
    uint32_t vu0_pc;

    float vu0_cf[4]; // VU0 FMAC control floating-point registers

    // COP0 System control registers
    uint32_t cop0_index;
    uint32_t cop0_random;
    uint32_t cop0_entrylo0;
    uint32_t cop0_entrylo1;
    uint32_t cop0_context;
    uint32_t cop0_pagemask;
    uint32_t cop0_wired;
    uint32_t cop0_badvaddr;
    uint32_t cop0_count;
    uint32_t cop0_entryhi;
    uint32_t cop0_compare;
    uint32_t cop0_status;
    uint32_t cop0_cause;
    uint32_t cop0_epc;
    uint32_t cop0_prid;
    uint32_t cop0_config;
    uint32_t cop0_badpaddr;
    uint32_t cop0_debug;
    uint32_t cop0_perf;
    uint32_t cop0_taglo;
    uint32_t cop0_taghi;
    uint32_t cop0_errorepc;

    // LL/SC reservation state (not part of COP0 Status bits).
    uint32_t llbit;
    uint32_t lladdr;

    // Delay slot state tracking
    bool in_delay_slot;
    uint32_t branch_pc;

    // COP2 control registers (VU0 integer + control)
    uint32_t cop2_ccr[32];

    // FPU registers (COP1)
    float f[32];
    float f_acc;    // FPU accumulator
    uint32_t fcr31; // Control/status register

    R5900Context()
    {
        std::memset(this, 0, sizeof(*this));

        // Initialize VU0 registers
        vu0_q = 1.0f; // Q register usually initialized to 1.0

        // Reset COP0 registers
        cop0_random = 47; // Start at maximum value
        // Status as the EE kernel leaves it when it hands control to the game,
        // which is the state recompiled code starts in -- we never execute the
        // boot ROM that would otherwise set this up.
        //
        // Both interrupt-enable bits matter, and they are not the same bit:
        //
        //   IE  (bit 0)     the architectural MIPS interrupt enable. The kernel
        //                   sets it once during boot and it normally stays set.
        //   EIE (bit 16)    the EE-specific enable that the `ei` and `di`
        //                   instructions toggle.
        //
        // Interrupts are only really on when both are set, and guest code reads
        // them separately. libkernel's StartThread, for instance, opens with
        // `mfc0 Status; xori 1; andi 1` and refuses to run when IE is clear --
        // that is its "you must call iStartThread from an interrupt handler"
        // guard. Leaving Status at zero made that guard fire forever, so every
        // StartThread returned -1 and any game that creates a thread stalled
        // with no diagnostic.
        //
        // EIE matters for the matching reason: DIntr reports whether it was set
        // so the caller knows whether to pair it with an EIntr. Starting at zero
        // makes DIntr always answer "already disabled" and the re-enable never
        // happens.
        //
        // BEV (0x00400000) is deliberately not set: that selects the boot
        // exception vectors, which is the pre-handoff state, not this one.
        cop0_status = 0x00010001; // EIE | IE
        cop0_prid = 0x00002e20; // CPU ID for R5900

        in_delay_slot = false;
        branch_pc = 0;
    }

    void dump() const
    {
        std::ios_base::fmtflags flags = std::cout.flags();
        std::cout << std::hex << std::setfill('0');
        std::cout << "--- R5900 Context Dump ---\n";
        std::cout << "PC: 0x" << std::setw(8) << pc << "\n";
        std::cout << "HI: 0x" << std::setw(8) << hi << " LO: 0x" << std::setw(8) << lo << "\n";
        std::cout << "HI1:0x" << std::setw(8) << hi1 << " LO1:0x" << std::setw(8) << lo1 << "\n";
        std::cout << "SA: 0x" << std::setw(8) << sa << "\n";
        for (int i = 0; i < 32; ++i)
        {
            std::cout << "R" << std::setw(2) << std::dec << i << ": 0x" << std::hex
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 3))
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 2)) << "_"
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 1))
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 0)) << "\n";
        }
        std::cout << "Status: 0x" << std::setw(8) << cop0_status
                  << " Cause: 0x" << std::setw(8) << cop0_cause
                  << " EPC: 0x" << std::setw(8) << cop0_epc << "\n";
        std::cout << "--- End Context Dump ---\n";
        std::cout.flags(flags); // Restore format flags
    }

    ~R5900Context() = default;
};

inline uint32_t getRegU32(const R5900Context *ctx, int reg)
{
    // Check if reg is valid (0-31)
    if (reg < 0 || reg > 31)
        return 0;
    if (reg == 0)
        return 0;
    return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[reg], 0));
}

inline void setReturnU32(R5900Context *ctx, uint32_t value)
{
    // R5900 sign-extends 32-bit results into 64-bit GPR, even for unsigned values.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value))); // $v0
}

inline void setReturnS32(R5900Context *ctx, int32_t value)
{
    // Signed 32-bit return should be sign-extended when observed as 64-bit.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value)); // $v0
}

inline void setReturnU64(R5900Context *ctx, uint64_t value)
{
    // Keep both conventions: full 64-bit value in $v0 and high 32-bit in $v1.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    ctx->r[3] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<uint32_t>(value >> 32)));
}

inline constexpr uint32_t PS2_PATH_WATCH_ADDR = 0x01EFFFA0u;
inline constexpr uint32_t PS2_PATH_WATCH_BYTES = 0x200u;

inline uint32_t ps2PathWatchPhysAddr()
{
    return PS2_PATH_WATCH_ADDR & PS2_RAM_MASK;
}

inline uint8_t ps2PathWatchExtractByteFromWrite(uint32_t writeAddr, uint32_t watchAddr, uint64_t valueLo, uint64_t valueHi)
{
    const uint32_t byteIndex = watchAddr - writeAddr;
    if (byteIndex < 8u)
    {
        return static_cast<uint8_t>((valueLo >> (byteIndex * 8u)) & 0xFFu);
    }
    return static_cast<uint8_t>((valueHi >> ((byteIndex - 8u) * 8u)) & 0xFFu);
}

inline uint32_t ps2TraceGuestRegisterLo32(const R5900Context *ctx, uint32_t reg)
{
    if (!ctx || reg >= 32u)
    {
        return 0u;
    }
    return static_cast<uint32_t>(_mm_cvtsi128_si64(ctx->r[reg]));
}

inline void ps2TraceGuestRead(uint32_t guestAddr,
                              uint32_t size,
                              uint64_t valueLo,
                              uint64_t valueHi,
                              const char *op,
                              const R5900Context *ctx)
{
    static const uint32_t firstAddress = []()
    {
        const char *value = std::getenv("PS2X_TRACE_GUEST_READ_ADDRESS_FIRST");
        if (!value || value[0] == '\0')
            return UINT32_MAX;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        return end && end != value && *end == '\0' && parsed <= UINT32_MAX
            ? static_cast<uint32_t>(parsed) & PS2_RAM_MASK
            : UINT32_MAX;
    }();
    if (firstAddress == UINT32_MAX || size == 0u)
        return;

    static const uint32_t lastAddress = []()
    {
        const char *value = std::getenv("PS2X_TRACE_GUEST_READ_ADDRESS_LAST");
        if (!value || value[0] == '\0')
            return UINT32_MAX;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        return end && end != value && *end == '\0' && parsed <= UINT32_MAX
            ? static_cast<uint32_t>(parsed) & PS2_RAM_MASK
            : UINT32_MAX;
    }();
    static const uint32_t maxTraces = []()
    {
        const char *value = std::getenv("PS2X_TRACE_GUEST_READ_MAX_TRACES");
        if (!value || value[0] == '\0')
            return 256u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        return end && end != value && *end == '\0' && parsed > 0u &&
                parsed <= UINT32_MAX
            ? static_cast<uint32_t>(parsed)
            : 256u;
    }();

    const uint32_t readStart = guestAddr & PS2_RAM_MASK;
    const uint32_t readLast = readStart + size - 1u;
    const uint32_t rangeLast = lastAddress != UINT32_MAX ? lastAddress : firstAddress;
    if (readLast < firstAddress || readStart > rangeLast)
        return;

    static std::atomic<uint32_t> traceCount{0u};
    const uint32_t index = traceCount.fetch_add(1u, std::memory_order_relaxed);
    if (index >= maxTraces)
        return;

    std::cerr << "[guest-read-watch] index=" << std::dec << index
              << " op=" << op
              << " addr=0x" << std::hex << guestAddr
              << " physical=0x" << readStart
              << " size=0x" << size
              << " lo=0x" << valueLo
              << " hi=0x" << valueHi
              << " pc=0x" << (ctx ? ctx->pc : 0u)
              << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
              << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
              << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
              << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
              << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
              << " a3=0x" << ps2TraceGuestRegisterLo32(ctx, 7)
              << std::dec << std::endl;
}

inline void ps2TraceGuestWrite(uint8_t *rdram,
                               uint32_t guestAddr,
                               uint32_t size,
                               uint64_t valueLo,
                               uint64_t valueHi,
                               const char *op,
                               const R5900Context *ctx)
{
    static const bool enabled = std::getenv("PS2X_XMEN_MEMORY_WATCHES") != nullptr;
    static const bool movieStateEnabled =
        std::getenv("PS2X_XMEN_MOVIE_STATE_WATCH") != nullptr;
    static const bool mediaRecordEnabled =
        std::getenv("PS2X_XMEN_MEDIA_RECORD_WATCH") != nullptr;
    static const bool movieStreamEnabled =
        std::getenv("PS2X_XMEN_MOVIE_STREAM_WATCH") != nullptr;
    if (!enabled && !movieStateEnabled && !mediaRecordEnabled && !movieStreamEnabled)
    {
        return;
    }

    if (movieStateEnabled)
    {
        constexpr uint32_t movieStateStart = 0x0146C644u;
        constexpr uint32_t movieStateEnd = 0x0146C654u;
        const uint32_t writeStart = guestAddr & PS2_RAM_MASK;
        const uint64_t writeEnd = static_cast<uint64_t>(writeStart) + size;
        if (writeStart < movieStateEnd && writeEnd > movieStateStart)
        {
            static std::atomic<uint32_t> movieStateLogCount{0u};
            const uint32_t index =
                movieStateLogCount.fetch_add(1u, std::memory_order_relaxed);
            if (index < 256u)
            {
                std::cerr << "[xmen-movie-manager-state-write] index=" << std::dec << index
                          << " op=" << op
                          << " addr=0x" << std::hex << writeStart
                          << " size=0x" << size
                          << " previous44=0x" << *reinterpret_cast<const uint32_t *>(rdram + 0x0146C644u)
                          << " previous48=0x" << *reinterpret_cast<const uint32_t *>(rdram + 0x0146C648u)
                          << " previous4c=0x" << *reinterpret_cast<const uint32_t *>(rdram + 0x0146C64Cu)
                          << " previous50=0x" << *reinterpret_cast<const uint32_t *>(rdram + 0x0146C650u)
                          << " lo=0x" << valueLo
                          << " hi=0x" << valueHi
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                          << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                          << " s0=0x" << ps2TraceGuestRegisterLo32(ctx, 16)
                          << " s1=0x" << ps2TraceGuestRegisterLo32(ctx, 17)
                          << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
                          << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
                          << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
                          << " a3=0x" << ps2TraceGuestRegisterLo32(ctx, 7)
                          << std::dec << std::endl;
            }
        }
    }
    if (mediaRecordEnabled)
    {
        constexpr uint32_t owner = 0x0146C600u;
        constexpr uint32_t recordStart = owner + 0x1278u;
        constexpr uint32_t recordStride = 116u;
        constexpr uint32_t recordCount = 8u;
        const uint32_t writeStart = guestAddr & PS2_RAM_MASK;
        const uint64_t writeEnd = static_cast<uint64_t>(writeStart) + size;
        for (uint32_t record = 0u; record < recordCount; ++record)
        {
            for (uint32_t field : {8u, 0xCu})
            {
                const uint32_t watchAddress = recordStart + record * recordStride + field;
                if (writeStart > watchAddress || writeEnd <= watchAddress)
                {
                    continue;
                }

                static std::atomic<uint32_t> mediaRecordLogCount{0u};
                const uint32_t index =
                    mediaRecordLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (index < 512u)
                {
                    uint32_t previous = 0u;
                    std::memcpy(&previous, rdram + watchAddress, sizeof(previous));
                    const uint32_t byteIndex = watchAddress - writeStart;
                    uint64_t source = byteIndex < 8u ? valueLo : valueHi;
                    const uint32_t shift = (byteIndex & 7u) * 8u;
                    std::cerr << "[xmen-media-record-write] index=" << std::dec << index
                              << " record=" << record
                              << " field=0x" << std::hex << field
                              << " op=" << op
                              << " addr=0x" << writeStart
                              << " size=0x" << size
                              << " previous=0x" << previous
                              << " value=0x" << static_cast<uint32_t>(source >> shift)
                              << " pc=0x" << (ctx ? ctx->pc : 0u)
                              << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                              << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
                              << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
                              << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
                              << " s0=0x" << ps2TraceGuestRegisterLo32(ctx, 16)
                              << " s1=0x" << ps2TraceGuestRegisterLo32(ctx, 17)
                              << std::dec << std::endl;
                }
            }
        }
    }
    if (movieStreamEnabled)
    {
        constexpr uint32_t owner = 0x0146C600u;
        uint32_t streamHolder = 0u;
        std::memcpy(&streamHolder, rdram + owner + 0x1CACu, sizeof(streamHolder));
        streamHolder &= PS2_RAM_MASK;

        uint32_t stream = 0u;
        if (streamHolder != 0u)
        {
            std::memcpy(&stream, rdram + streamHolder, sizeof(stream));
            stream &= PS2_RAM_MASK;
        }

        if (stream != 0u)
        {
            const uint32_t writeStart = guestAddr & PS2_RAM_MASK;
            const uint64_t writeEnd = static_cast<uint64_t>(writeStart) + size;
            for (uint32_t field : {1u, 0x60u})
            {
                const uint32_t watchAddress = stream + field;
                if (writeStart > watchAddress || writeEnd <= watchAddress)
                {
                    continue;
                }

                static std::atomic<uint32_t> movieStreamLogCount{0u};
                const uint32_t index =
                    movieStreamLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (index < 256u)
                {
                    uint16_t previous = 0u;
                    std::memcpy(&previous, rdram + watchAddress,
                                field == 1u ? sizeof(uint8_t) : sizeof(previous));
                    const uint32_t byteIndex = watchAddress - writeStart;
                    const uint64_t source = byteIndex < 8u ? valueLo : valueHi;
                    const uint32_t shift = (byteIndex & 7u) * 8u;
                    std::cerr << "[xmen-movie-stream-write] index=" << std::dec << index
                              << " field=0x" << std::hex << field
                              << " op=" << op
                              << " addr=0x" << writeStart
                              << " size=0x" << size
                              << " previous=0x" << previous
                              << " value=0x" << static_cast<uint32_t>(source >> shift)
                              << " pc=0x" << (ctx ? ctx->pc : 0u)
                              << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                              << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
                              << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
                              << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
                              << " s0=0x" << ps2TraceGuestRegisterLo32(ctx, 16)
                              << " s1=0x" << ps2TraceGuestRegisterLo32(ctx, 17)
                              << std::dec << std::endl;
                }
            }
        }
    }
    if (!enabled)
    {
        return;
    }

    const uint32_t textureHeapWriteStart = guestAddr & PS2_RAM_MASK;
    const uint32_t textureHeapWordCount = std::min(size / 4u, 4u);
    for (uint32_t word = 0u; word < textureHeapWordCount; ++word)
    {
        const uint64_t source = word < 2u ? valueLo : valueHi;
        const uint32_t value = static_cast<uint32_t>(source >> ((word & 1u) * 32u));
        const uint32_t address = textureHeapWriteStart + word * sizeof(uint32_t);
        const bool touchesKnownOverlap =
            (address >= 0x00A3BEC0u && address < 0x00A3BF60u) ||
            (address >= 0x00A3C8C0u && address < 0x00A3C960u);
        if (value != 0xFFFFFFFFu || !touchesKnownOverlap)
        {
            continue;
        }

        uint32_t previous = 0u;
        std::memcpy(&previous, rdram + address, sizeof(previous));
        if (previous == 0xFFFFFFFFu)
        {
            continue;
        }

        static std::atomic<uint32_t> xmenTextureMinusOneWriteLogCount{0u};
        const uint32_t index =
            xmenTextureMinusOneWriteLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (index >= 256u)
        {
            continue;
        }

        const uint32_t possibleObject = address >= 0x18u ? address - 0x18u : 0u;
        uint32_t possiblePreviousLink = 0u;
        uint32_t possibleNextLink = 0u;
        uint32_t possibleOffset = 0u;
        uint16_t possibleExtent = 0u;
        uint8_t possibleType = 0u;
        uint8_t possibleState = 0u;
        if (possibleObject + 0x49u <= PS2_RAM_SIZE)
        {
            std::memcpy(&possiblePreviousLink, rdram + possibleObject + 0x1Cu,
                        sizeof(possiblePreviousLink));
            std::memcpy(&possibleNextLink, rdram + possibleObject + 0x18u,
                        sizeof(possibleNextLink));
            std::memcpy(&possibleOffset, rdram + possibleObject + 0x38u,
                        sizeof(possibleOffset));
            std::memcpy(&possibleExtent, rdram + possibleObject + 0x40u,
                        sizeof(possibleExtent));
            std::memcpy(&possibleType, rdram + possibleObject + 0x46u,
                        sizeof(possibleType));
            std::memcpy(&possibleState, rdram + possibleObject + 0x48u,
                        sizeof(possibleState));
        }
        std::cerr << "[xmen-texture-heap:minus-one-write] index=" << std::dec << index
                  << " op=" << op
                  << " addr=0x" << std::hex << address
                  << " previous=0x" << previous
                  << " possibleObject=0x" << possibleObject
                  << " objectNext=0x" << possibleNextLink
                  << " objectPrevious=0x" << possiblePreviousLink
                  << " objectOffset=0x" << possibleOffset
                  << " objectExtent=0x" << possibleExtent
                  << " objectType=0x" << static_cast<uint32_t>(possibleType)
                  << " objectState=0x" << static_cast<uint32_t>(possibleState)
                  << " pc=0x" << (ctx ? ctx->pc : 0u)
                  << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                  << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                  << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
                  << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
                  << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
                  << " a3=0x" << ps2TraceGuestRegisterLo32(ctx, 7)
                  << std::dec << std::endl;
    }

    constexpr uint32_t xmenChainSavedReturnSlot = 0x01F12560u;
    const uint32_t focusedWriteStart = guestAddr & PS2_RAM_MASK;
    const uint64_t focusedWriteEnd = static_cast<uint64_t>(focusedWriteStart) + size;
    const bool touchesXmenChainSavedReturn =
        focusedWriteStart < xmenChainSavedReturnSlot + sizeof(uint64_t) &&
        focusedWriteEnd > xmenChainSavedReturnSlot;
    static std::atomic<bool> xmenChainSavedReturnArmed{false};
    if (touchesXmenChainSavedReturn && ctx && ctx->pc == 0x002E6C64u)
    {
        xmenChainSavedReturnArmed.store(true, std::memory_order_release);
    }
    if (touchesXmenChainSavedReturn &&
        xmenChainSavedReturnArmed.load(std::memory_order_acquire))
    {
        static std::atomic<uint32_t> xmenChainSavedReturnLogCount{0u};
        const uint32_t index =
            xmenChainSavedReturnLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (index < 2048u)
        {
            uint64_t previous = 0u;
            std::memcpy(&previous,
                        rdram + xmenChainSavedReturnSlot,
                        sizeof(previous));
            std::cerr << "[xmen-chain-slot-write] index=" << std::dec << index
                      << " op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " previous=0x" << previous
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << ctx->pc
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << std::dec << std::endl;
        }
    }

    const uint32_t writeWords = std::min(size / 4u, 4u);
    static const uint32_t targetWriteValue = []()
    {
        const char *value = std::getenv("PS2X_TRACE_GUEST_WRITE_VALUE");
        if (!value || value[0] == '\0')
            return UINT32_MAX;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 0);
        return end && end != value && *end == '\0' && parsed <= UINT32_MAX
            ? static_cast<uint32_t>(parsed)
            : UINT32_MAX;
    }();
    bool containsTargetWriteValue = false;
    for (uint32_t word = 0u; word < writeWords; ++word)
    {
        const uint64_t source = word < 2u ? valueLo : valueHi;
        const uint32_t shift = (word & 1u) * 32u;
        containsTargetWriteValue |=
            static_cast<uint32_t>(source >> shift) == targetWriteValue;
    }
    if (targetWriteValue != UINT32_MAX && containsTargetWriteValue)
    {
        static std::atomic<uint32_t> targetWriteValueLogCount{0u};
        const uint32_t index =
            targetWriteValueLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (index < 256u)
        {
            std::cerr << "[guest-write-value] index=" << std::dec << index
                      << " value=0x" << std::hex << targetWriteValue
                      << " op=" << op
                      << " addr=0x" << guestAddr
                      << " size=0x" << size
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
                      << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
                      << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
                      << " a3=0x" << ps2TraceGuestRegisterLo32(ctx, 7)
                      << " s0=0x" << ps2TraceGuestRegisterLo32(ctx, 16)
                      << " s1=0x" << ps2TraceGuestRegisterLo32(ctx, 17)
                      << std::dec << std::endl;
        }
    }

    constexpr uint32_t xmenBadRegistryValue = 0x0028F6B0u;
    bool containsBadRegistryValue = false;
    for (uint32_t word = 0u; word < writeWords; ++word)
    {
        const uint64_t source = word < 2u ? valueLo : valueHi;
        const uint32_t shift = (word & 1u) * 32u;
        containsBadRegistryValue |= static_cast<uint32_t>(source >> shift) == xmenBadRegistryValue;
    }
    if (containsBadRegistryValue)
    {
        static std::atomic<uint32_t> badRegistryValueLogCount{0};
        if (badRegistryValueLogCount.fetch_add(1, std::memory_order_relaxed) < 128u)
        {
            std::cerr << "[xmen-watch-value:28f6b0] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    static std::atomic<uint32_t> watchLogCount{0};
    constexpr uint32_t watchStartA = 0x00898EF0u;
    constexpr uint32_t watchEndA = 0x00898F00u;
    constexpr uint32_t watchStartB = 0x00741E20u;
    constexpr uint32_t watchEndB = 0x00741E34u;
    constexpr uint32_t watchStartC = 0x006F2B50u;
    constexpr uint32_t watchEndC = 0x006F2B80u;
    constexpr uint32_t watchStartD = 0x00747250u;
    constexpr uint32_t watchEndD = 0x007472B0u;
    constexpr uint32_t watchStartE = 0x00745C78u;
    constexpr uint32_t watchEndE = 0x00745C7Cu;
    constexpr uint32_t watchStartF = 0x00900410u;
    constexpr uint32_t watchEndF = 0x00900430u;
    constexpr uint32_t watchStartG = 0x00897460u;
    constexpr uint32_t watchEndG = 0x00898EE4u;
    constexpr uint32_t watchStartH = 0x00746FF0u;
    constexpr uint32_t watchEndH = 0x00746FF4u;
    constexpr uint32_t watchStartI = 0x00B1A870u;
    constexpr uint32_t watchEndI = 0x00B1AAA0u;
    constexpr uint32_t watchStartJ = 0x0089B000u;
    constexpr uint32_t watchEndJ = 0x0089B010u;
    static const uint32_t watchStartK = [] {
        const char* value = std::getenv("PS2X_WATCH_START_K");
        return value && *value
            ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) & PS2_RAM_MASK
            : 0x00720A4Cu;
    }();
    static const uint32_t watchSizeK = [] {
        const char* value = std::getenv("PS2X_WATCH_SIZE_K");
        return value && *value
            ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
            : 0x40u;
    }();
    static const uint32_t watchPcStartK = [] {
        const char* value = std::getenv("PS2X_WATCH_PC_START_K");
        return value && *value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) : 0u;
    }();
    static const uint32_t watchPcEndK = [] {
        const char* value = std::getenv("PS2X_WATCH_PC_END_K");
        return value && *value
            ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
            : 0xFFFFFFFFu;
    }();
    const uint64_t watchEndK = static_cast<uint64_t>(watchStartK) + watchSizeK;
    const uint32_t writeEnd = guestAddr + size;
    const bool touchesA = guestAddr < watchEndA && writeEnd > watchStartA;
    const bool touchesB = guestAddr < watchEndB && writeEnd > watchStartB;
    const bool touchesC = guestAddr < watchEndC && writeEnd > watchStartC;
    const bool touchesD = guestAddr < watchEndD && writeEnd > watchStartD;
    const bool touchesE = guestAddr < watchEndE && writeEnd > watchStartE;
    const bool touchesF = guestAddr < watchEndF && writeEnd > watchStartF;
    const bool touchesG = guestAddr < watchEndG && writeEnd > watchStartG;
    const bool touchesH = guestAddr < watchEndH && writeEnd > watchStartH;
    const bool touchesI = guestAddr < watchEndI && writeEnd > watchStartI;
    const bool touchesJ = guestAddr < watchEndJ && writeEnd > watchStartJ;
    const uint32_t guestPhys = guestAddr & PS2_RAM_MASK;
    const uint64_t writePhysEnd = static_cast<uint64_t>(guestPhys) + size;
    const bool touchesK = guestPhys < watchEndK && writePhysEnd > watchStartK;
    const uint32_t writePc = ctx ? ctx->pc : 0u;
    const bool watchPcMatchesK =
        watchPcStartK == 0u || (ctx && writePc >= watchPcStartK && writePc < watchPcEndK);
    if (touchesK && watchPcMatchesK)
    {
        static std::atomic<uint32_t> legalMatrixPacketWatchLogCount{0};
        if (legalMatrixPacketWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 256u)
        {
            std::cerr << "[xmen-watch:dynamic-k] range=0x" << std::hex << watchStartK
                      << "+0x" << watchSizeK
                      << " op=" << op
                      << " addr=0x" << guestAddr
                      << " size=0x" << size
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << " s0=0x" << ps2TraceGuestRegisterLo32(ctx, 16)
                      << " s1=0x" << ps2TraceGuestRegisterLo32(ctx, 17)
                      << " a0=0x" << ps2TraceGuestRegisterLo32(ctx, 4)
                      << " a1=0x" << ps2TraceGuestRegisterLo32(ctx, 5)
                      << " a2=0x" << ps2TraceGuestRegisterLo32(ctx, 6)
                      << " a3=0x" << ps2TraceGuestRegisterLo32(ctx, 7)
                      << std::dec << std::endl;
        }
    }
    if (touchesJ)
    {
        static std::atomic<uint32_t> renderBufferWatchLogCount{0};
        if (renderBufferWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 4096u)
        {
            std::cerr << "[xmen-watch:89b000] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (touchesI)
    {
        static std::atomic<uint32_t> objectWatchLogCount{0};
        if (objectWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 2048u)
        {
            std::cerr << "[xmen-watch:b1a870] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (touchesH)
    {
        static std::atomic<uint32_t> factoryWatchLogCount{0};
        if (factoryWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 256u)
        {
            std::cerr << "[xmen-watch:746ff0] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " lo=0x" << valueLo
                      << " hi=0x" << valueHi
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if ((touchesA || touchesB || touchesC || touchesD || touchesE || touchesF || touchesG) && watchLogCount.fetch_add(1, std::memory_order_relaxed) < 8192u)
    {
        const char *tag = touchesG ? "[xmen-watch:897460] op="
                          : touchesF ? "[xmen-watch:900410] op="
                          : touchesE ? "[xmen-watch:745c78] op="
                          : touchesD ? "[xmen-watch:747250] op="
                          : touchesC ? "[xmen-watch:6f2b50] op="
                          : touchesB ? "[xmen-watch:741e20] op="
                                     : "[xmen-watch:898ef0] op=";
        std::cerr << tag << op
                  << " addr=0x" << std::hex << guestAddr
                  << " size=0x" << size
                  << " lo=0x" << valueLo
                  << " hi=0x" << valueHi
                  << " pc=0x" << (ctx ? ctx->pc : 0u)
                  << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                  << std::dec << std::endl;
    }
    // TODO we dont need this anymore so on next release it will be deleted
}

inline void ps2TraceGuestRangeWrite(uint8_t *rdram,
                                    uint32_t guestAddr,
                                    uint32_t size,
                                    const char *op,
                                    const R5900Context *ctx)
{
    static const bool enabled = std::getenv("PS2X_XMEN_MEMORY_WATCHES") != nullptr;
    if (!enabled)
    {
        return;
    }

    static std::atomic<uint32_t> rangeWatchLogCount{0};
    constexpr uint32_t watchStartD = 0x00747250u;
    constexpr uint32_t watchEndD = 0x007472B0u;
    constexpr uint32_t watchStartE = 0x00745C78u;
    constexpr uint32_t watchEndE = 0x00745C7Cu;
    constexpr uint32_t watchStartF = 0x00900410u;
    constexpr uint32_t watchEndF = 0x00900430u;
    constexpr uint32_t watchStartG = 0x00897460u;
    constexpr uint32_t watchEndG = 0x00898EE4u;
    constexpr uint32_t watchStartH = 0x00746FF0u;
    constexpr uint32_t watchEndH = 0x00746FF4u;
    constexpr uint32_t watchStartI = 0x00B1A870u;
    constexpr uint32_t watchEndI = 0x00B1AAA0u;
    constexpr uint32_t watchStartJ = 0x0089B000u;
    constexpr uint32_t watchEndJ = 0x0089B010u;
    static const uint32_t watchStartK = [] {
        const char* value = std::getenv("PS2X_WATCH_START_K");
        return value && *value
            ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0)) & PS2_RAM_MASK
            : 0x00720A4Cu;
    }();
    static const uint32_t watchSizeK = [] {
        const char* value = std::getenv("PS2X_WATCH_SIZE_K");
        return value && *value
            ? static_cast<uint32_t>(std::strtoul(value, nullptr, 0))
            : 0x40u;
    }();
    const uint64_t watchEndK = static_cast<uint64_t>(watchStartK) + watchSizeK;
    const uint64_t writeEnd = static_cast<uint64_t>(guestAddr) + static_cast<uint64_t>(size);
    const bool touchesD = guestAddr < watchEndD && writeEnd > watchStartD;
    const bool touchesE = guestAddr < watchEndE && writeEnd > watchStartE;
    const bool touchesF = guestAddr < watchEndF && writeEnd > watchStartF;
    const bool touchesG = guestAddr < watchEndG && writeEnd > watchStartG;
    const bool touchesH = guestAddr < watchEndH && writeEnd > watchStartH;
    const bool touchesI = guestAddr < watchEndI && writeEnd > watchStartI;
    const bool touchesJ = guestAddr < watchEndJ && writeEnd > watchStartJ;
    const uint32_t guestPhys = guestAddr & PS2_RAM_MASK;
    const uint64_t writePhysEnd = static_cast<uint64_t>(guestPhys) + size;
    const bool touchesK = guestPhys < watchEndK && writePhysEnd > watchStartK;
    if (touchesK)
    {
        static std::atomic<uint32_t> dynamicRangeWatchLogCount{0};
        if (dynamicRangeWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 256u)
        {
            const uint32_t sampleAddr = std::max(guestPhys, watchStartK);
            uint32_t sample = 0u;
            if (rdram && sampleAddr + sizeof(sample) <= PS2_RAM_SIZE)
            {
                std::memcpy(&sample, rdram + sampleAddr, sizeof(sample));
            }
            std::cerr << "[xmen-range-watch:dynamic-k] range=0x" << std::hex << watchStartK
                      << "+0x" << watchSizeK
                      << " op=" << op
                      << " addr=0x" << guestAddr
                      << " size=0x" << size
                      << " sample-addr=0x" << sampleAddr
                      << " sample=0x" << sample
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (touchesJ)
    {
        static std::atomic<uint32_t> renderBufferRangeWatchLogCount{0};
        if (renderBufferRangeWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 512u)
        {
            uint32_t sample = 0u;
            if (rdram && watchStartJ + sizeof(sample) <= PS2_RAM_SIZE)
            {
                std::memcpy(&sample, rdram + watchStartJ, sizeof(sample));
            }
            std::cerr << "[xmen-range-watch:89b000] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " sample=0x" << sample
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << " sp=0x" << ps2TraceGuestRegisterLo32(ctx, 29)
                      << std::dec << std::endl;
        }
    }
    if (touchesI)
    {
        static std::atomic<uint32_t> objectRangeWatchLogCount{0};
        if (objectRangeWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 256u)
        {
            uint32_t sample = 0u;
            if (rdram && watchStartI + sizeof(sample) <= PS2_RAM_SIZE)
            {
                std::memcpy(&sample, rdram + watchStartI, sizeof(sample));
            }
            std::cerr << "[xmen-range-watch:b1a870] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " sample=0x" << sample
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if (touchesH)
    {
        static std::atomic<uint32_t> factoryRangeWatchLogCount{0};
        if (factoryRangeWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 256u)
        {
            uint32_t sample = 0u;
            if (rdram && watchStartH + sizeof(sample) <= PS2_RAM_SIZE)
            {
                std::memcpy(&sample, rdram + watchStartH, sizeof(sample));
            }
            std::cerr << "[xmen-range-watch:746ff0] op=" << op
                      << " addr=0x" << std::hex << guestAddr
                      << " size=0x" << size
                      << " sample=0x" << sample
                      << " pc=0x" << (ctx ? ctx->pc : 0u)
                      << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                      << std::dec << std::endl;
        }
    }
    if ((touchesD || touchesE || touchesF || touchesG) && rangeWatchLogCount.fetch_add(1, std::memory_order_relaxed) < 1024u)
    {
        const uint32_t sampleAddr = touchesG ? watchStartG : touchesF ? watchStartF : touchesE ? watchStartE : watchStartD;
        uint32_t sample = 0u;
        if (rdram && sampleAddr + sizeof(sample) <= PS2_RAM_SIZE)
        {
            std::memcpy(&sample, rdram + sampleAddr, sizeof(sample));
        }
        std::cerr << (touchesG ? "[xmen-range-watch:897460] op=" : touchesF ? "[xmen-range-watch:900410] op=" : touchesE ? "[xmen-range-watch:745c78] op=" : "[xmen-range-watch:747250] op=")
                  << op
                  << " addr=0x" << std::hex << guestAddr
                  << " size=0x" << size
                  << " sample=0x" << sample
                  << " pc=0x" << (ctx ? ctx->pc : 0u)
                  << " ra=0x" << ps2TraceGuestRegisterLo32(ctx, 31)
                  << std::dec << std::endl;
    }
    // TODO we dont need this anymore so on next release it will be deleted
}

class PS2Runtime
{
public:
    struct IoPaths
    {
        std::filesystem::path elfPath;
        std::filesystem::path elfDirectory;
        std::filesystem::path hostRoot;
        std::filesystem::path cdRoot;
        std::filesystem::path mcRoot;
        std::filesystem::path cdImage;
    };

    PS2Runtime();
    ~PS2Runtime();

    bool initialize(const char *title = "PS2 Game");
    bool syncCoreSubsystems();
    bool loadELF(const std::string &elfPath);
    void run();

    void setIopPluginSearchPaths(std::vector<std::filesystem::path> paths);
    [[nodiscard]] ps2x::iop::DebugSnapshot iopDebugSnapshot() const;

    using DebugUiCallback = void (*)(PS2Runtime &runtime, void *userData);
    void setDebugUiCallbacks(DebugUiCallback initCallback,
                             DebugUiCallback drawCallback,
                             DebugUiCallback shutdownCallback,
                             void *userData);

    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    enum class GuestBranchKind
    {
        DirectJump,
        DirectCall,
        IndirectJump,
        IndirectCall,
        Return,
    };

    enum class MissingFunctionPolicy : uint32_t
    {
        // Strict mode for tests/CI: log the bad target and request the runtime to stop.
        Stop = 0,

        // Debug mode: log once, leave ctx->pc on the bad target, and let the caller unwind.
        ContinueToTarget = 1,

        // Debug mode: same as ContinueToTarget, but triggers a debugger break once on MSVC.
        BreakOnce = 2,

        // Escape hatch only: skip missing calls by returning to fallthrough (it can hide guest bugs)
        SkipCallDebug = 3,
    };

    bool replaceFunction(uint32_t address, RecompiledFunction func);
    // TODO remove this later need to update all tests
    bool registerFunction(uint32_t address, RecompiledFunction func);
    RecompiledFunction lookupFunction(uint32_t address);
    bool hasFunction(uint32_t address) const;
    bool dispatchGuestBranch(uint8_t *rdram,
                             R5900Context *ctx,
                             uint32_t targetPc,
                             uint32_t sourcePc,
                             uint32_t fallthroughPc,
                             GuestBranchKind kind,
                             const char *debugName);
    void dispatchGuestReturn(R5900Context *ctx, uint32_t targetPc) noexcept;
    void reportMissingFunction(uint8_t *rdram,
                               R5900Context *ctx,
                               uint32_t targetPc,
                               uint32_t sourcePc,
                               GuestBranchKind kind,
                               const char *debugName);
    void setMissingFunctionPolicy(MissingFunctionPolicy policy);
    MissingFunctionPolicy missingFunctionPolicy() const;
    void resetMissingFunctionReportOnce();

    static const IoPaths &getIoPaths();
    static void setIoPaths(const IoPaths &paths);
    static void configureIoPathsFromElf(const std::string &elfPath);

    void SignalException(R5900Context *ctx, PS2Exception exception);

    void executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address);

public:
    void handleSyscall(uint8_t *rdram, R5900Context *ctx);
    void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId);
    void handleBreak(uint8_t *rdram, R5900Context *ctx);

    void handleTrap(uint8_t *rdram, R5900Context *ctx);
    void handleTLBR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWI(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBP(uint8_t *rdram, R5900Context *ctx);
    void clearLLBit(R5900Context *ctx);
    void configureGuestHeap(uint32_t guestBase, uint32_t guestLimit = PS2_RAM_SIZE);
    uint32_t guestMalloc(uint32_t size, uint32_t alignment = 16u);
    uint32_t guestCalloc(uint32_t count, uint32_t size, uint32_t alignment = 16u);
    uint32_t guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment = 16u);
    uint32_t guestAllocationRemainingSize(uint32_t guestAddr) const;
    void guestFree(uint32_t guestAddr);
    uint32_t guestHeapBase() const;
    uint32_t guestHeapEnd() const;
    uint32_t guestHeapLimit() const;
    uint32_t reserveAsyncCallbackStack(uint32_t size, uint32_t alignment = 16u);

    void drainCompletedDmacHandlers(uint8_t *rdram);

    void requestStop();
    bool isStopRequested() const;

    EeScheduler &eeScheduler();
    const EeScheduler &eeScheduler() const;
    void postEeEvent(EeEvent event);
    bool eeCheckpointDue(uint32_t cycles = 32u) noexcept;
    [[noreturn]] void eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc);

    struct EeExitHandlerRegistration
    {
        uint32_t function = 0;
        uint32_t argument = 0;
    };
    void addEeExitHandler(int threadId, uint32_t function, uint32_t argument);
    std::vector<EeExitHandlerRegistration> takeEeExitHandlers(int threadId);
    void removeEeExitHandlers(int threadId);
    bool findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const;
    void setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler);
    void initializeEeKernelState(uint8_t *rdram);

    uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);

    void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value);
    void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value);
    void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value);
    void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value);
    void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value);
    void kickGifDmaChainFromMMIO(uint8_t *rdram,
                                 R5900Context *ctx,
                                 uint32_t dPcrValue,
                                 uint32_t dStatValue,
                                 uint32_t tadr,
                                 uint32_t chcr);

    static inline bool isSpecialAddress(uint32_t addr)
    {
        return Ps2IsSpecialAddress(addr);
    }

public:
    inline R5900Context &cpu() { return m_cpuContext; }
    inline const R5900Context &cpu() const { return m_cpuContext; }

    inline PS2Memory &memory() { return m_memory; }
    inline const PS2Memory &memory() const { return m_memory; }

    inline GS &gs() { return m_gs; }
    inline const GS &gs() const { return m_gs; }
    inline GifArbiter &gifArbiter() { return m_gifArbiter; }
    inline const GifArbiter &gifArbiter() const { return m_gifArbiter; }
    inline VU1Interpreter &vu0() { return m_vu0; }
    inline const VU1Interpreter &vu0() const { return m_vu0; }
    inline VU1Interpreter &vu1() { return m_vu1; }
    inline const VU1Interpreter &vu1() const { return m_vu1; }

    inline PS2AudioBackend &audioBackend() { return m_audioBackend; }
    inline const PS2AudioBackend &audioBackend() const { return m_audioBackend; }
    inline PSPadBackend &padBackend() { return m_padBackend; }
    inline const PSPadBackend &padBackend() const { return m_padBackend; }

private:
    struct GuestHeapBlock
    {
        uint32_t addr = 0;
        uint32_t size = 0;
        bool free = true;
    };

    static uint32_t alignGuestHeapValue(uint32_t value, uint32_t alignment);
    static bool isGuestHeapAlignmentValid(uint32_t alignment);
    static uint32_t normalizeGuestHeapAlignment(uint32_t alignment);
    uint32_t clampGuestHeapBase(uint32_t guestBase) const;
    uint32_t clampGuestHeapLimit(uint32_t guestLimit) const;
    void resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit);
    void ensureGuestHeapInitializedLocked();
    int32_t findGuestHeapBlockIndexLocked(uint32_t guestAddr) const;
    uint32_t allocateGuestBlockLocked(uint32_t size, uint32_t alignment);
    void freeGuestBlockLocked(uint32_t guestAddr);
    void coalesceGuestHeapLocked();
    void HandleIntegerOverflow(R5900Context *ctx);

    [[nodiscard]] ps2x::iop::RpcAbi selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const;
    [[nodiscard]] ps2x::iop::RpcResult handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request);
    void notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer);
    void resetIop();

    friend class PS2IopTransport;
    friend class EeScheduler;

private:
    PS2Memory m_memory;
    GifArbiter m_gifArbiter;
    GS m_gs;
    std::unique_ptr<PS2IopHostAdapter> m_iopHost;
    std::unique_ptr<ps2x::iop::IopSubsystem> m_iopSubsystem;
    PS2AudioBackend m_audioBackend;
    PSPadBackend m_padBackend;
    VU1Interpreter m_vu0{VU1Interpreter::Unit::VU0};
    VU1Interpreter m_vu1{VU1Interpreter::Unit::VU1};
    R5900Context m_cpuContext;
    std::unique_ptr<EeScheduler> m_eeScheduler;
    mutable std::mutex m_eeKernelStateMutex;
    std::unordered_map<int, std::vector<EeExitHandlerRegistration>> m_eeExitHandlers;
    std::unordered_map<uint32_t, uint32_t> m_eeSyscallOverrides;
    std::unordered_set<uint32_t> m_eeSyscallMirrorAddresses;
    mutable std::mutex m_guestHeapMutex;
    mutable std::mutex m_asyncCallbackStackMutex;
    std::vector<GuestHeapBlock> m_guestHeapBlocks;
    uint32_t m_guestHeapBase = 0x00100000u;
    uint32_t m_guestHeapEnd = 0x00100000u;
    uint32_t m_guestHeapLimit = PS2_RAM_SIZE;
    uint32_t m_guestHeapSuggestedBase = 0x00100000u;
    bool m_guestHeapConfigured = false;
    uint32_t m_asyncCallbackStackFloor = 0x01F00000u;
    uint32_t m_asyncCallbackStackTop = PS2_RAM_SIZE;

    std::atomic<uint32_t> m_missingFunctionPolicy{static_cast<uint32_t>(MissingFunctionPolicy::ContinueToTarget)};
    std::atomic<bool> m_missingFunctionReported{false};
    std::atomic<bool> m_stopRequested{false};
    DebugUiCallback m_debugUiInitCallback = nullptr;
    DebugUiCallback m_debugUiDrawCallback = nullptr;
    DebugUiCallback m_debugUiShutdownCallback = nullptr;
    void *m_debugUiUserData = nullptr;
    bool m_debugUiInitialized = false;

public:
    std::atomic<uint32_t> m_debugPc{0};
    std::atomic<uint32_t> m_debugRa{0};
    std::atomic<uint32_t> m_debugSp{0};
    std::atomic<uint32_t> m_debugGp{0};

private:
    struct LoadedModule
    {
        std::string name;
        uint32_t baseAddress;
        size_t size;
        bool active;
    };

    std::vector<LoadedModule> m_loadedModules;
    uint8_t *m_boundRdram = nullptr;
    uint8_t *m_boundGSVram = nullptr;
};

// Generated by ps2xRecomp in ps2xRuntime/src/runner/register_functions.cpp.
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];

#endif // PS2_RUNTIME_H
