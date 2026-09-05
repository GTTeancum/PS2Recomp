#pragma once

#include <atomic>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
#include "runtime/ps2_vu1.h"
#endif

class GS;
class PS2Memory;
class VU1Interpreter;

// Diagnostic VU1 slice replay, including pending pipelines but not host pointers.
class VUReplay
{
public:
    struct CaptureSchedule
    {
        uint32_t shortCases = 0u;
        uint32_t longCases = 0u;
        uint32_t random = 0x57C019ABu;
        uint64_t nextShortTick = 1100u;
        uint64_t nextLongTick = 1100u;

        bool select(uint64_t tick, uint32_t maxCycles);
        bool complete() const { return shortCases == 16u && longCases == 16u; }
    };

    struct CaseTiming
    {
        uint32_t pc = 0;
        uint32_t budget = 0;
        uint64_t cycles = 0;
        uint64_t executeNs = 0;
        uint32_t gifBytes = 0;
    };
    struct Result
    {
        uint32_t cases = 0;
        uint64_t iterations = 0;
        uint64_t cycles = 0;
        uint64_t executeNs = 0;
        uint64_t digest = 14695981039346656037ull;
        uint64_t nativeUpper = 0;
        uint64_t interpretedUpper = 0;
        uint64_t nativePairs = 0;
        uint64_t interpretedPairs = 0;
        std::string error;
        std::vector<CaseTiming> timings;
    };

    struct UpperSample
    {
        uint32_t instruction = 0;
        uint64_t fetches = 0;
    };

    struct PairSample
    {
        uint32_t pc = 0;
        uint32_t lower = 0;
        uint32_t upper = 0;
        uint64_t executions = 0;
        std::vector<uint32_t> successors;
        bool entry = false;
    };

    static bool captureRequested();
    static bool captureSlice(VU1Interpreter &vu, uint8_t *code, uint32_t codeSize,
                             uint8_t *data, uint32_t dataSize, GS &gs,
                             PS2Memory *memory, uint32_t maxCycles);
    static bool observeGif(const uint8_t *packet, uint32_t bytes, uint64_t cycle);

    // Executes the slice even if appending exceeds the file/output limit.
    static bool record(std::ostream &output, VU1Interpreter &vu,
                       uint8_t *code, uint8_t *data, GS &gs,
                       PS2Memory *memory, uint32_t maxCycles);
    // Optional one-cycle cold pass. Upper fetches include dependency stalls;
    // pair executions are counted only when the instruction retires.
    // Optional profiler flag covers warm execution only, not snapshot I/O or the cold pass.
    static Result replay(std::istream &input, uint32_t repeats,
                         std::vector<UpperSample> *upperSamples = nullptr,
                         std::vector<PairSample> *pairSamples = nullptr
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
                         , VU1Interpreter::UpperLookup upperLookup = nullptr
#endif
                         , std::atomic_bool *executing = nullptr
                         );
    static bool writeUpperKernels(std::ostream &output,
                                  const std::vector<UpperSample> &samples, uint32_t limit);
    static bool writePairKernels(std::ostream &output,
                                 const std::vector<PairSample> &samples, uint32_t limit);

private:
    template <class Archive> static void visitState(Archive &archive, VU1Interpreter &vu);
    static std::vector<uint8_t> saveState(VU1Interpreter &vu);
    static void loadState(const std::vector<uint8_t> &bytes, VU1Interpreter &vu);
};
