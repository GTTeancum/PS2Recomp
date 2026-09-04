#include "runtime/ps2_vu1_replay.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace
{
    constexpr uint32_t kMagic = 0x31525556u; // VUR1, little endian.
    constexpr size_t kMaxRecord = 1024u * 1024u;
    constexpr size_t kMaxFile = 4u * kMaxRecord;
    constexpr size_t kMaxGifs = kMaxRecord / 2u;

    struct Writer
    {
        std::vector<uint8_t> bytes;
        template <class T> void value(T &v)
        {
            if constexpr (std::is_same_v<T, bool>)
                bytes.push_back(v ? 1u : 0u);
            else if constexpr (std::is_same_v<T, float>)
            {
                auto bits = std::bit_cast<uint32_t>(v);
                value(bits);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                auto bits = static_cast<std::underlying_type_t<T>>(v);
                value(bits);
            }
            else if constexpr (std::is_integral_v<T>)
            {
                auto bits = std::bit_cast<std::make_unsigned_t<T>>(v);
                for (size_t i = 0; i < sizeof(T); ++i)
                    bytes.push_back(static_cast<uint8_t>(bits >> (i * 8u)));
            }
            else
                for (auto &element : v)
                    value(element);
        }
        template <class... T> void operator()(T &...v) { (value(v), ...); }
        void raw(uint8_t *data, size_t size)
        {
            if (size == 0u)
                return;
            if (size > kMaxRecord || bytes.size() > kMaxRecord - size)
                throw std::runtime_error("VU replay record exceeds its limit");
            bytes.insert(bytes.end(), data, data + size);
        }
        void blob(std::vector<uint8_t> &data)
        {
            auto size = static_cast<uint32_t>(data.size());
            value(size);
            raw(data.data(), size);
        }
    };

    struct Reader
    {
        const std::vector<uint8_t> &bytes;
        size_t offset = 0;
        uint8_t byte()
        {
            if (offset == bytes.size())
                throw std::runtime_error("Truncated VU replay record");
            return bytes[offset++];
        }
        template <class T> void value(T &v)
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                const auto bits = byte();
                if (bits > 1u)
                    throw std::runtime_error("Invalid VU replay boolean");
                v = bits != 0u;
            }
            else if constexpr (std::is_same_v<T, float>)
            {
                uint32_t bits = 0;
                value(bits);
                v = std::bit_cast<float>(bits);
            }
            else if constexpr (std::is_enum_v<T>)
            {
                std::underlying_type_t<T> bits{};
                value(bits);
                v = static_cast<T>(bits);
            }
            else if constexpr (std::is_integral_v<T>)
            {
                std::make_unsigned_t<T> bits = 0;
                for (size_t i = 0; i < sizeof(T); ++i)
                    bits |= static_cast<std::make_unsigned_t<T>>(byte()) << (i * 8u);
                v = std::bit_cast<T>(bits);
            }
            else
                for (auto &element : v)
                    value(element);
        }
        template <class... T> void operator()(T &...v) { (value(v), ...); }
        void raw(uint8_t *data, size_t size)
        {
            if (size > bytes.size() - offset)
                throw std::runtime_error("Truncated VU replay payload");
            if (size != 0u)
                std::memcpy(data, bytes.data() + offset, size);
            offset += size;
        }
        void blob(std::vector<uint8_t> &data)
        {
            uint32_t size = 0;
            value(size);
            if (size > kMaxRecord || size > bytes.size() - offset)
                throw std::runtime_error("Invalid VU replay payload size");
            data.resize(size);
            raw(data.data(), size);
        }
        void finish() const
        {
            if (offset != bytes.size())
                throw std::runtime_error("Unexpected trailing VU replay data");
        }
    };

    struct Context
    {
        Writer gifs;
        bool replay = false;
        bool overflow = false;
    };
    thread_local Context *activeContext = nullptr;
    struct ContextScope
    {
        Context *previous;
        explicit ContextScope(Context &context) : previous(activeContext) { activeContext = &context; }
        ~ContextScope() { activeContext = previous; }
    };

    struct Record
    {
        uint32_t maxCycles = 0;
        uint64_t tick = 0;
        std::vector<uint8_t> code, data, before, afterData, after, gifs;
        template <class Archive> void visit(Archive &archive)
        {
            archive(maxCycles, tick);
            archive.blob(code);
            archive.blob(data);
            archive.blob(before);
            archive.blob(afterData);
            archive.blob(after);
            archive.blob(gifs);
        }
    };

    void require(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

template <class Archive> void VUReplay::visitState(Archive &a, VU1Interpreter &v)
{
    a(v.m_unit);
    auto &s = v.m_state;
    a(s.vf, s.vi, s.acc, s.q, s.p, s.i, s.r, s.pc, s.mac, s.clip, s.status, s.cycles,
      s.ebit, s.haltAfterDelaySlot, s.dBitEnabled, s.tBitEnabled, s.stoppedByD, s.stoppedByT,
      s.top, s.itop, s.branchPending, s.branchTarget, s.branchDelay);
    for (auto &e : v.m_flagPipeline)
        a(e.readyCycle, e.issueCycle, e.mac, e.status, e.extraSticky, e.clip,
          e.valid, e.writesMac, e.writesStatus, e.writesSticky, e.writesClip);
    auto scalar = [&](auto &e) { a(e.readyCycle, e.value, e.statusDi, e.valid); };
    scalar(v.m_fdiv);
    for (auto &e : v.m_efu)
        scalar(e);
    for (auto &e : v.m_storePipeline)
        a(e.readyCycle, e.address, e.words, e.laneMask, e.valid);
    for (auto &e : v.m_vfWritePipeline)
        a(e.readyCycle, e.sequence, e.value, e.reg, e.laneMask, e.valid);
    for (auto &e : v.m_viWritePipeline)
        a(e.readyCycle, e.sequence, e.value, e.reg, e.valid);
    for (auto &e : v.m_accWritePipeline)
        a(e.readyCycle, e.sequence, e.value, e.laneMask, e.valid);
    a(v.m_flagPipelineMask, v.m_storePipelineMask, v.m_vfWritePipelineMask,
      v.m_viWritePipelineMask, v.m_accWritePipelineMask);
    auto &x = v.m_xgkick;
    a(x.active);
    if (x.active)
    {
        a(x.sourceAddress, x.totalBytes, x.copiedBytes, x.currentTagEnd,
          x.cycleCredit, x.issueCycle, x.currentTagEop);
        require(x.copiedBytes <= x.packet.size(), "Invalid VU replay PATH1 prefix size");
        a.raw(x.packet.data(), x.copiedBytes);
    }
    a(v.m_vfReady, v.m_viReady, v.m_accReady, v.m_vfLatestWrite, v.m_viLatestWrite,
      v.m_accLatestWrite, v.m_cycle, v.m_nextWriteSequence, v.m_efuResourceReady,
      v.m_workingClip, v.m_currentUpperInstruction, v.m_viBranchBackupValue,
      v.m_viBranchBackupReg, v.m_viBranchBackupValid, v.m_stopRequested,
      v.m_running, v.m_pendingHaltD, v.m_pendingHaltT);
}

std::vector<uint8_t> VUReplay::saveState(VU1Interpreter &vu)
{
    Writer writer;
    writer.bytes.reserve(8192u);
    visitState(writer, vu);
    return std::move(writer.bytes);
}

void VUReplay::loadState(const std::vector<uint8_t> &bytes, VU1Interpreter &vu)
{
    Reader reader{bytes};
    vu.m_xgkick = {};
    visitState(reader, vu);
    reader.finish();
    require(vu.m_unit == VU1Interpreter::Unit::VU1, "Only VU1 replay is supported");
    require(vu.m_state.pc <= PS2_VU1_CODE_SIZE && vu.m_cycle < (1ull << 56u),
            "Invalid VU replay PC or cycle");
    require(vu.m_viBranchBackupReg < 16u, "Invalid VU replay branch register");
    const auto limit = vu.m_cycle + 65536u;
    auto checkQueue = [&](const auto &entries, uint32_t mask)
    {
        uint32_t actual = 0;
        for (uint32_t index = 0; index < entries.size(); ++index)
        {
            const auto &e = entries[index];
            if (e.valid)
            {
                actual |= 1u << index;
                require(e.readyCycle <= limit, "Invalid VU replay pipeline deadline");
            }
        }
        require(actual == mask, "Invalid VU replay pipeline mask");
    };
    checkQueue(vu.m_flagPipeline, vu.m_flagPipelineMask);
    checkQueue(vu.m_storePipeline, vu.m_storePipelineMask);
    checkQueue(vu.m_vfWritePipeline, vu.m_vfWritePipelineMask);
    checkQueue(vu.m_viWritePipeline, vu.m_viWritePipelineMask);
    checkQueue(vu.m_accWritePipeline, vu.m_accWritePipelineMask);
    for (const auto &e : vu.m_storePipeline)
        require(!e.valid || (e.address <= PS2_VU1_DATA_SIZE - 16u && e.laneMask <= 15u),
                "Invalid VU replay store");
    for (const auto &e : vu.m_vfWritePipeline)
        require(!e.valid || (e.reg < 32u && e.laneMask <= 15u), "Invalid VU replay VF write");
    for (const auto &e : vu.m_viWritePipeline)
        require(!e.valid || e.reg < 16u, "Invalid VU replay VI write");
    for (const auto &e : vu.m_accWritePipeline)
        require(!e.valid || e.laneMask <= 15u, "Invalid VU replay ACC write");
    require(!vu.m_fdiv.valid || vu.m_fdiv.readyCycle <= limit, "Invalid VU replay Q deadline");
    for (const auto &e : vu.m_efu)
        require(!e.valid || e.readyCycle <= limit, "Invalid VU replay P deadline");
    require(vu.m_efuResourceReady <= limit, "Invalid VU replay EFU deadline");
    for (const auto &reg : vu.m_vfReady)
        for (const auto deadline : reg)
            require(deadline <= limit, "Invalid VU replay VF readiness");
    for (const auto deadline : vu.m_viReady)
        require(deadline <= limit, "Invalid VU replay VI readiness");
    for (const auto deadline : vu.m_accReady)
        require(deadline <= limit, "Invalid VU replay ACC readiness");
    require(!vu.m_xgkick.active ||
            (vu.m_xgkick.totalBytes <= vu.m_xgkick.packet.size() &&
             vu.m_xgkick.currentTagEnd <= vu.m_xgkick.packet.size() &&
             (vu.m_xgkick.copiedBytes & 15u) == 0u && vu.m_xgkick.cycleCredit <= 2u),
            "Invalid VU replay PATH1 state");
    vu.m_activeVuData = nullptr;
    vu.m_activeVuDataSize = 0;
    vu.m_activeGs = nullptr;
    vu.m_activeMemory = nullptr;
    // Code caches stay keyed to their local PS2Memory generation, never imported pointers.
}

bool VUReplay::captureRequested()
{
    static const bool requested = std::getenv("PS2X_VU_REPLAY_CAPTURE") != nullptr;
    return requested && activeContext == nullptr;
}

bool VUReplay::observeGif(const uint8_t *packet, uint32_t bytes, uint64_t cycle)
{
    if (!activeContext)
        return false;
    auto &context = *activeContext;
    if (bytes > kMaxGifs || context.gifs.bytes.size() + bytes + 12u > kMaxGifs)
        context.overflow = true;
    if (!context.overflow)
    {
        context.gifs(cycle, bytes);
        context.gifs.bytes.insert(context.gifs.bytes.end(), packet, packet + bytes);
    }
    return context.replay;
}

bool VUReplay::record(std::ostream &output, VU1Interpreter &vu,
                      uint8_t *code, uint8_t *data, GS &gs,
                      PS2Memory *memory, uint32_t maxCycles)
{
    require(vu.m_unit == VU1Interpreter::Unit::VU1 && code && data && maxCycles <= (1u << 20u),
            "Invalid VU replay capture arguments");
    Record record;
    record.maxCycles = maxCycles;
    record.tick = memory ? memory->gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
    record.code.assign(code, code + PS2_VU1_CODE_SIZE);
    record.data.assign(data, data + PS2_VU1_DATA_SIZE);
    record.before = saveState(vu);
    Context context;
    {
        ContextScope scope(context);
        vu.run(code, PS2_VU1_CODE_SIZE, data, PS2_VU1_DATA_SIZE, gs, memory, maxCycles);
    }
    if (context.overflow)
        return false;
    record.gifs = std::move(context.gifs.bytes);
    record.after = saveState(vu);
    record.afterData.assign(data, data + PS2_VU1_DATA_SIZE);
    Writer body;
    record.visit(body);
    const auto position = output.tellp();
    if (position < 0 || static_cast<uint64_t>(position) + body.bytes.size() + 8u > kMaxFile)
        return false;
    Writer header;
    auto magic = kMagic;
    auto size = static_cast<uint32_t>(body.bytes.size());
    header(magic, size);
    output.write(reinterpret_cast<const char *>(header.bytes.data()), header.bytes.size());
    output.write(reinterpret_cast<const char *>(body.bytes.data()), body.bytes.size());
    output.flush();
    return output.good();
}

bool VUReplay::captureSlice(VU1Interpreter &vu, uint8_t *code, uint32_t codeSize,
                            uint8_t *data, uint32_t dataSize, GS &gs,
                            PS2Memory *memory, uint32_t maxCycles)
{
    if (activeContext || vu.m_unit != VU1Interpreter::Unit::VU1 || !memory ||
        codeSize != PS2_VU1_CODE_SIZE || dataSize != PS2_VU1_DATA_SIZE || maxCycles > (1u << 20u))
        return false;
    static std::ofstream output;
    static uint32_t shortCases = 0, longCases = 0, random = 0x57C019ABu;
    static bool finished = false;
    if (finished || memory->gs().vsyncTick.load(std::memory_order_relaxed) < 1100u)
        return false;
    random ^= random << 13u;
    random ^= random >> 17u;
    random ^= random << 5u;
    auto &count = maxCycles > 64u ? longCases : shortCases;
    if (count >= 16u || (maxCycles <= 64u && (random & 63u) != 0u))
        return false;
    if (!output.is_open())
    {
        output.open(std::getenv("PS2X_VU_REPLAY_CAPTURE"), std::ios::binary | std::ios::trunc);
        if (!output)
        {
            finished = true;
            std::fprintf(stderr, "[vu-replay:capture] cannot open output\n");
            return false;
        }
    }
    const auto pc = vu.m_state.pc;
    const auto cycle = vu.m_cycle;
    const bool saved = record(output, vu, code, data, gs, memory, maxCycles);
    ++count;
    std::fprintf(stderr, "[vu-replay:capture] short=%u long=%u pc=0x%x budget=%u cycles=%llu saved=%u bytes=%lld\n",
                 shortCases, longCases, pc, maxCycles,
                 static_cast<unsigned long long>(vu.m_cycle - cycle), saved ? 1u : 0u,
                 static_cast<long long>(output.tellp()));
    if (!saved || (shortCases == 16u && longCases == 16u))
    {
        finished = true;
        output.close();
    }
    return true;
}

VUReplay::Result VUReplay::replay(std::istream &input, uint32_t repeats,
                                 std::vector<UpperSample> *upperSamples,
                                 std::vector<PairSample> *pairSamples
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
                                 , VU1Interpreter::UpperLookup upperLookup
#endif
                                 )
{
    Result result;
    std::map<uint32_t, uint64_t> fetches;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint64_t> executedPairs;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> executedEdges;
    std::set<uint32_t> entryPcs;
    if (upperSamples)
        upperSamples->clear();
    if (pairSamples)
        pairSamples->clear();
    try
    {
        require(repeats > 0u && repeats <= 4096u, "Invalid VU replay repetition count");
        auto memory = std::make_unique<PS2Memory>();
        require(memory->initialize(), "Cannot initialize VU replay memory");
        GS gs;
        gs.init(memory->getGSVRAM(), PS2_GS_VRAM_SIZE, &memory->gs());
        auto vu = std::make_unique<VU1Interpreter>();
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
        vu->setUpperLookup(upperLookup);
#endif
#if defined(PS2X_ENABLE_VU_NATIVE_PAIRS)
        vu->setNativePairsEnabled(std::getenv("PS2X_VU_REPLAY_PAIRS") != nullptr);
#endif
        size_t totalBytes = 0;
        uint64_t totalCycles = 0;
        while (input.peek() != std::char_traits<char>::eof())
        {
            require(result.cases < 64u, "Too many VU replay records");
            std::vector<uint8_t> header(8u);
            require(static_cast<bool>(input.read(reinterpret_cast<char *>(header.data()), header.size())),
                    "Truncated VU replay header");
            Reader prefix{header};
            uint32_t magic = 0, size = 0;
            prefix(magic, size);
            require(magic == kMagic && size <= kMaxRecord, "Invalid VU replay header");
            totalBytes += size + header.size();
            require(totalBytes <= kMaxFile, "VU replay file exceeds its limit");
            std::vector<uint8_t> bytes(size);
            require(static_cast<bool>(input.read(reinterpret_cast<char *>(bytes.data()), size)),
                    "Truncated VU replay record");
            Reader reader{bytes};
            Record record;
            record.visit(reader);
            reader.finish();
            require(record.code.size() == PS2_VU1_CODE_SIZE && record.data.size() == PS2_VU1_DATA_SIZE &&
                    record.afterData.size() == PS2_VU1_DATA_SIZE && record.maxCycles <= (1u << 20u) &&
                    record.gifs.size() <= kMaxGifs, "Invalid VU replay case dimensions");
            loadState(record.before, *vu);
            const auto initialCycle = vu->m_cycle;
            const auto initialPc = vu->m_state.pc;
            if (pairSamples)
                entryPcs.insert(initialPc);
            loadState(record.after, *vu);
            require(vu->m_cycle >= initialCycle, "Invalid VU replay cycle ordering");
            const auto elapsedCycles = vu->m_cycle - initialCycle;
            require(elapsedCycles <= (1u << 21u), "Invalid VU replay elapsed cycles");
            totalCycles += elapsedCycles * (static_cast<uint64_t>(repeats) + 1u);
            require(totalCycles <= 100000000u, "VU replay exceeds its execution budget");
            for (uint32_t offset = 0; offset < PS2_VU1_CODE_SIZE; offset += 8u)
            {
                uint64_t pair = 0;
                std::memcpy(&pair, record.code.data() + offset, sizeof(pair));
                memory->write64(PS2_VU1_CODE_BASE + offset, pair);
            }
            memory->gs().vsyncTick.store(record.tick, std::memory_order_relaxed);
            uint64_t caseNs = 0;
            for (uint32_t iteration = 0; iteration <= repeats; ++iteration)
            {
                loadState(record.before, *vu);
                std::memcpy(memory->getVU1Data(), record.data.data(), record.data.size());
                Context context;
                context.replay = true;
                context.gifs.bytes.reserve(record.gifs.size());
                const auto start = std::chrono::steady_clock::now();
                {
                    ContextScope scope(context);
                    if ((upperSamples || pairSamples) && iteration == 0u && elapsedCycles != 0u)
                    {
                        const auto budgetEnd = initialCycle + record.maxCycles;
                        do
                        {
                            const auto pairPc = vu->m_state.pc;
                            uint32_t lower = 0;
                            uint32_t upper = 0;
                            if (pairPc + 8u <= PS2_VU1_CODE_SIZE)
                            {
                                std::memcpy(&lower, memory->getVU1Code() + pairPc, sizeof(lower));
                                std::memcpy(&upper, memory->getVU1Code() + pairPc + 4u, sizeof(upper));
                                if (upperSamples && !vu->decodeUpperUsage(upper).reserved)
                                    ++fetches[upper];
                                require(fetches.size() <= 4096u, "Too many VU upper instruction candidates");
                            }
                            const auto beforeCycle = vu->m_cycle;
                            vu->run(memory->getVU1Code(), PS2_VU1_CODE_SIZE,
                                    memory->getVU1Data(), PS2_VU1_DATA_SIZE, gs, memory.get(), 1u);
                            if (pairSamples && vu->m_state.pc != pairPc)
                            {
                                ++executedPairs[{pairPc, lower, upper}];
                                ++executedEdges[{pairPc, vu->m_state.pc}];
                                require(executedPairs.size() <= 4096u, "Too many VU instruction-pair candidates");
                                require(executedEdges.size() <= 8192u, "Too many VU instruction-pair edges");
                            }
                            if (!vu->m_running || vu->m_cycle == beforeCycle)
                                break;
                        } while (vu->m_cycle < budgetEnd);
                    }
                    else
                    {
                        vu->run(memory->getVU1Code(), PS2_VU1_CODE_SIZE,
                                memory->getVU1Data(), PS2_VU1_DATA_SIZE, gs, memory.get(), record.maxCycles);
                    }
                }
                const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count();
                const auto actual = saveState(*vu);
                if (actual != record.after || context.overflow || context.gifs.bytes != record.gifs ||
                    std::memcmp(memory->getVU1Data(), record.afterData.data(), record.afterData.size()) != 0)
                {
                    result.error = "Replay mismatch in case " + std::to_string(result.cases) +
                        " pc=" + std::to_string(initialPc) + " iteration=" + std::to_string(iteration) +
                        " state=" + std::to_string(actual == record.after) +
                        " data=" + std::to_string(std::memcmp(memory->getVU1Data(), record.afterData.data(), record.afterData.size()) == 0) +
                        " gifs=" + std::to_string(context.gifs.bytes == record.gifs);
                    return result;
                }
                // The first iteration validates a cold decode cache but is not timed.
                if (iteration != 0u)
                {
                    ++result.iterations;
                    result.executeNs += static_cast<uint64_t>(ns);
                    caseNs += static_cast<uint64_t>(ns);
                    result.cycles += elapsedCycles;
                }
            }
            for (const auto *part : {&record.after, &record.afterData, &record.gifs})
                for (const auto byte : *part)
                    result.digest = (result.digest ^ byte) * 1099511628211ull;
            result.timings.push_back({initialPc, record.maxCycles, elapsedCycles, caseNs,
                                      static_cast<uint32_t>(record.gifs.size())});
            ++result.cases;
        }
        require(result.cases != 0u && !input.bad(), "Empty or unreadable VU replay file");
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
        result.nativeUpper = vu->upperCounters().native;
        result.interpretedUpper = vu->upperCounters().interpreted;
#endif
#if defined(PS2X_ENABLE_VU_NATIVE_PAIRS)
        result.nativePairs = vu->pairCounters().native;
        result.interpretedPairs = vu->pairCounters().interpreted;
#endif
        if (upperSamples)
        {
            for (const auto &[instruction, count] : fetches)
                upperSamples->push_back({instruction, count});
            std::sort(upperSamples->begin(), upperSamples->end(), [](const auto &left, const auto &right)
            {
                return left.fetches != right.fetches ? left.fetches > right.fetches
                                                    : left.instruction < right.instruction;
            });
        }
        if (pairSamples)
        {
            for (const auto &[key, count] : executedPairs)
            {
                const auto &[pc, lower, upper] = key;
                PairSample sample{pc, lower, upper, count};
                sample.entry = entryPcs.contains(pc);
                for (const auto &[edge, edgeCount] : executedEdges)
                {
                    (void)edgeCount;
                    if (edge.first == pc)
                        sample.successors.push_back(edge.second);
                }
                pairSamples->push_back(std::move(sample));
            }
            std::sort(pairSamples->begin(), pairSamples->end(), [](const auto &left, const auto &right)
            {
                if (left.executions != right.executions)
                    return left.executions > right.executions;
                if (left.pc != right.pc)
                    return left.pc < right.pc;
                if (left.lower != right.lower)
                    return left.lower < right.lower;
                return left.upper < right.upper;
            });
        }
    }
    catch (const std::exception &error)
    {
        result.error = error.what();
    }
    return result;
}

bool VUReplay::writeUpperKernels(std::ostream &output,
                                const std::vector<UpperSample> &samples, uint32_t limit)
{
    if (limit == 0u || limit > 512u || samples.empty())
        return false;
    const auto flags = output.flags();
    const auto fill = output.fill();
    output << "// Private replay-derived VU instructions. Do not redistribute.\n";
    for (size_t i = 0; i < std::min<size_t>(limit, samples.size()); ++i)
        output << "VU_NATIVE_WORD(0x" << std::hex << std::setw(8) << std::setfill('0')
               << samples[i].instruction << "u)\n";
    output.flags(flags);
    output.fill(fill);
    return static_cast<bool>(output);
}

bool VUReplay::writePairKernels(std::ostream &output,
                               const std::vector<PairSample> &samples, uint32_t limit)
{
    if (limit == 0u || limit > 4096u || samples.empty())
        return false;
    const auto flags = output.flags();
    const auto fill = output.fill();
    std::set<uint32_t> entries;
    std::set<std::pair<uint32_t, uint32_t>> edges;
    std::map<uint32_t, const PairSample *> pairsByPc;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> pairWordHits;
    for (size_t i = 0; i < std::min<size_t>(limit, samples.size()); ++i)
    {
        const auto &sample = samples[i];
        if ((sample.pc & 7u) != 0u || sample.pc >= PS2_VU1_CODE_SIZE || sample.executions == 0u)
            return false;
        if (!pairsByPc.emplace(sample.pc, &sample).second)
            return false;
        auto &wordHits = pairWordHits[{sample.lower, sample.upper}];
        if (UINT64_MAX - wordHits < sample.executions)
            return false;
        wordHits += sample.executions;
        if (sample.entry)
            entries.insert(sample.pc);
        for (const uint32_t successor : sample.successors)
        {
            if ((successor & 7u) != 0u || successor >= PS2_VU1_CODE_SIZE)
                return false;
            edges.insert({sample.pc, successor});
        }
    }
    output << "// Private replay-derived VU instruction pairs. Do not redistribute.\n";
    for (const uint32_t entry : entries)
        output << "VU_NATIVE_ENTRY(0x" << std::hex << std::setw(4) << std::setfill('0') << entry << "u)\n";
    std::map<uint32_t, std::set<uint32_t>> predecessors;
    for (const auto &[from, to] : edges)
    {
        (void)from;
        predecessors[to].insert(from);
    }
    std::set<uint32_t> assigned;
    const auto emitBlock = [&](uint32_t start)
    {
        if (assigned.contains(start))
            return;
        uint32_t pc = start;
        uint32_t count = 0u;
        while (count < 8u)
        {
            const auto pair = pairsByPc.find(pc);
            if (pair == pairsByPc.end() || !assigned.insert(pc).second)
                break;
            ++count;
            const auto &successors = pair->second->successors;
            const uint32_t next = (pc + 8u) & (PS2_VU1_CODE_SIZE - 1u);
            const auto incoming = predecessors.find(next);
            if (successors.size() != 1u || successors[0] != next || entries.contains(next) ||
                incoming == predecessors.end() || incoming->second.size() != 1u ||
                !incoming->second.contains(pc))
                break;
            pc = next;
        }
        output << "VU_NATIVE_BLOCK(0x" << std::hex << std::setw(4) << std::setfill('0') << start
               << "u, " << std::dec << count << "u)\n";
    };
    for (const auto &[pc, pair] : pairsByPc)
    {
        (void)pair;
        const uint32_t previous = (pc - 8u) & (PS2_VU1_CODE_SIZE - 1u);
        const auto incoming = predecessors.find(pc);
        const bool sequentialOnly = incoming != predecessors.end() && incoming->second.size() == 1u &&
                                    incoming->second.contains(previous);
        if (entries.contains(pc) || !sequentialOnly)
            emitBlock(pc);
    }
    for (const auto &[pc, pair] : pairsByPc)
    {
        (void)pair;
        emitBlock(pc);
    }
    for (size_t i = 0; i < std::min<size_t>(limit, samples.size()); ++i)
    {
        const auto &sample = samples[i];
        output << "VU_NATIVE_PAIR(0x" << std::hex << std::setw(4) << std::setfill('0') << sample.pc
               << "u, 0x" << std::setw(8) << sample.lower
               << "u, 0x" << std::setw(8) << sample.upper << "u)\n";
    }
    std::vector<std::pair<std::pair<uint32_t, uint32_t>, uint64_t>> rankedPairs(
        pairWordHits.begin(), pairWordHits.end());
    std::sort(rankedPairs.begin(), rankedPairs.end(), [](const auto &left, const auto &right)
    {
        if (left.second != right.second)
            return left.second > right.second;
        return left.first < right.first;
    });
    for (const auto &[words, executions] : rankedPairs)
    {
        output << "VU_NATIVE_PAIR_WORDS(0x" << std::hex << std::setw(8) << std::setfill('0') << words.first
               << "u, 0x" << std::setw(8) << words.second
               << "u, " << std::dec << executions << "u)\n";
    }
    for (const auto &[from, to] : edges)
        output << "VU_NATIVE_EDGE(0x" << std::hex << std::setw(4) << std::setfill('0') << from
               << "u, 0x" << std::setw(4) << to << "u)\n";
    output.flags(flags);
    output.fill(fill);
    return static_cast<bool>(output);
}
