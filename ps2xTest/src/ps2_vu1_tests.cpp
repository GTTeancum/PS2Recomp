#include "MiniTest.h"
#include "ReplaySampler.h"
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
#include "runtime/ps2_vu1_native_module.h"
#endif
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_replay.h"

#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    constexpr uint32_t kVuUpperNop = 0x000002FFu;

    struct Vu1Fixture
    {
        PS2Memory mem;
        GS gs;
        uint8_t *code = nullptr;
        uint8_t *data = nullptr;

        bool initialize()
        {
            if (!mem.initialize())
                return false;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            code = mem.getVU1Code();
            data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);
            return code != nullptr && data != nullptr;
        }
    };

    uint32_t makeVifCmd(uint8_t opcode, uint8_t num, uint16_t imm)
    {
        return (static_cast<uint32_t>(opcode) << 24) |
               (static_cast<uint32_t>(num) << 16) |
               static_cast<uint32_t>(imm);
    }

    uint64_t makeGifTag(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true)
    {
        uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
        if (eop)
            tag |= (1ull << 15);
        tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
        tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
        return tag;
    }

    uint32_t makeVuLowerSpecial(uint8_t specialOp, uint8_t is, uint8_t it = 0u, uint8_t id = 0u, uint8_t dest = 0u)
    {
        return (0x40u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(it & 0x1Fu) << 16) |
               (static_cast<uint32_t>(is & 0x1Fu) << 11) |
               (static_cast<uint32_t>(id & 0x1Fu) << 6) |
               (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
               static_cast<uint32_t>(specialOp & 0x3u) |
               0x3Cu;
    }

    uint32_t makeVuLowerDirect(uint8_t funct, uint8_t is, uint8_t it = 0u, uint8_t id = 0u, uint8_t dest = 0u)
    {
        return (0x40u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(it & 0x1Fu) << 16) |
               (static_cast<uint32_t>(is & 0x1Fu) << 11) |
               (static_cast<uint32_t>(id & 0x1Fu) << 6) |
               static_cast<uint32_t>(funct & 0x3Fu);
    }

    uint32_t makeVuUpper(uint8_t op, uint8_t dest, uint8_t ft, uint8_t fs, uint8_t fd)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
               (static_cast<uint32_t>(fs & 0x1Fu) << 11) |
               (static_cast<uint32_t>(fd & 0x1Fu) << 6) |
               static_cast<uint32_t>(op & 0x3Fu);
    }

    uint32_t makeVuUpperSpecial(uint8_t specialOp, uint8_t dest, uint8_t ft, uint8_t fs)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(ft & 0x1Fu) << 16) |
               (static_cast<uint32_t>(fs & 0x1Fu) << 11) |
               (static_cast<uint32_t>(specialOp & 0x7Cu) << 4) |
               static_cast<uint32_t>(specialOp & 0x3u) |
               0x3Cu;
    }

    uint32_t makeVuFlagImmediate(uint8_t opcode, uint8_t targetVi, uint16_t immediate)
    {
        return (static_cast<uint32_t>(opcode & 0x7Fu) << 25) |
               (static_cast<uint32_t>((immediate >> 11) & 0x1u) << 21) |
               (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
               static_cast<uint32_t>(immediate & 0x7FFu);
    }

    uint32_t makeVuFlagRegister(uint8_t opcode, uint8_t targetVi, uint8_t sourceVi)
    {
        return (static_cast<uint32_t>(opcode & 0x7Fu) << 25) |
               (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
               (static_cast<uint32_t>(sourceVi & 0xFu) << 11);
    }

    uint32_t makeVuLq(uint8_t dest, uint8_t targetVf, uint8_t baseVi, int16_t imm)
    {
        return (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(targetVf & 0x1Fu) << 16) |
               (static_cast<uint32_t>(baseVi & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuSq(uint8_t dest, uint8_t sourceVf, uint8_t baseVi, int16_t imm)
    {
        return (0x01u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(baseVi & 0xFu) << 16) |
               (static_cast<uint32_t>(sourceVf & 0x1Fu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuIaddiu(uint8_t it, uint8_t is, int16_t imm)
    {
        return (0x08u << 25) |
               (static_cast<uint32_t>(it & 0xFu) << 16) |
               (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuBranch(int16_t imm)
    {
        return (0x20u << 25) | (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuJr(uint8_t is)
    {
        return (0x24u << 25) |
               (static_cast<uint32_t>(is & 0xFu) << 11);
    }

    uint32_t makeVuIbne(uint8_t is, uint8_t it, int16_t imm)
    {
        return (0x29u << 25) |
               (static_cast<uint32_t>(it & 0xFu) << 16) |
               (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuIbgtz(uint8_t is, int16_t imm)
    {
        return (0x2Du << 25) |
               (static_cast<uint32_t>(is & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuIlw(uint8_t dest, uint8_t targetVi, uint8_t baseVi, int16_t imm)
    {
        return (0x04u << 25) |
               (static_cast<uint32_t>(dest & 0xFu) << 21) |
               (static_cast<uint32_t>(targetVi & 0xFu) << 16) |
               (static_cast<uint32_t>(baseVi & 0xFu) << 11) |
               (static_cast<uint32_t>(imm) & 0x7FFu);
    }

    uint32_t makeVuDiv(uint8_t fs, uint8_t ft, uint8_t fsf, uint8_t ftf)
    {
        return makeVuLowerSpecial(0x38u, fs, ft, 0u, static_cast<uint8_t>(((ftf & 0x3u) << 2) | (fsf & 0x3u)));
    }

    uint32_t makeVuSqrt(uint8_t ft, uint8_t ftf)
    {
        return makeVuLowerSpecial(0x39u, 0u, ft, 0u, static_cast<uint8_t>((ftf & 0x3u) << 2));
    }

    void writeVuInstructionPair(uint8_t *code, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        std::memcpy(code + pc, &lower, sizeof(lower));
        std::memcpy(code + pc + sizeof(lower), &upper, sizeof(upper));
    }

    uint64_t packVuInstructionPair(uint32_t lower, uint32_t upper)
    {
        return static_cast<uint64_t>(lower) | (static_cast<uint64_t>(upper) << 32);
    }

    void writeTrackedVuInstructionPair(Vu1Fixture &fx, uint32_t pc, uint32_t lower, uint32_t upper)
    {
        fx.mem.write64(PS2_VU1_CODE_BASE + pc, packVuInstructionPair(lower, upper));
    }

    void appendU32(std::vector<uint8_t> &bytes, uint32_t value)
    {
        const uint8_t *src = reinterpret_cast<const uint8_t *>(&value);
        bytes.insert(bytes.end(), src, src + sizeof(value));
    }

    void uploadVu1Mpg(PS2Memory &mem, uint16_t instructionAddress, uint32_t lower, uint32_t upper)
    {
        std::vector<uint8_t> packet;
        appendU32(packet, makeVifCmd(0x4Au, 1u, instructionAddress));
        appendU32(packet, lower);
        appendU32(packet, upper);
        mem.processVIF1Data(packet.data(), static_cast<uint32_t>(packet.size()));
    }

    void writeVuQword(uint8_t *data, uint32_t qwordIndex, const float values[4])
    {
        std::memcpy(data + qwordIndex * 16u, values, sizeof(float) * 4u);
    }

    void readVuQword(const uint8_t *data, uint32_t qwordIndex, float values[4])
    {
        std::memcpy(values, data + qwordIndex * 16u, sizeof(float) * 4u);
    }
}

void register_ps2_vu1_tests()
{
    MiniTest::Case("PS2VU1", [](TestCase &tc)
    {
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
        tc.Run("native VU upper module rejects incompatible hosts and reloads", [](TestCase &t)
        {
            const auto host = VUNative::makeHost();
            VUNativeModule relative("ps2_vu_native_upper.dll");
            t.IsTrue(relative.initialize(host) == nullptr, "Relative module paths must not load");
            VUNativeModule missing(std::filesystem::path(PS2X_TEST_VU_NATIVE_MODULE) /
                                   "missing.dll");
            t.IsTrue(missing.initialize(host) == nullptr, "Missing modules must not initialize");
            for (uint32_t attempt = 0; attempt < 2u; ++attempt)
            {
                VUNativeModule module(PS2X_TEST_VU_NATIVE_MODULE);
                auto bad = host;
                ++bad.version;
                t.IsTrue(module.initialize(bad) == nullptr, "Reject a different interface version");
                bad = host;
                ++bad.interpreterBytes;
                t.IsTrue(module.initialize(bad) == nullptr, "Reject a different interpreter layout");
                bad = host;
                bad.fingerprint[0] ^= 1;
                t.IsTrue(module.initialize(bad) == nullptr, "Reject a stale source fingerprint");
                bad = host;
                bad.configuration[0] ^= 1;
                t.IsTrue(module.initialize(bad) == nullptr, "Reject a different build configuration");
                bad = host;
                bad.fmac = nullptr;
                t.IsTrue(module.initialize(bad) == nullptr, "Reject missing callbacks");
                t.IsTrue(module.initialize(host, sizeof(host) - 1u) == nullptr, "Reject a truncated interface");
                auto lookup = module.initialize(host);
                t.IsTrue(lookup != nullptr, "Matching module must initialize and reload");
                if (lookup)
                {
                    t.IsTrue(lookup(kVuUpperNop) != nullptr, "Synthetic NOP kernel must exist");
                    t.IsTrue(lookup(0xffffffffu) == nullptr, "Unknown words must fall back");
                }
            }
        });

        tc.Run("native VU upper kernels reproduce arithmetic boundary replays", [](TestCase &t)
        {
            VUNativeModule module(PS2X_TEST_VU_NATIVE_MODULE);
            const auto lookup = module.initialize(VUNative::makeHost());
            t.IsTrue(lookup != nullptr, "Native arithmetic module must load");
            if (!lookup)
                return;
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            constexpr uint32_t words[] = {
#define VU_NATIVE_WORD(Word) Word,
#include "../../ps2xRuntime/src/lib/vu/ps2_vu1_native_synthetic.inc"
#undef VU_NATIVE_WORD
            };
            constexpr uint32_t values[] = {
                0u, 0x80000000u, 1u, 0x807fffffu, 0x00800000u, 0x80800000u,
                0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
                0x7fc01234u, 0x3f800000u, 0xc0000000u, 0x3f000001u,
                0x4f000000u, 0xcf000000u
            };
            uint32_t random = 0x9e3779b9u;
            for (uint32_t sample = 0; sample < 32u; ++sample)
            {
                std::ostringstream recorded(std::ios::binary);
                uint32_t batchCases = 0u;
                uint32_t checkedCases = 0u;
                const auto checkBatch = [&]()
                {
                    std::istringstream input(recorded.str(), std::ios::binary);
                    const auto result = VUReplay::replay(input, 2u, nullptr, nullptr, lookup);
                    t.IsTrue(result.error.empty(), "Arithmetic sample " + std::to_string(sample) + ": " + result.error);
                    t.Equals(result.cases, batchCases, "Every kernel in the batch must replay");
                    t.IsTrue(result.nativeUpper != 0u && result.interpretedUpper == 0u,
                             "Arithmetic replay must use native kernels exclusively");
                    checkedCases += result.cases;
                    recorded.str("");
                    recorded.clear();
                    batchCases = 0u;
                    return result.error.empty();
                };
                for (const uint32_t word : words)
                {
                    t.IsTrue(lookup(word) != nullptr, "Every synthetic instruction needs a native kernel");
                    writeTrackedVuInstructionPair(fx, 0u, makeVuLowerSpecial(0x30u, 0u), word);
                    writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop | 0x40000000u);
                    writeTrackedVuInstructionPair(fx, 16u, 0u, kVuUpperNop);
                    VU1Interpreter vu;
                    for (uint32_t reg = 1; reg < 32u; ++reg)
                        for (uint32_t lane = 0; lane < 4u; ++lane)
                        {
                            random ^= random << 13u;
                            random ^= random >> 17u;
                            random ^= random << 5u;
                            const auto bits = sample < std::size(values)
                                ? values[(sample + reg * 3u + lane) % std::size(values)] : random;
                            std::memcpy(&vu.state().vf[reg][lane], &bits, sizeof(bits));
                        }
                    std::memcpy(vu.state().acc, vu.state().vf[4], sizeof(vu.state().acc));
                    vu.state().q = vu.state().vf[5][0];
                    vu.state().i = vu.state().vf[6][0];
                    vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                               fx.gs, &fx.mem, 0u, 0u, 0u, 0u);
                    t.IsTrue(VUReplay::record(recorded, vu, fx.code, fx.data, fx.gs, &fx.mem, 16u),
                             "Synthetic arithmetic must record without overflow");
                    if (++batchCases == 32u && !checkBatch())
                        return;
                }
                if (batchCases != 0u && !checkBatch())
                    return;
                t.Equals(checkedCases, static_cast<uint32_t>(std::size(words)), "Every synthetic instruction must be checked");
            }
        });

        tc.Run("native VU provider switches invalidate decode cache and preserve fallback", [](TestCase &t)
        {
            VUNativeModule module(PS2X_TEST_VU_NATIVE_MODULE);
            const auto lookup = module.initialize(VUNative::makeHost());
            t.IsTrue(lookup != nullptr, "Native arithmetic module must load");
            if (!lookup)
                return;
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            writeTrackedVuInstructionPair(fx, 0u, 0u, kVuUpperNop | 0x40000000u);
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop);
            VU1Interpreter vu;
            const auto run = [&] {
                vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                           fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            };
            run();
            t.Equals(vu.upperCounters().native, uint64_t{0}, "Default provider is interpreted");
            vu.setUpperLookup(lookup);
            run();
            t.IsTrue(vu.upperCounters().native > 0u, "Attaching provider invalidates an existing cache");
            const auto attached = vu.upperCounters();
            vu.setUpperLookup(nullptr);
            run();
            t.Equals(vu.upperCounters().native, attached.native, "Detach removes cached native pointers");
            t.IsTrue(vu.upperCounters().interpreted > attached.interpreted, "Detached VU interprets again");
            vu.setUpperLookup(lookup);
            const uint32_t fallbackWord = makeVuUpper(0x28u, 0xFu, 31u, 31u, 31u);
            t.IsTrue(lookup(fallbackWord) == nullptr, "Fallback test word must not be compiled");
            writeTrackedVuInstructionPair(fx, 0u, 0u, fallbackWord);
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop | 0x40000000u);
            writeTrackedVuInstructionPair(fx, 16u, 0u, kVuUpperNop);
            vu.state().vf[31][0] = 7.0f;
            const auto beforeFallback = vu.upperCounters();
            run();
            t.Equals(vu.state().vf[31][0], 14.0f, "Unknown arithmetic must execute through the interpreter");
            t.Equals(vu.upperCounters().interpreted, beforeFallback.interpreted + 1u,
                     "Code edits must invalidate the native selection at that PC");
            vu.setUpperLookup(nullptr);
        });
#endif
#if defined(PS2X_ENABLE_VU_NATIVE_PAIRS)
        tc.Run("native VU pairs preserve execution and interpreter fallback", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            const uint32_t lower = makeVuLowerDirect(0x30u, 1u, 2u, 3u);
            writeTrackedVuInstructionPair(fx, 0u, lower, kVuUpperNop);
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop | 0x40000000u);
            writeTrackedVuInstructionPair(fx, 16u, 0u, kVuUpperNop);

            VU1Interpreter vu;
            vu.setNativePairsEnabled(true);
            vu.state().vi[1] = 4;
            vu.state().vi[2] = 7;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            t.Equals(vu.state().vi[3], 11, "Native lower IADD must preserve the interpreted result");
            const auto native = vu.pairCounters();
            t.Equals(native.native, uint64_t{3}, "All three synthetic pairs must use native kernels");
            t.Equals(native.interpreted, uint64_t{0}, "Synthetic native sequence must not fall back");

            vu.setNativePairsEnabled(false);
            vu.state().vi[1] = 9;
            vu.state().vi[2] = 6;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            t.Equals(vu.state().vi[3], 15, "Disabled provider must retain interpreter behavior");
            const auto interpreted = vu.pairCounters();
            t.Equals(interpreted.native, native.native, "Disabling must stop native pair execution");
            t.Equals(interpreted.interpreted, native.interpreted + 3u,
                     "Disabling must invalidate cached pair kernels");
        });
#endif
#if defined(PS2X_ENABLE_VU_NATIVE_BLOCKS)
        tc.Run("native VU blocks preserve loops stores and budget fallback", [](TestCase &t)
        {
            constexpr uint32_t startPc = 0x3000u;
            const auto initialize = [&](Vu1Fixture &fx, VU1Interpreter &vu)
            {
                if (!fx.initialize())
                    return false;
                writeTrackedVuInstructionPair(fx, startPc + 0u,
                                               makeVuLq(0xFu, 2u, 1u, 0), kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 8u, 0u, kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 16u, 0u, kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 24u, 0u, kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 32u,
                                               makeVuSq(0xFu, 2u, 3u, 0), kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 40u,
                                               makeVuIaddiu(4u, 4u, -1), kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 48u,
                                               makeVuIbgtz(4u, -7), kVuUpperNop);
                writeTrackedVuInstructionPair(fx, startPc + 56u, 0u, kVuUpperNop);

                const float source[4] = {1.25f, -2.5f, 3.75f, 1.0f};
                writeVuQword(fx.data, 1u, source);
                vu.state().vi[1] = 1;
                vu.state().vi[3] = 4;
                vu.state().vi[4] = 2;
                return true;
            };
            const auto compare = [&](const Vu1Fixture &expectedFx,
                                     const VU1Interpreter &expected,
                                     const Vu1Fixture &actualFx,
                                     const VU1Interpreter &actual)
            {
                t.Equals(actual.state().pc, expected.state().pc,
                         "Native block PC must match the interpreter");
                t.Equals(actual.state().cycles, expected.state().cycles,
                         "Native block cycle count must match the interpreter");
                t.Equals(actual.state().vi[4], expected.state().vi[4],
                         "Native block loop counter must match the interpreter");
                t.IsTrue(std::memcmp(actual.state().vf[2], expected.state().vf[2],
                                     sizeof(actual.state().vf[2])) == 0,
                         "Native block vector result must match the interpreter");
                t.IsTrue(std::memcmp(actualFx.data, expectedFx.data,
                                     PS2_VU1_DATA_SIZE) == 0,
                         "Native block stores must match the interpreter");
            };

            Vu1Fixture interpretedFx;
            Vu1Fixture nativeFx;
            VU1Interpreter interpreted;
            VU1Interpreter native;
            t.IsTrue(initialize(interpretedFx, interpreted),
                     "Interpreted VU1 fixture should initialize");
            t.IsTrue(initialize(nativeFx, native),
                     "Native VU1 fixture should initialize");
            native.setNativeBlocksEnabled(true);
            interpreted.execute(interpretedFx.code, PS2_VU1_CODE_SIZE,
                                interpretedFx.data, PS2_VU1_DATA_SIZE,
                                interpretedFx.gs, &interpretedFx.mem,
                                startPc, 0u, 0u, 16u);
            native.execute(nativeFx.code, PS2_VU1_CODE_SIZE,
                           nativeFx.data, PS2_VU1_DATA_SIZE,
                           nativeFx.gs, &nativeFx.mem,
                           startPc, 0u, 0u, 16u);
            compare(interpretedFx, interpreted, nativeFx, native);
            const auto nativeCounters = native.blockCounters();
            t.Equals(nativeCounters.attempted, uint64_t{1},
                     "The synthetic loop should enter one native block");
            t.Equals(nativeCounters.executed, uint64_t{1},
                     "The synthetic loop should execute natively");
            t.Equals(nativeCounters.pairs, uint64_t{16},
                     "The native block should execute both loop iterations");

            Vu1Fixture budgetInterpretedFx;
            Vu1Fixture budgetNativeFx;
            VU1Interpreter budgetInterpreted;
            VU1Interpreter budgetNative;
            t.IsTrue(initialize(budgetInterpretedFx, budgetInterpreted),
                     "Budget reference fixture should initialize");
            t.IsTrue(initialize(budgetNativeFx, budgetNative),
                     "Budget native fixture should initialize");
            budgetNative.setNativeBlocksEnabled(true);
            budgetInterpreted.execute(budgetInterpretedFx.code, PS2_VU1_CODE_SIZE,
                                      budgetInterpretedFx.data, PS2_VU1_DATA_SIZE,
                                      budgetInterpretedFx.gs, &budgetInterpretedFx.mem,
                                      startPc, 0u, 0u, 7u);
            budgetNative.execute(budgetNativeFx.code, PS2_VU1_CODE_SIZE,
                                 budgetNativeFx.data, PS2_VU1_DATA_SIZE,
                                 budgetNativeFx.gs, &budgetNativeFx.mem,
                                 startPc, 0u, 0u, 7u);
            compare(budgetInterpretedFx, budgetInterpreted,
                    budgetNativeFx, budgetNative);
            const auto budgetCounters = budgetNative.blockCounters();
            t.Equals(budgetCounters.attempted, uint64_t{1},
                     "A short budget should still attempt the native block");
            t.Equals(budgetCounters.executed, uint64_t{0},
                     "A short budget should fall back before partial execution");
            t.Equals(budgetCounters.pairs, uint64_t{0},
                     "Budget fallback must not report native pairs");

            Vu1Fixture pendingInterpretedFx;
            Vu1Fixture pendingNativeFx;
            VU1Interpreter pendingInterpreted;
            VU1Interpreter pendingNative;
            t.IsTrue(initialize(pendingInterpretedFx, pendingInterpreted),
                     "Pending-VI reference fixture should initialize");
            t.IsTrue(initialize(pendingNativeFx, pendingNative),
                     "Pending-VI native fixture should initialize");
            const auto stagePendingVi = [&](Vu1Fixture &fx, VU1Interpreter &vu)
            {
                writeTrackedVuInstructionPair(fx, startPc - 8u,
                                               makeVuIlw(0x8u, 3u, 5u, 0),
                                               kVuUpperNop);
                const uint32_t loadedVi = 6u;
                std::memcpy(fx.data + 2u * 16u, &loadedVi, sizeof(loadedVi));
                vu.state().vi[5] = 2;
                vu.execute(fx.code, PS2_VU1_CODE_SIZE,
                           fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                           startPc - 8u, 0u, 0u, 1u);
                t.Equals(vu.state().vi[3], int32_t{4},
                         "Four-cycle ILW must still be pending at block entry");
            };
            stagePendingVi(pendingInterpretedFx, pendingInterpreted);
            stagePendingVi(pendingNativeFx, pendingNative);
            pendingNative.setNativeBlocksEnabled(true);
            pendingInterpreted.resume(pendingInterpretedFx.code, PS2_VU1_CODE_SIZE,
                                      pendingInterpretedFx.data, PS2_VU1_DATA_SIZE,
                                      pendingInterpretedFx.gs, &pendingInterpretedFx.mem,
                                      0u, 0u, 16u);
            pendingNative.resume(pendingNativeFx.code, PS2_VU1_CODE_SIZE,
                                 pendingNativeFx.data, PS2_VU1_DATA_SIZE,
                                 pendingNativeFx.gs, &pendingNativeFx.mem,
                                 0u, 0u, 16u);
            compare(pendingInterpretedFx, pendingInterpreted,
                    pendingNativeFx, pendingNative);
            t.Equals(pendingNative.state().vi[3], int32_t{6},
                     "Pending VI value must retire before its in-block read");
            t.Equals(pendingNative.blockCounters().pairs, uint64_t{16},
                     "A safe pending VI write must not force block fallback");

            constexpr uint32_t vfBlockPc = 0x3100u;
            const auto runPendingVf = [&](Vu1Fixture &fx, VU1Interpreter &vu,
                                          bool nativeBlocks)
            {
                writeTrackedVuInstructionPair(fx, vfBlockPc - 24u,
                                               makeVuLq(0xFu, 2u, 1u, 0),
                                               kVuUpperNop);
                writeTrackedVuInstructionPair(fx, vfBlockPc - 16u, 0u,
                                               kVuUpperNop);
                writeTrackedVuInstructionPair(fx, vfBlockPc - 8u, 0u,
                                               kVuUpperNop);
                writeTrackedVuInstructionPair(
                    fx, vfBlockPc, 0u,
                    makeVuUpper(0x28u, 0xFu, 2u, 1u, 3u));
                writeTrackedVuInstructionPair(fx, vfBlockPc + 8u, 0u,
                                               kVuUpperNop);
                const float source[4] = {2.0f, 4.0f, 6.0f, 8.0f};
                writeVuQword(fx.data, 1u, source);
                vu.state().vi[1] = 1;
                vu.state().vf[1][0] = 1.0f;
                vu.state().vf[1][1] = 2.0f;
                vu.state().vf[1][2] = 3.0f;
                vu.state().vf[1][3] = 4.0f;
                vu.execute(fx.code, PS2_VU1_CODE_SIZE,
                           fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                           vfBlockPc - 24u, 0u, 0u, 3u);
                vu.setNativeBlocksEnabled(nativeBlocks);
                vu.resume(fx.code, PS2_VU1_CODE_SIZE,
                          fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                          0u, 0u, 3u);
            };
            Vu1Fixture pendingVfInterpretedFx;
            Vu1Fixture pendingVfNativeFx;
            VU1Interpreter pendingVfInterpreted;
            VU1Interpreter pendingVfNative;
            t.IsTrue(pendingVfInterpretedFx.initialize(),
                     "Pending-VF reference fixture should initialize");
            t.IsTrue(pendingVfNativeFx.initialize(),
                     "Pending-VF native fixture should initialize");
            runPendingVf(pendingVfInterpretedFx, pendingVfInterpreted, false);
            runPendingVf(pendingVfNativeFx, pendingVfNative, true);
            compare(pendingVfInterpretedFx, pendingVfInterpreted,
                    pendingVfNativeFx, pendingVfNative);
            t.IsTrue(std::memcmp(pendingVfNative.state().vf[3],
                                 pendingVfInterpreted.state().vf[3],
                                 sizeof(pendingVfNative.state().vf[3])) == 0,
                     "Native block must wait for its first VF source");
            t.Equals(pendingVfNative.blockCounters().attempted, uint64_t{1},
                     "The delayed VF source should stall inside one block attempt");
            t.Equals(pendingVfNative.blockCounters().pairs, uint64_t{2},
                     "The block should execute after the delayed VF source retires");
        });
#endif
        tc.Run("VU replay capture schedule spreads bounded deterministic quotas", [](TestCase &t)
        {
            VUReplay::CaptureSchedule schedule;
            VUReplay::CaptureSchedule duplicate;
            for (uint32_t index = 0u; index < 1024u; ++index)
                t.IsTrue(!schedule.select(1099u, 64u), "Do not capture before gameplay");
            t.IsTrue(!schedule.select(1100u, 0u), "Do not capture empty budgets");
            t.IsTrue(!schedule.select(1100u, (1u << 20u) + 1u), "Reject oversized budgets");
            t.Equals(schedule.random, duplicate.random, "Ineligible calls must not consume sampling state");
            uint64_t lastShort = 0u, lastLong = 0u;
            uint32_t shortCount = 0u, longCount = 0u;
            bool deterministic = true, spaced = true;
            for (uint64_t tick = 1100u; tick < 1180u; ++tick)
            {
                for (uint32_t index = 0u; index < 8192u; ++index)
                {
                    const uint32_t budget = (index & 1u) != 0u ? (1u << 20u) : 64u;
                    const bool selected = schedule.select(tick, budget);
                    deterministic &= selected == duplicate.select(tick, budget);
                    if (!selected)
                        continue;
                    auto &last = budget > 64u ? lastLong : lastShort;
                    auto &count = budget > 64u ? longCount : shortCount;
                    spaced &= count == 0u || tick >= last + 4u;
                    last = tick;
                    ++count;
                }
            }
            t.IsTrue(deterministic, "Identical call streams must select identical samples");
            t.IsTrue(spaced, "Each quota must wait four ticks between captures");
            t.Equals(shortCount, 16u, "Short capture quota must be bounded and fill");
            t.Equals(longCount, 16u, "Long capture quota must be bounded and fill");
            t.IsTrue(schedule.complete(), "Both capture quotas should complete");
            t.IsTrue(lastShort >= 1160u && lastLong >= 1160u,
                     "Captures must span at least sixty gameplay ticks");
        });

#if defined(PS2X_ENABLE_VU_NATIVE_BLOCKS)
        tc.Run("native VU block entry deadlines preserve lane reads and overwritten VI", [](TestCase &t)
        {
            constexpr uint32_t startPc = 0x3200u;
            for (uint32_t source : {2u, 4u, 32u})
            for (uint32_t lane : {0x8u, 0x4u, 0x2u, 0x1u, 0xFu})
            for (uint32_t elapsed : {1u, 2u, 3u})
            for (uint32_t budget : {0u, 1u, 7u, 8u, 9u, 16u})
            {
                Vu1Fixture referenceFx;
                Vu1Fixture nativeFx;
                VU1Interpreter reference;
                VU1Interpreter native;
                const auto initialize = [&](Vu1Fixture &fx, VU1Interpreter &vu)
                {
                    if (!fx.initialize())
                        return false;
                    writeTrackedVuInstructionPair(fx, startPc,
                        makeVuIaddiu(2u, 0u, 1), kVuUpperNop);
                    for (uint32_t index = 1u; index <= 4u; ++index)
                        writeTrackedVuInstructionPair(fx, startPc + index * 8u,
                            index == 1u ? makeVuSq(0x8u, 4u, 2u, 0) : 0u,
                            makeVuUpper(0x28u, 0x10u >> index, 2u, 1u, 4u + index));
                    for (uint32_t index = 5u; index < 8u; ++index)
                        writeTrackedVuInstructionPair(fx, startPc + index * 8u, 0u, kVuUpperNop);
                    const uint32_t preludePc = startPc - elapsed * 8u;
                    writeTrackedVuInstructionPair(fx, preludePc,
                        source == 32u ? makeVuIlw(lane, 2u, 1u, 0) :
                                        makeVuLq(lane, source, 1u, 0), kVuUpperNop);
                    for (uint32_t pc = preludePc + 8u; pc < startPc; pc += 8u)
                        writeTrackedVuInstructionPair(fx, pc, 0u, kVuUpperNop);
                    const float loaded[4] = {2.0f, -4.0f, 6.0f, -8.0f};
                    writeVuQword(fx.data, 3u, loaded);
                    vu.state().vi[1] = 3;
                    vu.state().vi[2] = 7;
                    for (uint32_t component = 0u; component < 4u; ++component)
                    {
                        vu.state().vf[1][component] = 1.0f;
                        vu.state().vf[2][component] = 10.0f;
                        vu.state().vf[4][component] = 20.0f;
                    }
                    vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                               fx.gs, &fx.mem, preludePc, 0u, 0u, elapsed);
                    return true;
                };
                if (!initialize(referenceFx, reference) || !initialize(nativeFx, native))
                {
                    t.IsTrue(false, "Block deadline fixtures must initialize");
                    return;
                }
                native.setNativeBlocksEnabled(true);
                for (uint32_t cycles : {budget, 1u, 16u})
                {
                    std::ostringstream expected(std::ios::binary);
                    std::ostringstream actual(std::ios::binary);
                    t.IsTrue(VUReplay::record(expected, reference, referenceFx.code,
                        referenceFx.data, referenceFx.gs, &referenceFx.mem, cycles),
                        "Reference deadline slice must record");
                    t.IsTrue(VUReplay::record(actual, native, nativeFx.code,
                        nativeFx.data, nativeFx.gs, &nativeFx.mem, cycles),
                        "Native deadline slice must record");
                    t.IsTrue(expected.str() == actual.str(),
                        "Lane deadlines and VI replacement must preserve full resumed state");
                }
                if (source == 32u && budget >= 8u)
                    t.IsTrue(native.blockCounters().pairs >= 8u,
                        "An overwritten entry VI deadline must permit native execution");
                if (source == 2u && lane == 0x1u && budget >= 8u)
                    t.IsTrue(native.blockCounters().pairs >= 8u,
                        "A later lane read must use its own deadline");
            }
        });

        tc.Run("native VU blocks schedule internal waits with exact budgets and PATH1", [](TestCase &t)
        {
            constexpr uint32_t startPc = 0x3300u;
            for (uint32_t packetKind : {0u, 1u, 2u})
            for (uint32_t loops : {1u, 3u})
            for (uint32_t budget : {0u, 1u, 4u, 8u, 16u, 17u, 18u, 31u, 32u, 33u, 34u, 35u, 64u})
            {
                Vu1Fixture referenceFx;
                Vu1Fixture nativeFx;
                VU1Interpreter reference;
                VU1Interpreter native;
                const auto initialize = [&](Vu1Fixture &fx, VU1Interpreter &vu)
                {
                    if (!fx.initialize())
                        return false;
                    writeTrackedVuInstructionPair(fx, startPc - 8u,
                        packetKind != 0u ? makeVuLowerSpecial(0x6Cu, 0u) : 0u, kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, startPc,
                        makeVuLq(0xFu, 2u, 1u, 0), kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, startPc + 8u, 0u,
                        makeVuUpper(0x28u, 0xFu, 1u, 2u, 3u));
                    writeTrackedVuInstructionPair(fx, startPc + 16u,
                        makeVuSq(0xFu, 3u, 3u, 0), kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, startPc + 24u, 0u,
                        makeVuUpper(0x28u, 0xFu, 2u, 3u, 4u));
                    writeTrackedVuInstructionPair(fx, startPc + 32u, 0u,
                        makeVuUpper(0x2Au, 0xFu, 1u, 4u, 5u));
                    writeTrackedVuInstructionPair(fx, startPc + 40u,
                        makeVuIaddiu(4u, 4u, -1), kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, startPc + 48u,
                        makeVuIbgtz(4u, -7), kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, startPc + 56u, 0u, kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, startPc + 64u, 0u, kVuUpperNop | 0x40000000u);
                    writeTrackedVuInstructionPair(fx, startPc + 72u, 0u, kVuUpperNop);
                    const uint64_t tag = makeGifTag(packetKind == 2u ? 4u : 32u,
                        GIF_FMT_PACKED, 1u, packetKind != 2u);
                    const uint64_t regs = 0xFu;
                    std::memcpy(fx.data, &tag, sizeof(tag));
                    std::memcpy(fx.data + 8u, &regs, sizeof(regs));
                    if (packetKind == 2u)
                    {
                        const uint64_t tail = makeGifTag(16u, GIF_FMT_PACKED, 1u);
                        std::memcpy(fx.data + 80u, &tail, sizeof(tail));
                        std::memcpy(fx.data + 88u, &regs, sizeof(regs));
                    }
                    const float source[4] = {2.0f, -3.0f, 4.0f, -5.0f};
                    writeVuQword(fx.data, 1u, source);
                    vu.state().vi[1] = 1;
                    vu.state().vi[3] = 4;
                    vu.state().vi[4] = static_cast<int32_t>(loops);
                    for (uint32_t lane = 0u; lane < 4u; ++lane)
                        vu.state().vf[1][lane] = static_cast<float>(lane + 1u);
                    vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                               fx.gs, &fx.mem, startPc - 8u, 0u, 0u, 1u);
                    return true;
                };
                if (!initialize(referenceFx, reference) || !initialize(nativeFx, native))
                {
                    t.IsTrue(false, "Scheduled block fixtures must initialize");
                    return;
                }
                native.setNativeBlocksEnabled(true);
                const auto compareSlice = [&](uint32_t cycles)
                {
                    std::ostringstream expected(std::ios::binary);
                    std::ostringstream actual(std::ios::binary);
                    t.IsTrue(VUReplay::record(expected, reference, referenceFx.code,
                        referenceFx.data, referenceFx.gs, &referenceFx.mem, cycles),
                        "Reference scheduled slice must record");
                    t.IsTrue(VUReplay::record(actual, native, nativeFx.code,
                        nativeFx.data, nativeFx.gs, &nativeFx.mem, cycles),
                        "Native scheduled slice must record");
                    t.IsTrue(expected.str() == actual.str(),
                        "Internal waits must preserve complete state and GIF emission cycles");
                };
                compareSlice(budget);
                if (budget < 17u || (packetKind == 2u && budget == 17u))
                    t.Equals(native.blockCounters().pairs, uint64_t{0},
                        "Wait cycles and an unparsed GIFtag must prevent early block execution");
                if (packetKind < 2u && budget >= 17u)
                    t.IsTrue(native.blockCounters().pairs >= 8u,
                        "A seventeen-cycle budget must execute the eight-pair block natively");
                compareSlice(1u);
                compareSlice(64u);
            }
        });

        tc.Run("native VU integer loads preserve delayed results and branch-slot tails", [](TestCase &t)
        {
            constexpr uint32_t startPc = 0x3400u;
            for (int32_t base : {1, 1023, -1})
            for (uint32_t value : {0u, 0x12347FFFu, 0xABCDFFFFu})
            for (bool pending : {false, true})
            for (uint32_t budget : {0u, 1u, 4u, 7u, 10u, 11u, 12u, 13u, 14u, 15u, 32u})
            {
                Vu1Fixture referenceFx, nativeFx;
                VU1Interpreter reference, native;
                const auto initialize = [&](Vu1Fixture &fx, VU1Interpreter &vu)
                {
                    if (!fx.initialize())
                        return false;
                    const uint32_t words[] = {
                        makeVuIlw(8u, 2u, 1u, 0), makeVuIaddiu(3u, 2u, 1),
                        makeVuIlw(4u, 2u, 1u, 1), makeVuIaddiu(2u, 0u, 9),
                        makeVuIlw(1u, 5u, 1u, -1), makeVuIlw(15u, 0u, 1u, 0),
                        makeVuIbgtz(3u, 3), makeVuIlw(2u, 6u, 1u, 0)};
                    for (uint32_t index = 0u; index < 8u; ++index)
                        writeTrackedVuInstructionPair(fx, startPc + index * 8u, words[index], kVuUpperNop);
                    for (uint32_t offset : {64u, 80u})
                    {
                        writeTrackedVuInstructionPair(fx, startPc + offset, 0u, kVuUpperNop | 0x40000000u);
                        writeTrackedVuInstructionPair(fx, startPc + offset + 8u, 0u, kVuUpperNop);
                    }
                    writeTrackedVuInstructionPair(fx, startPc - 8u,
                        pending ? makeVuIlw(8u, 6u, 1u, 1) : 0u, kVuUpperNop);
                    for (int32_t offset : {-1, 0, 1})
                    {
                        const uint32_t address = (static_cast<uint32_t>(base + offset) * 16u) &
                            (PS2_VU1_DATA_SIZE - 1u);
                        const uint32_t data[] = {value, 0x56788000u, 0x1234FEDCu, 0x12348001u};
                        std::memcpy(fx.data + address, data, sizeof(data));
                    }
                    vu.state().vi[1] = base;
                    vu.state().vi[2] = 17;
                    vu.state().vi[3] = 23;
                    vu.state().vi[5] = 29;
                    vu.state().vi[6] = 31;
                    vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                               fx.gs, &fx.mem, startPc - 8u, 0u, 0u, 1u);
                    return true;
                };
                if (!initialize(referenceFx, reference) || !initialize(nativeFx, native))
                {
                    t.IsTrue(false, "Integer-load fixtures must initialize");
                    return;
                }
                native.setNativeBlocksEnabled(true);
                for (uint32_t cycles : {budget, 1u, 1u, 1u, 32u})
                {
                    std::ostringstream expected(std::ios::binary), actual(std::ios::binary);
                    t.IsTrue(VUReplay::record(expected, reference, referenceFx.code,
                        referenceFx.data, referenceFx.gs, &referenceFx.mem, cycles), "Reference ILW slice must record");
                    t.IsTrue(VUReplay::record(actual, native, nativeFx.code,
                        nativeFx.data, nativeFx.gs, &nativeFx.mem, cycles), "Native ILW slice must record");
                    t.IsTrue(expected.str() == actual.str(),
                        "Integer-load stalls, cancellation, and pending tails must preserve complete state");
                }
                if (budget >= 11u)
                    t.IsTrue(native.blockCounters().pairs >= 8u,
                        "Integer loads must execute inside the native block");
            }
        });

        tc.Run("native VU blocks preserve in-flight GIF bytes cycles and packet boundaries", [](TestCase &t)
        {
            constexpr uint32_t startPc = 0x3000u;
            for (uint32_t packetKind = 0u; packetKind < 5u; ++packetKind)
            {
                for (uint32_t budget : {0u, 1u, 7u, 8u, 9u, 16u, 64u})
                {
                    Vu1Fixture referenceFx;
                    Vu1Fixture nativeFx;
                    VU1Interpreter reference;
                    VU1Interpreter native;
                    const auto initialize = [&](Vu1Fixture &fx, VU1Interpreter &vu)
                    {
                        if (!fx.initialize())
                            return false;
                        writeTrackedVuInstructionPair(fx, startPc - 8u,
                            makeVuLowerSpecial(0x6Cu, 0u), kVuUpperNop);
                        writeTrackedVuInstructionPair(fx, startPc,
                            makeVuLq(0xFu, 2u, 1u, 0), kVuUpperNop);
                        for (uint32_t offset : {8u, 16u, 24u, 56u})
                            writeTrackedVuInstructionPair(fx, startPc + offset, 0u, kVuUpperNop);
                        writeTrackedVuInstructionPair(fx, startPc + 32u,
                            makeVuSq(0xFu, 2u, 3u, 0), kVuUpperNop);
                        writeTrackedVuInstructionPair(fx, startPc + 40u,
                            makeVuIaddiu(4u, 4u, -1), kVuUpperNop);
                        writeTrackedVuInstructionPair(fx, startPc + 48u,
                            makeVuIbgtz(4u, -7), kVuUpperNop);
                        writeTrackedVuInstructionPair(fx, startPc + 64u,
                            0u, kVuUpperNop | 0x40000000u);
                        writeTrackedVuInstructionPair(fx, startPc + 72u, 0u, kVuUpperNop);

                        const uint16_t loops = packetKind == 0u ? 1u : packetKind == 1u ? 4u : 16u;
                        const uint64_t tag = makeGifTag(packetKind >= 3u ? 1u : loops,
                                                       GIF_FMT_PACKED, 1u, packetKind < 3u);
                        const uint64_t regs = 0xFu;
                        std::memcpy(fx.data, &tag, sizeof(tag));
                        std::memcpy(fx.data + 8u, &regs, sizeof(regs));
                        std::memset(fx.data + 16u, 0x5Au, 16u * 20u);
                        const float source[4] = {1.25f, -2.5f, 3.75f, 1.0f};
                        writeVuQword(fx.data, 1u, source);
                        if (packetKind >= 3u)
                        {
                            const uint64_t nextTag = makeGifTag(packetKind == 3u ? 16u : 0x7FFFu,
                                                               GIF_FMT_PACKED, 1u);
                            std::memcpy(fx.data + 32u, &nextTag, sizeof(nextTag));
                            std::memcpy(fx.data + 40u, &regs, sizeof(regs));
                        }
                        vu.state().vi[1] = 1;
                        vu.state().vi[3] = 4;
                        vu.state().vi[4] = 2;
                        vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                                   fx.gs, &fx.mem, startPc - 8u, 0u, 0u, 1u);
                        return true;
                    };
                    if (!initialize(referenceFx, reference) || !initialize(nativeFx, native))
                    {
                        t.IsTrue(false, "In-flight transfer fixtures must initialize");
                        return;
                    }
                    native.setNativeBlocksEnabled(true);
                    const auto compareSlice = [&](uint32_t cycles)
                    {
                        std::ostringstream expected(std::ios::binary);
                        std::ostringstream actual(std::ios::binary);
                        t.IsTrue(VUReplay::record(expected, reference, referenceFx.code,
                            referenceFx.data, referenceFx.gs, &referenceFx.mem, cycles),
                            "Reference slice must record");
                        t.IsTrue(VUReplay::record(actual, native, nativeFx.code,
                            nativeFx.data, nativeFx.gs, &nativeFx.mem, cycles),
                            "Native slice must record");
                        t.IsTrue(expected.str() == actual.str(),
                            "Complete state, memory, partial GIF bytes, and emission cycles must match");
                    };
                    compareSlice(budget);
                    if (packetKind < 3u && budget >= 8u)
                        t.IsTrue(native.blockCounters().pairs >= 8u,
                                 "A known packet must permit native block execution");
                    if (packetKind >= 3u && budget == 8u)
                        t.Equals(native.blockCounters().pairs, uint64_t{0u},
                                 "An unparsed GIFtag must keep the first block interpreted");
                    compareSlice(1u);
                    compareSlice(64u);
                }
            }
        });
#endif
        if (const char *path = std::getenv("PS2X_VU_REPLAY_FILE"))
        {
            tc.Run("recorded VU slices reproduce state memory and graphics packets", [path = std::string(path)](TestCase &t)
            {
                uint32_t repeats = 128u;
                if (const char *value = std::getenv("PS2X_VU_REPLAY_REPEATS"))
                {
                    char *end = nullptr;
                    const auto parsed = std::strtoul(value, &end, 10);
                    t.IsTrue(end != value && *end == '\0' && parsed > 0u && parsed <= 4096u,
                             "Replay repetitions must be between 1 and 4096");
                    if (end == value || *end != '\0' || parsed == 0u || parsed > 4096u)
                        return;
                    repeats = static_cast<uint32_t>(parsed);
                }
                std::ifstream input(path, std::ios::binary);
                const char *exportPath = std::getenv("PS2X_VU_REPLAY_UPPER_EXPORT");
                const char *pairExportPath = std::getenv("PS2X_VU_REPLAY_PAIR_EXPORT");
                std::vector<VUReplay::UpperSample> upperSamples;
                std::vector<VUReplay::PairSample> pairSamples;
                VUReplay::Result result;
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
                VUNativeModule module(PS2X_TEST_VU_NATIVE_MODULE);
                VU1Interpreter::UpperLookup lookup = nullptr;
                if (std::getenv("PS2X_VU_REPLAY_NATIVE"))
                {
                    lookup = module.initialize(VUNative::makeHost());
                    t.IsTrue(lookup != nullptr, "Native module must match this host build");
                    if (!lookup)
                        return;
                }
#endif
                {
                    ReplaySampler sampler(std::getenv("PS2X_VU_REPLAY_PROFILE") != nullptr);
                    result = VUReplay::replay(input, repeats, exportPath ? &upperSamples : nullptr,
                                             pairExportPath ? &pairSamples : nullptr
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
                                             , lookup
#endif
                                             );
                }
                if (pairExportPath && result.error.empty())
                {
                    std::ofstream output(pairExportPath);
                    t.IsTrue(VUReplay::writePairKernels(output, pairSamples, 4096u), "Native pair export must succeed");
                    std::printf("[vu-replay:pair-export] candidates=%zu limit=4096 executions-exclude-stalls=1\n",
                                pairSamples.size());
                }
                t.IsTrue(result.error.empty(), result.error);
                std::printf("[vu-replay:upper-coverage] native=%llu interpreted=%llu includes-cold-pass=1\n",
                            static_cast<unsigned long long>(result.nativeUpper),
                            static_cast<unsigned long long>(result.interpretedUpper));
                std::printf("[vu-replay:pair-coverage] native=%llu interpreted=%llu includes-cold-pass=1\n",
                            static_cast<unsigned long long>(result.nativePairs),
                            static_cast<unsigned long long>(result.interpretedPairs));
                if (exportPath && result.error.empty())
                {
                    std::ofstream output(exportPath);
                    t.IsTrue(VUReplay::writeUpperKernels(output, upperSamples, 128u), "Native instruction export must succeed");
                    std::printf("[vu-replay:upper-export] candidates=%zu limit=128 fetch-counts-include-stalls=1\n", upperSamples.size());
                }
                for (size_t index = 0; index < result.timings.size(); ++index)
                {
                    const auto &entry = result.timings[index];
                    std::printf("[vu-replay:case] index=%zu pc=0x%x budget=%u cycles=%llu gif-bytes=%u execute-ms=%.3f\n",
                                index, entry.pc, entry.budget, static_cast<unsigned long long>(entry.cycles),
                                entry.gifBytes, static_cast<double>(entry.executeNs) / 1000000.0);
                }
                std::printf("[vu-replay:result] cases=%u iterations=%llu cycles=%llu execute-ms=%.3f digest=%016llx error=%s\n",
                            result.cases, static_cast<unsigned long long>(result.iterations),
                            static_cast<unsigned long long>(result.cycles),
                            static_cast<double>(result.executeNs) / 1000000.0,
                            static_cast<unsigned long long>(result.digest), result.error.c_str());
            });
        }

        tc.Run("VU replay preserves pending arithmetic pipelines", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            writeTrackedVuInstructionPair(fx, 0u, makeVuDiv(1u, 2u, 0u, 0u),
                                          makeVuUpper(0x28u, 0xFu, 2u, 1u, 3u));
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop | 0x40000000u);
            writeTrackedVuInstructionPair(fx, 16u, 0u, kVuUpperNop);
            VU1Interpreter vu;
            vu.state().vf[1][0] = 8.0f;
            vu.state().vf[2][0] = 2.0f;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
            t.Equals(vu.state().vf[3][0], 0.0f, "FMAC must still be pending at capture");
            t.Equals(vu.state().q, 1.0f, "DIV must still be pending at capture");
            std::ostringstream output(std::ios::binary);
            t.IsTrue(VUReplay::record(output, vu, fx.code, fx.data, fx.gs, &fx.mem, 32u),
                     "The arithmetic slice must be recorded");
            t.Equals(vu.state().vf[3][0], 10.0f, "Recording must execute the actual pending FMAC");
            t.Equals(vu.state().q, 4.0f, "Recording must execute the actual pending DIV");
            std::istringstream input(output.str(), std::ios::binary);
            std::vector<VUReplay::UpperSample> upperSamples;
            std::vector<VUReplay::PairSample> pairSamples;
            const auto result = VUReplay::replay(input, 3u, &upperSamples, &pairSamples);
            t.IsTrue(result.error.empty(), result.error);
            t.Equals(result.cases, 1u, "One arithmetic slice must replay");
            t.Equals(result.iterations, uint64_t{3u}, "All measured repeats must agree");
            t.IsTrue(!upperSamples.empty(), "One-cycle replay must identify upper instructions");
            std::ostringstream kernels;
            t.IsTrue(VUReplay::writeUpperKernels(kernels, upperSamples, 128u), "Synthetic native recipe must be written");
            t.IsTrue(kernels.str().find("VU_NATIVE_WORD(0x000002ffu)") != std::string::npos, "Recipe must include the reached NOP");
            t.IsTrue(!pairSamples.empty(), "One-cycle replay must identify executed instruction pairs");
            std::ostringstream pairKernels;
            t.IsTrue(VUReplay::writePairKernels(pairKernels, pairSamples, 4096u), "Synthetic pair recipe must be written");
            t.IsTrue(pairKernels.str().find("VU_NATIVE_ENTRY(0x0008u)") != std::string::npos,
                     "Pair recipe must retain replay entry PCs");
            t.IsTrue(pairKernels.str().find("VU_NATIVE_BLOCK(0x0008u, 2u)") != std::string::npos,
                     "Pair recipe must partition a bounded straight-line block");
            t.IsTrue(pairKernels.str().find("VU_NATIVE_PAIR(0x0008u, 0x00000000u, 0x400002ffu)") != std::string::npos,
                     "Pair recipe must retain PC and both instruction words");
            t.IsTrue(pairKernels.str().find("VU_NATIVE_PAIR_WORDS(0x00000000u, 0x400002ffu, ") != std::string::npos,
                     "Pair recipe must rank unique words by retired execution count");
            t.IsTrue(pairKernels.str().find("VU_NATIVE_EDGE(0x0008u, 0x0010u)") != std::string::npos,
                     "Pair recipe must retain observed successors");
            t.IsTrue(!VUReplay::writeUpperKernels(kernels, upperSamples, 0u), "Zero kernel limit must be rejected");
            t.IsTrue(!VUReplay::writeUpperKernels(kernels, upperSamples, 513u), "Unbounded kernel limits must be rejected");
            t.IsTrue(!VUReplay::writePairKernels(pairKernels, pairSamples, 0u), "Zero pair limit must be rejected");
            t.IsTrue(!VUReplay::writePairKernels(pairKernels, pairSamples, 4097u), "Unbounded pair limit must be rejected");

            auto truncated = output.str();
            truncated.pop_back();
            std::istringstream badInput(truncated, std::ios::binary);
            t.IsTrue(!VUReplay::replay(badInput, 1u).error.empty(), "Truncated captures must be rejected");
        });

        tc.Run("flag and VF deadlines preserve slot reuse across single-cycle slices", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            constexpr uint32_t pairs = 40u;
            for (uint32_t i = 0; i < pairs + 8u; ++i)
            {
                const bool alternate = ((i / 8u) & 1u) != 0u;
                const uint32_t lower = i >= pairs ? 0u : i % 3u == 0u
                    ? makeVuFlagImmediate(0x15u, 0u, static_cast<uint16_t>((i & 15u) << 6u))
                    : makeVuLowerSpecial(0x30u, alternate ? 1u : 2u, static_cast<uint8_t>(20u + i % 8u), 0u, 0xFu);
                const uint32_t upper = i >= pairs ? kVuUpperNop
                    : makeVuUpper(0x28u, 0xFu, 2u, alternate ? 2u : 1u, static_cast<uint8_t>(4u + i % 8u));
                writeTrackedVuInstructionPair(fx, i * 8u, lower, upper);
            }
            VU1Interpreter vu;
            float expected[32][4]{};
            expected[0][3] = 1.0f;
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                vu.state().vf[1][lane] = expected[1][lane] = static_cast<float>(lane + 1u);
                vu.state().vf[2][lane] = expected[2][lane] = static_cast<float>((lane + 1u) * 10u);
            }
            uint32_t expectedStatus = 0u;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 0u);
            for (uint32_t cycle = 1u; cycle <= pairs + 8u; ++cycle)
            {
                vu.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                          fx.gs, &fx.mem, 0u, 0u, 0u);
                t.Equals(vu.state().cycles, uint64_t{cycle - 1u}, "Zero budget must preserve the cycle");
                vu.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                          fx.gs, &fx.mem, 0u, 0u, 1u);
                if (cycle >= 4u && cycle - 4u < pairs)
                {
                    const uint32_t retired = cycle - 4u;
                    const bool alternate = ((retired / 8u) & 1u) != 0u;
                    for (uint32_t lane = 0; lane < 4u; ++lane)
                    {
                        expected[4u + retired % 8u][lane] = static_cast<float>((lane + 1u) * (alternate ? 20u : 11u));
                        if (retired % 3u != 0u)
                            expected[20u + retired % 8u][lane] = expected[alternate ? 1u : 2u][lane];
                    }
                    if (retired % 3u == 0u)
                        expectedStatus = (retired & 15u) << 6u;
                }
                t.Equals(vu.state().cycles, uint64_t{cycle}, "Each slice must advance one cycle");
                t.Equals(vu.state().pc, cycle * 8u, "Independent pairs must not stall");
                t.Equals(vu.state().status, expectedStatus, "Same-pair FSSET must win at its deadline");
                t.Equals(vu.state().mac, 0u, "Positive ADD results must preserve zero MAC flags");
                for (uint32_t reg = 0; reg < 32u; ++reg)
                    for (uint32_t lane = 0; lane < 4u; ++lane)
                        t.Equals(vu.state().vf[reg][lane], expected[reg][lane], "VF values must commit at their exact deadline");
            }
        });

        tc.Run("fresh execution and reset discard pending flag and VF deadlines", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            writeTrackedVuInstructionPair(fx, 0u, makeVuFlagImmediate(0x15u, 0u, 0xFC0u),
                                          makeVuUpper(0x28u, 0xFu, 2u, 1u, 3u));
            for (uint32_t pc = 8u; pc <= 128u; pc += 8u)
                writeTrackedVuInstructionPair(fx, pc, 0u, kVuUpperNop);
            VU1Interpreter vu;
            for (uint32_t restart = 0; restart < 12u; ++restart)
            {
                vu.state().vf[1][0] = 1.0f;
                vu.state().vf[2][0] = 2.0f;
                vu.state().vf[3][0] = 123.0f;
                vu.state().status = 0x80u;
                vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                           fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
                if ((restart & 1u) != 0u)
                {
                    vu.reset();
                    vu.state().vf[3][0] = 123.0f;
                    vu.state().status = 0x80u;
                }
                vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                           fx.gs, &fx.mem, 8u, 0u, 0u, 12u);
                t.Equals(vu.state().vf[3][0], 123.0f, "Cancelled VF writes must not reappear");
                t.Equals(vu.state().status, 0x80u, "Cancelled flags must not reappear");
            }
        });

        tc.Run("VU replay preserves partial PATH1 transfers and checks packet bytes", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            const uint64_t tag = makeGifTag(4u, GIF_FMT_PACKED, 1u, true);
            const uint64_t regs = 0xFu;
            std::memcpy(fx.data, &tag, sizeof(tag));
            std::memcpy(fx.data + 8u, &regs, sizeof(regs));
            std::memset(fx.data + 16u, 0x5Au, 64u);
            writeTrackedVuInstructionPair(fx, 0u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop | 0x40000000u);
            writeTrackedVuInstructionPair(fx, 16u, 0u, kVuUpperNop);
            VU1Interpreter vu;
            vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
            t.Equals(vu.debugCounters().xgkickFinishes, uint64_t{0u}, "PATH1 must still be active at capture");
            std::ostringstream output(std::ios::binary);
            t.IsTrue(VUReplay::record(output, vu, fx.code, fx.data, fx.gs, &fx.mem, 32u),
                     "The PATH1 slice must be recorded");
            t.Equals(vu.debugCounters().xgkickFinishes, uint64_t{1u}, "Recording must finish the live transfer");
            std::istringstream input(output.str(), std::ios::binary);
            const auto result = VUReplay::replay(input, 2u);
            t.IsTrue(result.error.empty(), result.error);
            t.Equals(result.cases, 1u, "One PATH1 slice must replay");

            auto corrupted = output.str();
            corrupted.back() ^= 1; // Last byte is in the expected emitted GIF packet.
            std::istringstream badInput(corrupted, std::ios::binary);
            t.IsTrue(!VUReplay::replay(badInput, 1u).error.empty(), "Changed packet bytes must fail replay");
            auto wrongCycle = output.str();
            wrongCycle[wrongCycle.size() - 80u - 12u] ^= 1;
            std::istringstream badTiming(wrongCycle, std::ios::binary);
            t.IsTrue(!VUReplay::replay(badTiming, 1u).error.empty(), "Changed packet emission cycles must fail replay");
        });

        tc.Run("VU replay rejects invalid headers and repetition counts", [](TestCase &t)
        {
            std::istringstream badHeader(std::string(8u, '\xFF'), std::ios::binary);
            t.IsTrue(!VUReplay::replay(badHeader, 1u).error.empty(), "Invalid headers must fail");
            std::istringstream empty;
            t.IsTrue(!VUReplay::replay(empty, 1u).error.empty(), "An empty capture must fail");
            std::istringstream noRepeats;
            t.IsTrue(!VUReplay::replay(noRepeats, 0u).error.empty(), "Zero repetitions must fail");
        });

        tc.Run("upper NOP preserves register bits and flags", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            const uint32_t operands[] = {0x00000001u, 0x80000001u, 0x7FC01234u, 0xFF800000u};
            VU1Interpreter vu1;
            auto &state = vu1.state();
            std::memcpy(state.vf[1], operands, sizeof(operands));
            std::memcpy(state.vf[2], operands, sizeof(operands));
            std::memcpy(state.acc, operands, sizeof(operands));
            state.mac = 0x1234u;
            state.status = 0xABCu;
            state.clip = 0x123456u;
            const uint32_t nop = makeVuUpperSpecial(0x2Fu, 0xFu, 2u, 1u);
            writeTrackedVuInstructionPair(fx, 0u, 0u, nop | 0x40000000u);
            writeTrackedVuInstructionPair(fx, 8u, 0u, nop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                        fx.gs, &fx.mem, 0u, 0u, 0u, 32u);
            t.IsTrue(!vu1.isRunning(), "NOP must retain end-bit handling");
            t.Equals(state.pc, 16u, "NOP end-bit delay slot must execute");
            t.Equals(state.cycles, uint64_t{2u}, "NOP pairs must consume two cycles");
            t.IsTrue(std::memcmp(state.vf[1], operands, sizeof(operands)) == 0,
                     "NOP must preserve its encoded source register bits");
            t.IsTrue(std::memcmp(state.vf[2], operands, sizeof(operands)) == 0,
                     "NOP must preserve its encoded target register bits");
            t.IsTrue(std::memcmp(state.acc, operands, sizeof(operands)) == 0,
                     "NOP must preserve accumulator bits");
            t.Equals(state.mac, 0x1234u, "NOP must preserve MAC flags");
            t.Equals(state.status, 0xABCu, "NOP must preserve status flags");
            t.Equals(state.clip, 0x123456u, "NOP must preserve clip flags");
        });

        tc.Run("VU arithmetic workload records registers flags and memory", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            for (uint32_t index = 0u; index < 128u; ++index)
            {
                const uint8_t mask = static_cast<uint8_t>((index % 15u) + 1u);
                const uint8_t fs = static_cast<uint8_t>(1u + index % 8u);
                const uint8_t ft = static_cast<uint8_t>(9u + index % 8u);
                uint32_t upper = kVuUpperNop;
                uint32_t lower = 0u;
                if (index < 48u)
                    upper = makeVuUpper(static_cast<uint8_t>(index), mask, ft, fs,
                                        static_cast<uint8_t>(17u + index % 12u));
                else if (index < 96u)
                {
                    const auto special = static_cast<uint8_t>(index - 48u);
                    if (special != 0x2Bu) // Reserved upper opcode.
                        upper = makeVuUpperSpecial(special, mask, ft, fs);
                }
                else if (index < 126u)
                    lower = (index & 1u) != 0u
                        ? makeVuSq(mask, static_cast<uint8_t>(17u + index % 12u), 0u,
                                   static_cast<int16_t>(index - 96u))
                        : makeVuLq(mask, ft, 0u, static_cast<int16_t>(index - 96u));
                if (index == 126u)
                    upper |= 0x40000000u;
                writeTrackedVuInstructionPair(fx, index * 8u, lower, upper);
            }
            VU1Interpreter vu1;
            uint32_t random = 0x162EA971u;
            auto next = [&]() { random = random * 1664525u + 1013904223u; return random; };
            uint64_t hash = 14695981039346656037ull;
            auto absorb = [&](const void *data, size_t size)
            {
                const auto *bytes = static_cast<const uint8_t *>(data);
                for (size_t index = 0u; index < size; ++index)
                    hash = (hash ^ bytes[index]) * 1099511628211ull;
            };
            uint64_t executeNs = 0u;
            for (uint32_t round = 0u; round < 1024u; ++round)
            {
                vu1.reset();
                auto &state = vu1.state();
                for (uint32_t reg = 1u; reg < 32u; ++reg)
                for (uint32_t lane = 0u; lane < 4u; ++lane)
                {
                    const uint32_t bits = next();
                    std::memcpy(&state.vf[reg][lane], &bits, sizeof(bits));
                }
                for (float &value : state.acc)
                {
                    const uint32_t bits = next();
                    std::memcpy(&value, &bits, sizeof(bits));
                }
                const uint32_t qBits = next();
                const uint32_t iBits = next();
                std::memcpy(&state.q, &qBits, sizeof(qBits));
                std::memcpy(&state.i, &iBits, sizeof(iBits));
                for (uint32_t offset = 0u; offset < PS2_VU1_DATA_SIZE; offset += 4u)
                {
                    const uint32_t bits = next();
                    std::memcpy(fx.data + offset, &bits, sizeof(bits));
                }
                const auto start = std::chrono::steady_clock::now();
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                            fx.gs, &fx.mem, 0u, 0u, 0u, 4096u);
                executeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
                t.IsTrue(!vu1.isRunning(), "The workload must stop after its end-bit delay slot");
                t.Equals(state.pc, 1024u, "All 128 instruction pairs must execute");
                if (vu1.isRunning() || state.pc != 1024u)
                    return;
                absorb(state.vf, sizeof(state.vf));
                absorb(state.vi, sizeof(state.vi));
                absorb(state.acc, sizeof(state.acc));
                absorb(&state.q, sizeof(state.q));
                absorb(&state.i, sizeof(state.i));
                absorb(&state.mac, sizeof(state.mac));
                absorb(&state.clip, sizeof(state.clip));
                absorb(&state.status, sizeof(state.status));
                absorb(&state.cycles, sizeof(state.cycles));
                absorb(fx.data, PS2_VU1_DATA_SIZE);
            }
            std::printf("[vu-arithmetic-workload] rounds=1024 hash=%016llx execute-ms=%.3f\n",
                        static_cast<unsigned long long>(hash), static_cast<double>(executeNs) / 1000000.0);
        });

        tc.Run("upper ADD applies the destination mask", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u, makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u)); // ADD.xz vf3, vf1, vf2

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[2][1] = 20.0f;
            vu1.state().vf[2][2] = 30.0f;
            vu1.state().vf[2][3] = 40.0f;
            vu1.state().vf[3][0] = -1.0f;
            vu1.state().vf[3][1] = -2.0f;
            vu1.state().vf[3][2] = -3.0f;
            vu1.state().vf[3][3] = -4.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);

            t.Equals(vu1.state().vf[3][0], -1.0f,
                     "FMAC destination must remain hidden before its four-cycle writeback");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 3u);
            t.Equals(vu1.state().vf[3][0], 11.0f, "ADD.x should write x");
            t.Equals(vu1.state().vf[3][1], -2.0f, "ADD.xz should preserve y");
            t.Equals(vu1.state().vf[3][2], 33.0f, "ADD.xz should write z");
            t.Equals(vu1.state().vf[3][3], -4.0f, "ADD.xz should preserve w");
        });

        tc.Run("LOI commits the lower immediate after the upper instruction", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const float newI = 7.0f;
            uint32_t lowerImmediate = 0u;
            std::memcpy(&lowerImmediate, &newI, sizeof(newI));
            const uint32_t upperAddiWithIBit = makeVuUpper(0x22u, 0xFu, 0u, 1u, 2u) | 0x80000000u; // ADDi.xyzw vf2, vf1
            writeVuInstructionPair(fx.code, 0u, lowerImmediate, upperAddiWithIBit);

            VU1Interpreter vu1;
            vu1.state().i = 2.0f;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);

            t.Equals(vu1.state().vf[2][0], 0.0f,
                     "ADDi result should remain in the FMAC pipeline");
            t.Equals(vu1.state().i, 7.0f,
                     "LOI should become visible after the upper from the same pair");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 3u);
            t.Equals(vu1.state().vf[2][0], 3.0f, "ADDi should use old I for x");
            t.Equals(vu1.state().vf[2][1], 4.0f, "ADDi should use old I for y");
            t.Equals(vu1.state().vf[2][2], 5.0f, "ADDi should use old I for z");
            t.Equals(vu1.state().vf[2][3], 6.0f, "ADDi should use old I for w");
            t.Equals(vu1.state().i, 7.0f, "LOI should commit lower immediate into I after upper execution");
        });

        tc.Run("ITOF converts the raw signed integer bits without float normalization", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpperSpecial(0x10u, 0xFu, 2u, 1u));

            VU1Interpreter vu1;
            const int32_t raw[4] = {1, -16, 4096, -32768};
            std::memcpy(vu1.state().vf[1], raw, sizeof(raw));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vf[2][0], 1.0f,
                     "ITOF0 must not flush an integer bit pattern that resembles a denormal");
            t.Equals(vu1.state().vf[2][1], -16.0f,
                     "ITOF0 must preserve negative integer bit patterns");
            t.Equals(vu1.state().vf[2][2], 4096.0f,
                     "ITOF0 should convert positive fixed-point source bits");
            t.Equals(vu1.state().vf[2][3], -32768.0f,
                     "ITOF0 should convert negative fixed-point source bits");
        });

        tc.Run("MTIR decodes fsf as a component selector", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            for (uint32_t component = 0; component < 4u; ++component)
            {
                writeVuInstructionPair(
                    fx.code, component * 8u,
                    makeVuLowerSpecial(0x3Cu, 1u,
                                       static_cast<uint8_t>(component + 2u),
                                       0u,
                                       static_cast<uint8_t>(component)),
                    kVuUpperNop);
            }

            VU1Interpreter vu1;
            const uint32_t raw[4] = {0x00001111u, 0x00002222u, 0x00003333u, 0x00004444u};
            std::memcpy(vu1.state().vf[1], raw, sizeof(raw));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[2], 0x1111,
                     "MTIR fsf=x should read VF.x");
            t.Equals(vu1.state().vi[3], 0x2222,
                     "MTIR fsf=y should read VF.y");
            t.Equals(vu1.state().vi[4], 0x3333,
                     "MTIR fsf=z should read VF.z");
            t.Equals(vu1.state().vi[5], 0x4444,
                     "MTIR fsf=w should read VF.w");
        });

        tc.Run("LQ and SQ use VI qword addressing and destination masks", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const float sourceQw[4] = {10.0f, 20.0f, 30.0f, 40.0f};
            const float destQw[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
            writeVuQword(fx.data, 3u, sourceQw);
            writeVuQword(fx.data, 5u, destQw);
            writeVuInstructionPair(fx.code, 0u, makeVuLq(0x5u, 4u, 1u, 1), kVuUpperNop); // LQ.yw vf4, 1(vi1)
            writeVuInstructionPair(fx.code, 8u, makeVuSq(0xAu, 4u, 2u, 1), kVuUpperNop); // SQ.xz vf4, 1(vi2)

            VU1Interpreter vu1;
            vu1.state().vi[1] = 2;
            vu1.state().vi[2] = 4;
            vu1.state().vf[4][0] = 100.0f;
            vu1.state().vf[4][1] = 200.0f;
            vu1.state().vf[4][2] = 300.0f;
            vu1.state().vf[4][3] = 400.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vf[4][1], 200.0f,
                     "LQ result should remain hidden before its fourth cycle");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 1u);
            t.Equals(vu1.state().vf[4][0], 100.0f, "LQ.yw should preserve x");
            t.Equals(vu1.state().vf[4][1], 20.0f, "LQ.yw should load y");
            t.Equals(vu1.state().vf[4][2], 300.0f, "LQ.yw should preserve z");
            t.Equals(vu1.state().vf[4][3], 40.0f, "LQ.yw should load w");

            float stored[4] = {};
            readVuQword(fx.data, 5u, stored);
            t.Equals(stored[0], 100.0f, "SQ.xz should store x");
            t.Equals(stored[1], -2.0f, "SQ.xz should preserve y");
            t.Equals(stored[2], 300.0f, "SQ.xz should store z");
            t.Equals(stored[3], -4.0f, "SQ.xz should preserve w");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(4u),
                     "disjoint LQ/SQ lanes should issue without delaying LQ writeback");
        });

        tc.Run("integer lower ops keep VI0 hardwired to zero", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuIaddiu(2u, 1u, 5), kVuUpperNop);      // IADDIU vi2, vi1, 5
            writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(0u, 2u, 7), kVuUpperNop);      // IADDIU vi0, vi2, 7
            writeVuInstructionPair(fx.code, 16u, makeVuLowerDirect(0x30u, 2u, 1u, 3u), kVuUpperNop); // IADD vi3, vi2, vi1

            VU1Interpreter vu1;
            vu1.state().vi[0] = 99;
            vu1.state().vi[1] = 10;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vi[2], 15, "IADDIU should add signed immediate to VI source");
            t.Equals(vu1.state().vi[3], 25, "IADD should add VI source registers");
            t.Equals(vu1.state().vi[0], 0, "VI0 should remain hardwired to zero");
        });

        tc.Run("XTOP and XITOP expose VIF TOP values to VI registers", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuLowerSpecial(0x68u, 0u, 2u), kVuUpperNop); // XTOP vi2
            writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x69u, 0u, 3u), kVuUpperNop); // XITOP vi3

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0x123u, 0x2ABu, 2u);

            t.Equals(vu1.state().vi[2], 0x123, "XTOP should move TOP into the target VI register");
            t.Equals(vu1.state().vi[3], 0x2AB, "XITOP should move ITOP into the target VI register");
        });

        tc.Run("lower branch commits after one delay-slot instruction", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuBranch(2), kVuUpperNop);              // target pc = 24
            writeVuInstructionPair(fx.code, 8u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop);      // delay slot
            writeVuInstructionPair(fx.code, 16u, makeVuIaddiu(2u, 0u, 99), kVuUpperNop);    // skipped
            writeVuInstructionPair(fx.code, 24u, makeVuIaddiu(3u, 0u, 7), kVuUpperNop);     // branch target

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vi[1], 1, "branch delay slot should execute");
            t.Equals(vu1.state().vi[2], 0, "instruction between delay slot and target should be skipped");
            t.Equals(vu1.state().vi[3], 7, "branch target should execute after the delay slot");
        });

        tc.Run("lower side sees old VF value when upper writes the same register", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code,
                                   0u,
                                   makeVuSq(0xFu, 1u, 1u, 0),                 // SQ.xyzw vf1, 0(vi1)
                                   makeVuUpper(0x28u, 0xFu, 3u, 2u, 1u));     // ADD.xyzw vf1, vf2, vf3

            VU1Interpreter vu1;
            vu1.state().vi[1] = 6;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[2][1] = 20.0f;
            vu1.state().vf[2][2] = 30.0f;
            vu1.state().vf[2][3] = 40.0f;
            vu1.state().vf[3][0] = 100.0f;
            vu1.state().vf[3][1] = 200.0f;
            vu1.state().vf[3][2] = 300.0f;
            vu1.state().vf[3][3] = 400.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 2u);

            float stored[4] = {};
            readVuQword(fx.data, 6u, stored);
            t.Equals(stored[0], 1.0f, "SQ should observe old VF value for x");
            t.Equals(stored[1], 2.0f, "SQ should observe old VF value for y");
            t.Equals(stored[2], 3.0f, "SQ should observe old VF value for z");
            t.Equals(stored[3], 4.0f, "SQ should observe old VF value for w");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                       fx.gs, &fx.mem, 0u, 0u, 2u);
            t.Equals(vu1.state().vf[1][0], 110.0f, "upper ADD should write x after lower read");
            t.Equals(vu1.state().vf[1][1], 220.0f, "upper ADD should write y after lower read");
            t.Equals(vu1.state().vf[1][2], 330.0f, "upper ADD should write z after lower read");
            t.Equals(vu1.state().vf[1][3], 440.0f, "upper ADD should write w after lower read");
        });

        tc.Run("upper suppresses only the colliding lower VF write", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const float loaded[4] = {90.0f, 91.0f, 92.0f, 93.0f};
            writeVuQword(fx.data, 5u, loaded);
            writeVuInstructionPair(
                fx.code,
                0u,
                makeVuLowerSpecial(0x34u, 1u, 1u, 0u, 0xFu),
                makeVuUpper(0x28u, 0x8u, 3u, 2u, 1u));

            VU1Interpreter vu1;
            vu1.state().vi[1] = 5;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[1][3] = 4.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[3][0] = 100.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 1u);

            t.Equals(vu1.state().vf[1][0], 1.0f,
                     "upper write should remain pending until the FMAC writeback cycle");
            t.Equals(vu1.state().vi[1], 6,
                     "LQI post-increment should commit after one cycle");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.Equals(vu1.state().vf[1][0], 110.0f,
                     "upper x should be written");
            t.Equals(vu1.state().vf[1][1], 2.0f,
                     "discarded lower must not leak y into the upper result");
            t.Equals(vu1.state().vf[1][2], 3.0f,
                     "discarded lower must not leak z into the upper result");
            t.Equals(vu1.state().vf[1][3], 4.0f,
                      "discarded lower must not leak w into the upper result");
            t.Equals(vu1.state().vi[1], 6,
                     "LQI post-increment must survive suppression of its colliding VF write");
        });

        tc.Run("upper and lower both read the pre-pair VF state", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuLowerSpecial(0x30u, 1u, 2u, 0u, 0x8u),
                makeVuUpper(0x28u, 0x8u, 3u, 2u, 1u));

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 10.0f;
            vu1.state().vf[2][0] = 20.0f;
            vu1.state().vf[3][0] = 1.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 1u);

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.Equals(vu1.state().vf[1][0], 21.0f,
                     "upper must read vf2 before lower MOVE overwrites it");
            t.Equals(vu1.state().vf[2][0], 10.0f,
                     "lower must read vf1 before upper ADD overwrites it");
        });

        tc.Run("FMAC dependency stalls only the lanes that are read", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpper(0x28u, 0x8u, 2u, 1u, 3u));
            writeVuInstructionPair(
                fx.code, 8u, 0u,
                makeVuUpper(0x28u, 0x8u, 2u, 3u, 4u));

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[2][0] = 2.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vf[4][0], 0.0f,
                     "the dependent result should remain pending until its own writeback");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.Equals(vu1.state().vf[4][0], 5.0f,
                     "dependent ADD should consume the completed x lane");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(8u),
                     "dependency stall and the dependent FMAC writeback must both consume cycles");
        });

        tc.Run("ACC forwarding feeds the next upper instruction without a dependency stall", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpperSpecial(0x28u, 0x8u, 2u, 1u)); // ADDA.x acc, vf1, vf2
            writeVuInstructionPair(
                fx.code, 8u, 0u,
                makeVuUpper(0x29u, 0x8u, 4u, 3u, 5u)); // MADD.x vf5, vf3, vf4

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[2][0] = 10.0f;
            vu1.state().vf[3][0] = 2.0f;
            vu1.state().vf[4][0] = 3.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().acc[0], 11.0f,
                     "ADDA should forward ACC to the following upper instruction");
            t.Equals(vu1.state().vf[5][0], 17.0f,
                     "MADD should consume the forwarded ACC value without stalling");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(5u),
                     "ACC forwarding must not introduce a four-cycle dependency stall");
        });

        tc.Run("ILW result becomes visible after four cycles before IALU consumes it", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            const uint32_t source[4] = {0x1234u, 0u, 0u, 0u};
            std::memcpy(fx.data + 2u * 16u, source, sizeof(source));
            writeVuInstructionPair(
                fx.code, 0u, makeVuIlw(0x8u, 2u, 1u, 0), kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIaddiu(3u, 2u, 1), kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 2;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vi[2], 0x1234,
                     "ILW should commit the selected word after four cycles");
            t.Equals(vu1.state().vi[3], 0x1235,
                     "IADDIU should wait for and consume the ILW result");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(5u),
                     "ILW-to-IALU dependency should account for all stalled cycles");
        });

        tc.Run("integer dependency masks cover every VI source and destination", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            const uint32_t first = 100u, second = 200u;
            std::memcpy(fx.data, &first, sizeof(first));
            std::memcpy(fx.data + 16u, &second, sizeof(second));
            for (uint8_t left = 0u; left < 16u; ++left)
            {
                for (uint8_t right = 0u; right < 16u; ++right)
                {
                    const uint8_t dest = (left + right) & 15u;
                    writeTrackedVuInstructionPair(fx, 0u, makeVuIlw(0x8u, left, 0u, 0), kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, 8u, makeVuIlw(0x8u, right, 0u, 1), kVuUpperNop);
                    writeTrackedVuInstructionPair(fx, 16u, makeVuLowerDirect(0x30u, left, right, dest),
                                                  kVuUpperNop | 0x40000000u);
                    writeTrackedVuInstructionPair(fx, 24u, 0u, kVuUpperNop);
                    VU1Interpreter vu;
                    for (uint32_t reg = 1u; reg < 16u; ++reg)
                        vu.state().vi[reg] = static_cast<int32_t>(reg * 7u);
                    vu.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE,
                               fx.gs, &fx.mem, 0u, 0u, 0u, 64u);
                    const int32_t leftValue = left == 0u ? 0 : (left == right ? 200 : 100);
                    const int32_t rightValue = right == 0u ? 0 : 200;
                    for (uint32_t reg = 0u; reg < 16u; ++reg)
                    {
                        int32_t expected = static_cast<int32_t>(reg * 7u);
                        if (reg == left)
                            expected = 100;
                        if (reg == right)
                            expected = 200;
                        if (reg == dest)
                            expected = leftValue + rightValue;
                        if (reg == 0u)
                            expected = 0;
                        t.Equals(vu.state().vi[reg], expected, "Only the named VI registers may change");
                    }
                    const uint64_t expectedCycles = right != 0u ? 7u : (left != 0u ? 6u : 4u);
                    t.Equals(vu.state().cycles, expectedCycles, "IADD must wait for its latest nonzero VI input");
                    t.Equals(vu.state().pc, 32u, "The end-bit delay slot must complete");
                }
            }
        });

        tc.Run("DIV and SQRT update the Q register from selected vector components", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, makeVuDiv(1u, 2u, 1u, 2u), kVuUpperNop);              // Q = vf1.y / vf2.z
            writeVuInstructionPair(fx.code, 8u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop);          // WAITQ
            writeVuInstructionPair(fx.code, 16u, makeVuSqrt(3u, 3u), kVuUpperNop);                    // Q = sqrt(abs(vf3.w))
            writeVuInstructionPair(fx.code, 24u, makeVuLowerSpecial(0x3Bu, 0u), kVuUpperNop);         // WAITQ

            VU1Interpreter vu1;
            vu1.state().vf[1][1] = 18.0f;
            vu1.state().vf[2][2] = 3.0f;
            vu1.state().vf[3][3] = 25.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 8u);
            t.Equals(vu1.state().q, 6.0f, "WAITQ should expose DIV after its seven-cycle latency");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(8u),
                     "maxCycles should count FDIV stalls as elapsed VU cycles");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 8u);
            t.Equals(vu1.state().q, 5.0f, "WAITQ should expose SQRT after its seven-cycle latency");
        });

        tc.Run("FDIV resource serializes back-to-back scalar operations", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuDiv(1u, 2u, 0u, 0u),
                kVuUpperNop); // Q = vf1.x / vf2.x
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuSqrt(3u, 0u),
                kVuUpperNop); // Must wait for the shared FDIV unit.
            writeVuInstructionPair(
                fx.code, 16u,
                makeVuLowerSpecial(0x3Bu, 0u),
                kVuUpperNop); // WAITQ

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 18.0f;
            vu1.state().vf[2][0] = 3.0f;
            vu1.state().vf[3][0] = 25.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 15u);

            t.Equals(vu1.state().q, 5.0f,
                     "the second FDIV operation should commit the final Q value");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(15u),
                     "back-to-back FDIV operations should include the resource stall");
        });

        tc.Run("FDIV commits current and sticky divide-invalid status", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, makeVuDiv(1u, 2u, 0u, 0u),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuLowerSpecial(0x3Bu, 0u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 0.0f;
            vu1.state().vf[2][0] = 0.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 8u);
            t.Equals(vu1.state().status, 0x410u,
                     "zero divided by zero should set current and sticky I");

            vu1.reset();
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[2][0] = 0.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 8u);
            t.Equals(vu1.state().status, 0x820u,
                     "a nonzero numerator divided by zero should set current and sticky D");
        });

        tc.Run("EFU WAITP and RNG execute with architectural latency and state", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuLowerSpecial(0x70u, 1u),
                kVuUpperNop); // ESADD P, vf1
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuLowerSpecial(0x7Bu, 0u),
                kVuUpperNop); // WAITP
            writeVuInstructionPair(
                fx.code, 16u,
                makeVuLowerSpecial(0x42u, 2u, 0u, 0u, 0x8u),
                kVuUpperNop); // RINIT R, vf2.x
            writeVuInstructionPair(
                fx.code, 24u,
                makeVuLowerSpecial(0x40u, 0u, 3u, 0u, 0x8u),
                kVuUpperNop); // RNEXT.x vf3

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.state().vf[2][0] = 1.5f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 14u);

            t.Equals(vu1.state().p, 14.0f,
                     "WAITP should expose ESADD after eleven cycles");
            t.Equals(vu1.state().vf[3][0], 0.0f,
                     "RNEXT vector result should respect FMAC writeback latency");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 3u);
            t.IsTrue(vu1.state().vf[3][0] >= 1.0f &&
                         vu1.state().vf[3][0] < 2.0f &&
                         vu1.state().vf[3][0] != 1.5f,
                      "RNEXT should advance the 23-bit R LFSR and write a 1.x value");
        });

        tc.Run("EFU resource observes throughput separately from P visibility", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuLowerSpecial(0x70u, 1u),
                kVuUpperNop); // ESADD: result at cycle 11, resource free at 10.
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuLowerSpecial(0x72u, 1u),
                kVuUpperNop); // ELENG: must issue at cycle 10.
            writeVuInstructionPair(
                fx.code, 16u,
                makeVuLowerSpecial(0x7Bu, 0u),
                kVuUpperNop); // WAITP waits for ELENG at cycle 28.

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][1] = 2.0f;
            vu1.state().vf[1][2] = 3.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 29u);

            t.IsTrue(vu1.state().p > 3.7f && vu1.state().p < 3.8f,
                     "WAITP should expose the second EFU result");
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(29u),
                     "EFU scheduling should use opcode throughput and result latency");
        });

        tc.Run("all EFU opcodes produce P at their architectural latency", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            struct EfuCase
            {
                uint8_t opcode;
                uint32_t latency;
            };
            constexpr EfuCase cases[] = {
                {0x70u, 11u}, {0x71u, 18u}, {0x72u, 18u}, {0x73u, 24u},
                {0x74u, 54u}, {0x75u, 54u}, {0x76u, 12u}, {0x77u, 18u},
                {0x78u, 12u}, {0x79u, 29u}, {0x7Au, 12u}, {0x7Cu, 54u},
                {0x7Du, 44u}};

            for (const EfuCase &efu : cases)
            {
                std::memset(fx.code, 0, PS2_VU1_CODE_SIZE);
                writeVuInstructionPair(
                    fx.code, 0u,
                    makeVuLowerSpecial(efu.opcode, 1u, 0u, 0u, 0u),
                    kVuUpperNop);
                writeVuInstructionPair(
                    fx.code, 8u,
                    makeVuLowerSpecial(0x7Bu, 0u),
                    kVuUpperNop);

                VU1Interpreter vu1;
                vu1.state().vf[1][0] = 0.25f;
                vu1.state().vf[1][1] = 0.5f;
                vu1.state().vf[1][2] = 0.75f;
                vu1.state().vf[1][3] = 1.0f;
                vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                            fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                            0u, 0u, 0u, efu.latency + 1u);

                t.Equals(vu1.state().cycles,
                         static_cast<uint64_t>(efu.latency + 1u),
                         "WAITP should count every EFU stall as an elapsed VU cycle");
                t.IsTrue(std::isfinite(vu1.state().p),
                         "architected EFU opcode should commit a finite P result");
            }
        });

        tc.Run("E and enabled D/T stop with their architectural delay rules", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                kVuUpperNop | 0x40000000u);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIaddiu(1u, 0u, 7),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 9),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 1u);
            t.IsTrue(vu1.isRunning(),
                     "exhausting a cycle slice before E's delay slot should leave VU1 running");
            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 31u);
            t.IsTrue(!vu1.isRunning(),
                     "completing E's delay slot should mark VU1 idle");
            t.Equals(vu1.state().vi[1], 7,
                     "E should execute exactly one sequential delay slot");
            t.Equals(vu1.state().vi[2], 0,
                     "E should stop before the instruction after its delay slot");

            vu1.reset();
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuIaddiu(1u, 0u, 3),
                kVuUpperNop | 0x08000000u);
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIaddiu(2u, 0u, 5),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 2u);
            t.Equals(vu1.state().vi[2], 5,
                     "T must be ignored while TE is disabled");

            vu1.reset();
            vu1.state().tBitEnabled = true;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[1], 3,
                     "T instruction itself should complete");
            t.Equals(vu1.state().vi[2], 0,
                     "enabled T should stop without an ordinary delay slot");
            t.IsTrue(vu1.state().stoppedByT,
                     "the stop reason should identify T");

            vu1.reset();
            vu1.state().dBitEnabled = true;
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuIaddiu(1u, 0u, 4),
                kVuUpperNop | 0x10000000u);
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIaddiu(2u, 0u, 6),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[1], 4,
                     "D instruction itself should complete");
            t.Equals(vu1.state().vi[2], 0,
                     "enabled D should stop without an ordinary delay slot");
            t.IsTrue(vu1.state().stoppedByD,
                     "the stop reason should identify D");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 1u);
            t.Equals(vu1.state().vi[2], 6,
                     "MSCNT-style resume should continue at the stopped TPC");
            t.IsTrue(!vu1.state().stoppedByD && !vu1.state().stoppedByT,
                     "resuming should clear the previous D/T stop reason");

            vu1.reset();
            vu1.state().tBitEnabled = true;
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuBranch(1),
                kVuUpperNop | 0x08000000u);
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIaddiu(2u, 0u, 11),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 16u, makeVuIaddiu(3u, 0u, 13),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 32u);
            t.Equals(vu1.state().vi[2], 11,
                     "T on a branch should still execute its branch delay slot");
            t.Equals(vu1.state().vi[3], 0,
                     "T on a branch should stop before executing the branch target");
            t.Equals(vu1.state().pc, 16u,
                     "the stopped TPC should be the branch destination");
        });

        tc.Run("conditional branch sees the previous VI value for one instruction", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, makeVuIaddiu(1u, 0u, 1),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIbne(1u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[2], 2,
                     "the instruction after a conditional branch remains its delay slot");
            t.Equals(vu1.state().vi[3], 3,
                     "an immediately following branch should see the pre-write VI value");

            vu1.reset();
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 16u, makeVuIbne(1u, 0u, 2),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 40u, makeVuIaddiu(5u, 0u, 5),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);
            t.Equals(vu1.state().vi[3], 3,
                     "taken branch should still execute its delay slot");
            t.Equals(vu1.state().vi[4], 0,
                     "after one intervening instruction the branch should observe and branch on the new VI value");
            t.Equals(vu1.state().vi[5], 5,
                     "taken branch should arrive at its target after the delay slot");

            vu1.reset();
            writeTrackedVuInstructionPair(
                fx, 0u, makeVuIaddiu(1u, 0u, 1),
                makeVuUpper(0x28u, 0x8u, 2u, 1u, 3u));
            writeTrackedVuInstructionPair(
                fx, 8u, makeVuIbne(1u, 0u, 2),
                makeVuUpper(0x28u, 0x8u, 0u, 3u, 4u));
            writeTrackedVuInstructionPair(
                fx, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeTrackedVuInstructionPair(
                fx, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 7u);
            t.Equals(vu1.state().vi[2], 2,
                     "a stalled conditional branch should retain its delay slot");
            t.Equals(vu1.state().vi[3], 3,
                     "the VI bypass must survive VF hazard stalls before the next pair issues");
            t.Equals(vu1.state().vi[4], 0,
                     "elapsed stall cycles must not expire the one-instruction VI bypass");
        });

        tc.Run("flag checks are visible to an immediately following branch", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u,
                makeVuFlagImmediate(0x16u, 1u, 0x001u),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIbne(1u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().status = 0x001u;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[1], 1,
                     "FSAND should publish its result after one cycle");
            t.Equals(vu1.state().vi[2], 2,
                     "the taken branch should still execute its delay slot");
            t.Equals(vu1.state().vi[3], 0,
                     "the immediately following branch must consume the flag-check result");
            t.Equals(vu1.state().vi[4], 4,
                     "the flag-driven branch should arrive at its target");
        });

        tc.Run("JR shares the one-instruction VI branch visibility rule", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, makeVuIaddiu(1u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 8u, makeVuJr(1u),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 16u, makeVuIaddiu(2u, 0u, 2),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 24u, makeVuIaddiu(3u, 0u, 3),
                kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u, makeVuIaddiu(4u, 0u, 4),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 4;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 4u);

            t.Equals(vu1.state().vi[1], 2,
                     "the pending IALU write should still commit normally");
            t.Equals(vu1.state().vi[2], 2,
                     "JR should execute exactly one delay-slot pair");
            t.Equals(vu1.state().vi[3], 0,
                     "JR should skip the sequential instruction after its delay slot");
            t.Equals(vu1.state().vi[4], 4,
                     "JR immediately after a VI write should branch using the previous VI value");
        });

        tc.Run("cached and uncached decode paths observe edited instruction pairs", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            for (uint32_t mode = 0; mode < 4u; ++mode)
            {
                std::array<uint8_t, 32u> localCode{};
                const bool tracked = mode < 2u;
                uint8_t *code = tracked ? fx.code : localCode.data();
                const uint32_t size = tracked ? PS2_VU1_CODE_SIZE : static_cast<uint32_t>(localCode.size());
                const uint32_t start = mode == 1u ? 4u : 0u;
                PS2Memory *memory = mode == 3u ? nullptr : &fx.mem;
                VU1Interpreter vu;
                for (const uint16_t immediate : {1u, 29u, 73u})
                {
                    const uint32_t lower = makeVuIaddiu(1u, 0u, static_cast<int16_t>(immediate));
                    const uint32_t upper = kVuUpperNop | 0x40000000u;
                    if (tracked)
                    {
                        fx.mem.write32(PS2_VU1_CODE_BASE + start, lower);
                        fx.mem.write32(PS2_VU1_CODE_BASE + start + 4u, upper);
                        fx.mem.write32(PS2_VU1_CODE_BASE + start + 8u, 0u);
                        fx.mem.write32(PS2_VU1_CODE_BASE + start + 12u, kVuUpperNop);
                    }
                    else
                    {
                        writeVuInstructionPair(code, start, lower, upper);
                        writeVuInstructionPair(code, start + 8u, 0u, kVuUpperNop);
                    }
                    vu.execute(code, size, fx.data, PS2_VU1_DATA_SIZE,
                               fx.gs, memory, start, 0u, 0u, 16u);
                    t.Equals(vu.state().vi[1], static_cast<int32_t>(immediate), "Each source edit must be decoded");
                    t.Equals(vu.state().pc, start + 16u, "The current pair's end bit and delay slot must survive");
                }
            }
        });

        tc.Run("MPG upload invalidates cached VU1 decode before MSCAL", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            VU1Interpreter vu1;
            fx.mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                vu1.execute(fx.code,
                            PS2_VU1_CODE_SIZE,
                            fx.data,
                            PS2_VU1_DATA_SIZE,
                            fx.gs,
                            &fx.mem,
                            startPC,
                            top,
                            itop,
                            1u);
            });

            uploadVu1Mpg(fx.mem, 0u, makeVuIaddiu(1u, 0u, 1), kVuUpperNop);
            const uint32_t firstMscal = makeVifCmd(0x14u, 0u, 0u);
            fx.mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&firstMscal), sizeof(firstMscal));
            t.Equals(vu1.state().vi[1], 1, "first MSCAL should execute the first uploaded program");

            uploadVu1Mpg(fx.mem, 0u, makeVuIaddiu(1u, 0u, 2), kVuUpperNop);
            const uint32_t secondMscal = makeVifCmd(0x14u, 0u, 0u);
            fx.mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&secondMscal), sizeof(secondMscal));
            t.Equals(vu1.state().vi[1], 2, "second MSCAL should see the MPG-updated instruction");
        });

        tc.Run("direct VU1 code writes invalidate cached decode", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            VU1Interpreter vu1;
            fx.mem.write64(PS2_VU1_CODE_BASE, packVuInstructionPair(makeVuIaddiu(1u, 0u, 1), kVuUpperNop));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
            t.Equals(vu1.state().vi[1], 1, "first execution should use the original direct write");

            fx.mem.write64(PS2_VU1_CODE_BASE, packVuInstructionPair(makeVuIaddiu(1u, 0u, 2), kVuUpperNop));
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE, fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
            t.Equals(vu1.state().vi[1], 2, "second execution should rebuild decode after the direct write");
        });

        tc.Run("XGKICK sends a VU memory GIF packet through PATH1", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *vuCode = mem.getVU1Code();
            uint8_t *vuData = mem.getVU1Data();
            std::memset(vuCode, 0, PS2_VU1_CODE_SIZE);
            std::memset(vuData, 0, PS2_VU1_DATA_SIZE);

            constexpr uint32_t kLastQw = (PS2_VU1_DATA_SIZE / 16u) - 1u;
            const uint32_t tagOffset = kLastQw * 16u;

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(vuData + tagOffset, &imageTag, sizeof(imageTag));

            for (uint32_t i = 0; i < 16u; ++i)
            {
                vuData[i] = static_cast<uint8_t>(0xC0u + i);
            }

            const uint32_t lower = makeVuLowerSpecial(0x6Cu, 1u);
            std::memcpy(vuCode + 0u, &lower, sizeof(lower));
            const uint32_t upper = 0u;
            std::memcpy(vuCode + 4u, &upper, sizeof(upper));

            VU1Interpreter vu1;
            vu1.state().vi[1] = static_cast<int32_t>(kLastQw);
            vu1.execute(vuCode,
                        PS2_VU1_CODE_SIZE,
                        vuData,
                        PS2_VU1_DATA_SIZE,
                        gs,
                        &mem,
                        0u,
                        0u,
                        0u,
                        3u);

            t.Equals(captured.size(), static_cast<size_t>(1u), "XGKICK should emit one wrapped GIF packet");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(32u), "wrapped packet should include tag plus one qword payload");
                bool payloadOk = true;
                for (uint32_t i = 0; i < 16u; ++i)
                {
                    if (captured[0].size() < 32u || captured[0][16u + i] != static_cast<uint8_t>(0xC0u + i))
                    {
                        payloadOk = false;
                        break;
                    }
                }
                t.IsTrue(payloadOk, "wrapped payload should be copied from start of VU1 memory");
            }
        });

        tc.Run("XGKICK preserves qword bytes across arbitrary memory boundaries", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");
            if (!fx.code || !fx.data)
                return;
            writeTrackedVuInstructionPair(fx, 0u, makeVuLowerSpecial(0x6Cu, 1u), kVuUpperNop);
            writeTrackedVuInstructionPair(fx, 8u, 0u, kVuUpperNop);
            writeTrackedVuInstructionPair(fx, 16u, 0u, kVuUpperNop);
            std::vector<uint8_t> expected(32u, 0u), captured;
            const uint64_t tag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(expected.data(), &tag, sizeof(tag));
            for (uint32_t i = 16u; i < 32u; ++i)
                expected[i] = static_cast<uint8_t>(0xA0u + i);
            fx.mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t size)
            {
                captured.assign(packet, packet + size);
            });
            for (const uint32_t size : {32u, 33u, 47u, 64u, uint32_t{PS2_VU1_DATA_SIZE}})
            {
                const uint32_t source = ((size - 1u) / 16u) * 16u;
                std::vector<uint8_t> data(size + 16u, 0xCDu);
                for (uint32_t i = 0; i < expected.size(); ++i)
                    data[(source + i) % size] = expected[i];
                VU1Interpreter vu;
                vu.state().vi[1] = static_cast<int32_t>(source / 16u);
                captured.clear();
                vu.execute(fx.code, PS2_VU1_CODE_SIZE, data.data(), size,
                           fx.gs, &fx.mem, 0u, 0u, 0u, 1u);
                t.IsTrue(captured.empty(), "The first cycle must transfer only the tag");
                vu.resume(fx.code, PS2_VU1_CODE_SIZE, data.data(), size,
                          fx.gs, &fx.mem, 0u, 0u, 0u);
                t.Equals(vu.state().cycles, uint64_t{1u}, "A zero-cycle resume must not advance PATH1");
                t.IsTrue(captured.empty(), "A zero-cycle resume must not emit the packet");
                vu.resume(fx.code, PS2_VU1_CODE_SIZE, data.data(), size,
                          fx.gs, &fx.mem, 0u, 0u, 1u);
                t.IsTrue(captured.empty(), "The payload must still be pending at cycle two");
                vu.resume(fx.code, PS2_VU1_CODE_SIZE, data.data(), size,
                          fx.gs, &fx.mem, 0u, 0u, 1u);
                t.Equals(vu.state().cycles, uint64_t{3u}, "The payload must finish at cycle three");
                t.IsTrue(captured == expected, "Every wrapped tag and payload byte must match");
            }
        });

        tc.Run("XGKICK accepts IMAGE2 GIF tags", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes)
            {
                captured.emplace_back(packet, packet + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);

            const uint64_t image2Tag = makeGifTag(1u, GIF_FMT_IMAGE2, 0u, true);
            std::memcpy(data, &image2Tag, sizeof(image2Tag));
            std::memset(data + 16u, 0x5Au, 16u);
            writeVuInstructionPair(
                code, 0u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 3u);

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "IMAGE2 should complete one PATH1 packet");
            if (!captured.empty())
            {
                t.Equals(captured[0].size(), static_cast<size_t>(32u),
                         "IMAGE2 PATH1 packet should include its image payload");
            }
        });

        tc.Run("XGKICK observes stores committed before a future PATH1 qword", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes)
            {
                captured.emplace_back(packet, packet + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0xFF, PS2_VU1_DATA_SIZE);

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memset(data, 0, 16u);
            std::memcpy(data, &imageTag, sizeof(imageTag));
            writeVuInstructionPair(
                code, 0u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);
            writeVuInstructionPair(
                code, 8u,
                makeVuSq(0xFu, 4u, 2u, 0),
                kVuUpperNop);
            writeVuInstructionPair(code, 16u, 0u, kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 1;
            const float replacement[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::memcpy(vu1.state().vf[4], replacement, sizeof(replacement));
            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 3u);

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "PATH1 should finish after consuming the updated payload");
            if (!captured.empty())
            {
                t.IsTrue(captured[0].size() >= 32u &&
                             std::memcmp(captured[0].data() + 16u,
                                         replacement,
                                         sizeof(replacement)) == 0,
                         "XGKICK must read each qword in its transfer cycle");
            }
        });

        tc.Run("a second XGKICK stalls until the active PATH1 transfer completes", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *packet, uint32_t sizeBytes)
            {
                captured.emplace_back(packet, packet + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(data + 0u, &imageTag, sizeof(imageTag));
            std::memcpy(data + 32u, &imageTag, sizeof(imageTag));
            std::memset(data + 16u, 0x11, 16u);
            std::memset(data + 48u, 0x22, 16u);
            writeVuInstructionPair(
                code, 0u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);
            writeVuInstructionPair(
                code, 8u,
                makeVuLowerSpecial(0x6Cu, 2u),
                kVuUpperNop);
            writeVuInstructionPair(code, 16u, 0u, kVuUpperNop);
            writeVuInstructionPair(code, 24u, 0u, kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 2;
            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 6u);

            t.Equals(captured.size(), static_cast<size_t>(2u),
                     "both PATH1 transfers should complete in issue order");
            if (captured.size() == 2u)
            {
                t.IsTrue(captured[0].size() >= 32u && captured[0][16u] == 0x11u,
                         "the first XGKICK payload should be delivered first");
                t.IsTrue(captured[1].size() >= 32u && captured[1][16u] == 0x22u,
                         "the stalled XGKICK should retain its own source address");
            }
            t.Equals(vu1.state().cycles, static_cast<uint64_t>(6u),
                     "XGKICK resource stalls should consume VU cycles");
        });

        tc.Run("synthetic Code Veronica text packet preserves black-frame PATH1 data", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            std::vector<std::vector<uint8_t>> captured;
            mem.setGifPacketCallback([&](const uint8_t *data, uint32_t sizeBytes)
            {
                captured.emplace_back(data, data + sizeBytes);
            });

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            uint8_t *code = mem.getVU1Code();
            uint8_t *data = mem.getVU1Data();
            std::memset(code, 0, PS2_VU1_CODE_SIZE);
            std::memset(data, 0, PS2_VU1_DATA_SIZE);

            const uint64_t imageTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(data, &imageTag, sizeof(imageTag));
            writeVuInstructionPair(
                code, 0u, 0u,
                makeVuUpper(0x28u, 0xFu, 3u, 2u, 4u));
            writeVuInstructionPair(
                code, 8u,
                makeVuSq(0xFu, 4u, 2u, 0),
                kVuUpperNop);
            writeVuInstructionPair(
                code, 16u,
                makeVuLowerSpecial(0x6Cu, 1u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vi[1] = 0;
            vu1.state().vi[2] = 1;
            const float a[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            const float b[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::memcpy(vu1.state().vf[2], a, sizeof(a));
            std::memcpy(vu1.state().vf[3], b, sizeof(b));

            vu1.execute(code, PS2_VU1_CODE_SIZE,
                        data, PS2_VU1_DATA_SIZE, gs, &mem,
                        0u, 0u, 0u, 10u);

            t.Equals(captured.size(), static_cast<size_t>(1u),
                     "the synthetic scene should emit exactly one PATH1 packet");
            if (!captured.empty())
            {
                const float expected[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                t.Equals(captured[0].size(), static_cast<size_t>(32u),
                         "text regression packet should contain one image qword");
                t.IsTrue(captured[0].size() >= 32u &&
                             std::memcmp(captured[0].data() + 16u,
                                         expected,
                                         sizeof(expected)) == 0,
                         "PATH1 must observe the post-FMAC store, not stale blue/magenta data");
            }
        });

        tc.Run("MSCAL can start a VU1 XGKICK program and update GS VRAM", [](TestCase &t)
        {
            PS2Memory mem;
            t.IsTrue(mem.initialize(), "PS2Memory initialize should succeed");

            GS gs;
            gs.init(mem.getGSVRAM(), static_cast<uint32_t>(PS2_GS_VRAM_SIZE), &mem.gs());
            GifArbiter arbiter([&](const uint8_t *data, uint32_t sizeBytes)
            {
                gs.processGIFPacket(data, sizeBytes);
            });
            mem.setGifArbiter(&arbiter);

            const uint64_t bitblt =
                (static_cast<uint64_t>(0u) << 0) |
                (static_cast<uint64_t>(1u) << 16) |
                (static_cast<uint64_t>(0u) << 24) |
                (static_cast<uint64_t>(0u) << 32) |
                (static_cast<uint64_t>(1u) << 48) |
                (static_cast<uint64_t>(0u) << 56);
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (1ull << 32));
            gs.writeRegister(GS_REG_TRXDIR, 0ull);

            uint8_t *vuCode = mem.getVU1Code();
            uint8_t *vuData = mem.getVU1Data();
            std::memset(vuCode, 0, PS2_VU1_CODE_SIZE);
            std::memset(vuData, 0, PS2_VU1_DATA_SIZE);

            const uint32_t lower = makeVuLowerSpecial(0x6Cu, 0u);
            std::memcpy(vuCode + 0u, &lower, sizeof(lower));
            const uint32_t upper = 0u;
            std::memcpy(vuCode + 4u, &upper, sizeof(upper));

            const uint64_t gifTag = makeGifTag(1u, GIF_FMT_IMAGE, 0u, true);
            std::memcpy(vuData + 0u, &gifTag, sizeof(gifTag));
            const uint64_t tagHi = 0u;
            std::memcpy(vuData + 8u, &tagHi, sizeof(tagHi));
            for (uint32_t i = 0; i < 16u; ++i)
            {
                vuData[16u + i] = static_cast<uint8_t>(0x90u + i);
            }

            VU1Interpreter vu1;
            mem.setVu1MscalCallback([&](uint32_t startPC, uint32_t top, uint32_t itop)
            {
                vu1.execute(vuCode,
                            PS2_VU1_CODE_SIZE,
                            vuData,
                            PS2_VU1_DATA_SIZE,
                            gs,
                            &mem,
                            startPC,
                            top,
                            itop,
                            3u);
            });

            const uint32_t mscalCmd = makeVifCmd(0x14u, 0u, 0u);
            mem.processVIF1Data(reinterpret_cast<const uint8_t *>(&mscalCmd), sizeof(mscalCmd));

            const uint8_t *vramOut = mem.getGSVRAM();
            bool imageOk = true;
            for (uint32_t x = 0; x < 4u && imageOk; ++x)
            {
                const uint32_t off = GSPSMCT32::addrPSMCT32(0u, 1u, x, 0u);
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vramOut[off + c] != static_cast<uint8_t>(0x90u + x * 4u + c))
                    {
                        imageOk = false;
                        break;
                    }
                }
            }
            t.IsTrue(imageOk, "MSCAL-triggered XGKICK should route PATH1 packet into GS VRAM");
        });

        tc.Run("standalone VU1 code honors the nullable PS2Memory API", [](TestCase &t)
        {
            std::vector<uint8_t> code(8u, 0u);
            std::vector<uint8_t> data(16u, 0u);
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            VU1Interpreter vu1;
            vu1.execute(code.data(), static_cast<uint32_t>(code.size()),
                        data.data(), static_cast<uint32_t>(data.size()),
                        gs, nullptr, 0u, 0u, 0u, 1u);

            t.Equals(vu1.state().pc, 0u,
                     "external code should execute and wrap without dereferencing a null memory tracker");
        });

        tc.Run("VU1 status-immediate ops decode IMM12 and their target VI", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u,
                                   makeVuFlagImmediate(0x14u, 5u, 0x812u),
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 8u,
                                   makeVuFlagImmediate(0x16u, 6u, 0x810u),
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 16u,
                                   makeVuFlagImmediate(0x17u, 7u, 0x040u),
                                   kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().status = 0x812u;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 3u);

            t.Equals(vu1.state().vi[5], 1,
                     "FSEQ should compare all 12 immediate bits and write IT");
            t.Equals(vu1.state().vi[6], 0x810,
                     "FSAND should return the masked 12-bit status in IT");
            t.Equals(vu1.state().vi[7], 0x852,
                     "FSOR should return the 12-bit OR value rather than a boolean");
            t.Equals(vu1.state().vi[1], 0,
                     "status-immediate ops should not hardcode VI1");
        });

        tc.Run("VU1 FSSET enters the four-cycle flag pipeline", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u,
                                   makeVuFlagImmediate(0x15u, 0u, 0xA80u),
                                   kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().status = 0x015u;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 3u);
            t.Equals(vu1.state().status, 0x015u,
                     "FSSET should not be visible before four cycles elapse");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 1u);
            t.Equals(vu1.state().status, 0xA95u,
                     "FSSET should replace sticky bits while preserving current and D/I bits");
        });

        tc.Run("VU1 has one flag pipeline and FSSET wins a same-pair conflict", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u,
                                   makeVuFlagImmediate(0x15u, 0u, 0xA80u),
                                   makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u));
            for (uint32_t pc = 8u; pc <= 32u; pc += 8u)
                writeVuInstructionPair(fx.code, pc, 0u, kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][2] = -3.0f;
            vu1.state().vf[2][0] = -1.0f;
            vu1.state().vf[2][2] = 1.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().mac, 0x28u,
                     "same-pair FSSET should suppress only STATUS, not the upper MAC result");
            t.Equals(vu1.state().status, 0xA80u,
                     "the single runtime pipeline should commit FSSET sticky bits");
        });

        tc.Run("VU1 FMAC flags respect destination lanes and become visible after four cycles", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u,
                                   makeVuUpper(0x28u, 0xAu, 2u, 1u, 3u));
            writeVuInstructionPair(fx.code, 8u,
                                   makeVuFlagRegister(0x18u, 6u, 7u),
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 16u,
                                   0u,
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 24u,
                                   0u,
                                   kVuUpperNop);
            writeVuInstructionPair(fx.code, 32u,
                                   makeVuFlagRegister(0x1Au, 4u, 5u),
                                   kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 1.0f;
            vu1.state().vf[1][2] = -3.0f;
            vu1.state().vf[2][0] = -1.0f;
            vu1.state().vf[2][2] = 1.0f;
            vu1.state().vi[5] = 0xFFFF;
            vu1.state().vi[7] = 0;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 3u);

            t.Equals(vu1.state().mac, 0u,
                     "FMAC flags should remain hidden before four cycles elapse");
            t.Equals(vu1.state().vi[6], 1,
                     "FMEQ before the commit cycle should observe the old MAC flags");

            vu1.resume(fx.code, PS2_VU1_CODE_SIZE,
                       fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                       0u, 0u, 2u);

            t.Equals(vu1.state().mac, 0x28u,
                     "ADD.xz should report zero on x and sign on z only");
            t.Equals(vu1.state().status, 0xC3u,
                     "FMAC commit should update current Z/S and accumulate their sticky bits");
            t.Equals(vu1.state().vi[4], 0x28,
                      "FMAND on the commit cycle should observe the new MAC flags");
        });

        tc.Run("VU1 CLIP and FCGET share the four-cycle flag timeline", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpperSpecial(0x1Fu, 0u, 2u, 1u));
            writeVuInstructionPair(
                fx.code, 8u,
                makeVuFlagRegister(0x1Cu, 3u, 0u),
                kVuUpperNop);
            writeVuInstructionPair(fx.code, 16u, 0u, kVuUpperNop);
            writeVuInstructionPair(fx.code, 24u, 0u, kVuUpperNop);
            writeVuInstructionPair(
                fx.code, 32u,
                makeVuFlagRegister(0x1Cu, 4u, 0u),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = 2.0f;
            vu1.state().vf[2][3] = 1.0f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vi[3], 0,
                     "FCGET before cycle four should observe the previous CLIP value");
            t.Equals(vu1.state().vi[4], 1,
                     "FCGET on cycle four should observe the committed +X CLIP bit");
            t.Equals(vu1.state().clip, 1u,
                     "CLIP should shift and commit the six new comparison bits");
        });

        tc.Run("VU1 FMAC normalizes overflow and underflow before writing results", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u,
                                   makeVuUpper(0x2Au, 0xFu, 2u, 1u, 3u));

            VU1Interpreter vu1;
            vu1.state().vf[1][0] = std::numeric_limits<float>::max();
            vu1.state().vf[1][1] = std::numeric_limits<float>::min();
            vu1.state().vf[1][2] = -2.0f;
            vu1.state().vf[1][3] = 0.0f;
            vu1.state().vf[2][0] = 2.0f;
            vu1.state().vf[2][1] = 0.5f;
            vu1.state().vf[2][2] = 1.0f;
            vu1.state().vf[2][3] = 1.0f;

            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().mac, 0x8425u,
                     "MUL should report x overflow, y underflow+zero, z sign and w zero");
            t.Equals(vu1.state().status, 0x3CFu,
                     "current and sticky status should summarize Z/S/U/O");
            t.Equals(vu1.state().vf[3][0], std::numeric_limits<float>::max(),
                     "overflow should clamp to the largest finite VU value");
            t.Equals(vu1.state().vf[3][1], 0.0f,
                     "underflow should flush to signed zero before writeback");
        });

        tc.Run("FMAC product contributes Z/S/U/O sticky flags", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(
                fx.code, 0u, 0u,
                makeVuUpper(0x29u, 0x8u, 2u, 1u, 3u)); // MADD.x

            VU1Interpreter vu1;
            vu1.state().acc[0] = 1.0f;
            vu1.state().vf[1][0] = std::numeric_limits<float>::min();
            vu1.state().vf[2][0] = 0.5f;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 5u);

            t.Equals(vu1.state().vf[3][0], 1.0f,
                     "the accumulated FMAC result should remain normal");
            t.Equals(vu1.state().mac, 0u,
                     "MAC flags should describe the final accumulated value");
            t.Equals(vu1.state().status, 0x140u,
                     "the underflowing product should set sticky Z and U");
        });

        tc.Run("reserved opcodes stop before executing or corrupting state", [](TestCase &t)
        {
            Vu1Fixture fx;
            t.IsTrue(fx.initialize(), "VU1 fixture should initialize");

            writeVuInstructionPair(fx.code, 0u, 0u, 0x30u);
            writeVuInstructionPair(
                fx.code, 8u, makeVuIaddiu(1u, 0u, 7),
                kVuUpperNop);

            VU1Interpreter vu1;
            vu1.execute(fx.code, PS2_VU1_CODE_SIZE,
                        fx.data, PS2_VU1_DATA_SIZE, fx.gs, &fx.mem,
                        0u, 0u, 0u, 8u);

            t.Equals(vu1.state().cycles, static_cast<uint64_t>(0u),
                     "reserved opcode should stop before consuming its issue cycle");
            t.Equals(vu1.state().pc, 0u,
                     "reserved opcode should retain the diagnostic PC");
            t.Equals(vu1.state().vi[1], 0,
                     "instruction following a reserved opcode must not execute");
        });
    });
}
