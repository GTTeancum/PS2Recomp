#pragma once
#include "ps2_vu1.h"
#include <optional>
#include <vector>

// Host-side boundary for completed VU1 drains. No replacement engine is enabled here.
class VUCompiledState
{
public:
    struct VfWrite
    {
        uint64_t ready = 0, sequence = 0;
        std::array<uint32_t, 4> words{};
        uint8_t reg = 0, lanes = 0;
        bool valid = false;
        bool operator==(const VfWrite &) const = default;
    };
    struct FlagWrite
    {
        uint64_t ready = 0, issue = 0;
        uint32_t mac = 0, status = 0, extraSticky = 0, clip = 0;
        bool valid = false, writesMac = false, writesStatus = false, writesSticky = false, writesClip = false;
        bool operator==(const FlagWrite &) const = default;
    };
    struct Input
    {
        VU1State state{};
        uint64_t cycle = 0, nextSequence = 0;
        uint32_t budget = 0, vfMask = 0, flagMask = 0;
        std::array<VfWrite, 16> vfWrites{};
        std::array<FlagWrite, 8> flags{};
        std::array<std::array<uint64_t, 4>, 32> vfReady{}, vfLatest{};
        int32_t branchBackupValue = 0;
        uint8_t branchBackupReg = 0;
        bool branchBackupValid = false;
    };
    struct Packet
    {
        uint64_t cycle = 0; // Relative to Input::cycle.
        std::vector<uint8_t> bytes;
    };
    struct Output
    {
        VU1State state{};
        uint64_t elapsed = 0;
        uint32_t statusMask = 0, macMask = 0;
        std::array<uint8_t, 16384> data{};
        std::vector<Packet> packets;
        bool dataIsLive = false;
    };
    static std::optional<Input> capture(const VU1Interpreter &, uint32_t budget);
    // False means nothing was changed/submitted. True is a completed commit:
    // callers must not fall back after it, including after downstream GS work.
    static bool commit(VU1Interpreter &, const Input &, const Output &,
        uint8_t *data, uint32_t dataSize, GS &, PS2Memory * = nullptr);
    // Persistent streams hold exclusive ownership between capture and commit.
    // This keeps output validation while avoiding a second full pipeline capture.
    static bool commitStream(VU1Interpreter &, const Input &, Output &&,
        uint8_t *data, uint32_t dataSize, GS &, PS2Memory * = nullptr);

private:
    static bool commitInternal(VU1Interpreter &, const Input &, const Output &,
        uint8_t *data, uint32_t dataSize, GS &, PS2Memory *, bool streamOwned,
        Output *ownedOutput);
};
