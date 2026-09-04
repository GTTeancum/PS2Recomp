#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

class GS;
class PS2Memory;
class VU1Interpreter;

// Diagnostic VU1 slice replay, including pending pipelines but not host pointers.
class VUReplay
{
public:
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
        std::string error;
        std::vector<CaseTiming> timings;
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
    static Result replay(std::istream &input, uint32_t repeats);

private:
    template <class Archive> static void visitState(Archive &archive, VU1Interpreter &vu);
    static std::vector<uint8_t> saveState(VU1Interpreter &vu);
    static void loadState(const std::vector<uint8_t> &bytes, VU1Interpreter &vu);
};
