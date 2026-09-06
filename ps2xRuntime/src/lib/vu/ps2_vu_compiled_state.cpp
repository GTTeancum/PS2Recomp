#include "runtime/ps2_vu_compiled_state.h"
#include "runtime/ps2_vu1_replay.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <limits>

std::optional<VUCompiledState::Input> VUCompiledState::capture(const VU1Interpreter &v, uint32_t budget)
{
    const auto &s = v.m_state;
    if (v.m_unit != VU1Interpreter::Unit::VU1 || budget <= 64 || budget > 1048576 ||
        !v.m_running || v.m_stopRequested || s.pc >= 16384 || (s.pc & 7) || s.ebit ||
        s.haltAfterDelaySlot || s.branchPending || s.dBitEnabled || s.tBitEnabled ||
        s.stoppedByD || s.stoppedByT || v.m_pendingHaltD || v.m_pendingHaltT ||
        v.m_xgkick.active || v.m_fdiv.valid || v.m_storePipelineMask ||
        v.m_viWritePipelineMask || v.m_accWritePipelineMask ||
        v.m_efuResourceReady > v.m_cycle || v.m_cycle > std::numeric_limits<uint64_t>::max() - budget)
        return std::nullopt;
    for (const auto &e : v.m_efu) if (e.valid) return std::nullopt;
    for (const auto &e : v.m_storePipeline) if (e.valid) return std::nullopt;
    for (const auto &e : v.m_viWritePipeline) if (e.valid) return std::nullopt;
    for (const auto &e : v.m_accWritePipeline) if (e.valid) return std::nullopt;
    for (const auto ready : v.m_viReady) if (ready > v.m_cycle) return std::nullopt;
    for (const auto ready : v.m_accReady) if (ready > v.m_cycle) return std::nullopt;
    for (const auto &reg : v.m_vfReady)
        for (const auto ready : reg) if (ready > v.m_cycle + 3) return std::nullopt;
    if (v.m_viBranchBackupValid && v.m_viBranchBackupReg >= 16) return std::nullopt;
    Input input{};
    input.state = s;
    input.cycle = v.m_cycle;
    input.nextSequence = v.m_nextWriteSequence;
    input.budget = budget;
    input.vfReady = v.m_vfReady;
    input.vfLatest = v.m_vfLatestWrite;
    input.branchBackupValue = v.m_viBranchBackupValue;
    input.branchBackupReg = v.m_viBranchBackupReg;
    input.branchBackupValid = v.m_viBranchBackupValid;
    for (size_t i = 0; i < input.vfWrites.size(); ++i)
    {
        const auto &e = v.m_vfWritePipeline[i];
        auto &out = input.vfWrites[i];
        if (!e.valid) continue;
        if (e.readyCycle <= v.m_cycle || e.readyCycle > v.m_cycle + 3 ||
            e.reg == 0 || e.reg >= 32 || !e.laneMask || e.laneMask > 15) return std::nullopt;
        out.ready = e.readyCycle;
        out.sequence = e.sequence;
        std::memcpy(out.words.data(), e.value.data(), 16);
        out.reg = e.reg;
        out.lanes = e.laneMask;
        out.valid = true;
        input.vfMask |= 1u << i;
    }
    for (size_t i = 0; i < input.flags.size(); ++i)
    {
        const auto &e = v.m_flagPipeline[i];
        if (!e.valid) continue;
        if (e.readyCycle <= v.m_cycle || e.readyCycle > v.m_cycle + 3) return std::nullopt;
        input.flags[i] = {e.readyCycle, e.issueCycle, e.mac, e.status, e.extraSticky, e.clip,
            e.valid, e.writesMac, e.writesStatus, e.writesSticky, e.writesClip};
        input.flagMask |= 1u << i;
    }
    if (input.vfMask != v.m_vfWritePipelineMask || input.flagMask != v.m_flagPipelineMask) return std::nullopt;
    return input;
}

namespace
{
bool sameState(const VU1State &a, const VU1State &b)
{
    return !std::memcmp(a.vf, b.vf, sizeof(a.vf)) && !std::memcmp(a.vi, b.vi, sizeof(a.vi)) &&
        !std::memcmp(a.acc, b.acc, sizeof(a.acc)) && !std::memcmp(&a.q, &b.q, sizeof(a.q)) &&
        !std::memcmp(&a.p, &b.p, sizeof(a.p)) && !std::memcmp(&a.i, &b.i, sizeof(a.i)) &&
        a.r == b.r && a.pc == b.pc && a.mac == b.mac && a.clip == b.clip && a.status == b.status &&
        a.cycles == b.cycles && a.ebit == b.ebit && a.haltAfterDelaySlot == b.haltAfterDelaySlot &&
        a.dBitEnabled == b.dBitEnabled && a.tBitEnabled == b.tBitEnabled &&
        a.stoppedByD == b.stoppedByD && a.stoppedByT == b.stoppedByT && a.top == b.top && a.itop == b.itop &&
        a.branchPending == b.branchPending && a.branchTarget == b.branchTarget && a.branchDelay == b.branchDelay;
}

bool validPacket(const std::vector<uint8_t> &bytes)
{
    if (bytes.empty() || bytes.size() > 65536 || (bytes.size() & 15)) return false;
    size_t offset = 0;
    while (offset < bytes.size())
    {
        uint64_t tag;
        std::memcpy(&tag, bytes.data() + offset, 8);
        const uint32_t loops = static_cast<uint32_t>(tag & 0x7fff);
        const uint32_t format = static_cast<uint32_t>((tag >> 58) & 3);
        const uint32_t nreg = (tag >> 60) ? static_cast<uint32_t>(tag >> 60) : 16;
        const uint32_t payload = format == 0 ? loops * nreg * 16 :
            format == 1 ? ((loops * nreg + 1) / 2) * 16 : loops * 16;
        if (payload > bytes.size() - offset - 16) return false;
        offset += 16 + payload;
        if (tag & 0x8000) return offset == bytes.size();
    }
    return false;
}
}

bool VUCompiledState::commit(VU1Interpreter &v, const Input &input, const Output &output,
    uint8_t *data, uint32_t dataSize, GS &gs, PS2Memory *memory)
{
    const auto current = capture(v, input.budget);
    if (!current || !data || dataSize != 16384 ||
        !sameState(current->state, input.state) ||
        current->cycle != input.cycle || current->nextSequence != input.nextSequence ||
        current->vfWrites != input.vfWrites || current->flags != input.flags ||
        current->vfReady != input.vfReady || current->vfLatest != input.vfLatest ||
        current->branchBackupValue != input.branchBackupValue || current->branchBackupReg != input.branchBackupReg ||
        current->branchBackupValid != input.branchBackupValid || current->vfMask != input.vfMask ||
        current->flagMask != input.flagMask || !output.elapsed || output.elapsed > input.budget ||
        (output.statusMask & 0xfff) != 0xfff || (output.macMask & 0xffff) != 0xffff ||
        output.packets.size() > 1024) return false;
    const auto &s = output.state;
    constexpr uint32_t zeroRegister[] = {0, 0, 0, 0x3f800000};
    if (s.pc >= 16384 || (s.pc & 7) || s.cycles != input.cycle + output.elapsed ||
        s.ebit || s.haltAfterDelaySlot || s.branchPending || s.dBitEnabled || s.tBitEnabled ||
        s.stoppedByD || s.stoppedByT || s.top != input.state.top || s.itop != input.state.itop ||
        s.vi[0] || std::memcmp(s.vf[0], zeroRegister, sizeof(zeroRegister)) ||
        (s.mac & ~0xffffu) || (s.status & ~0xfffu) || (s.clip & ~0xffffffu)) return false;
    for (const auto &e : input.flags) if (e.valid && e.ready > s.cycles) return false;
    for (const auto &e : input.vfWrites) if (e.valid && e.ready > s.cycles) return false;
    for (const auto &reg : input.vfReady)
        for (const auto ready : reg) if (ready > s.cycles) return false;
    size_t bytes = 0;
    uint64_t previousCycle = 0;
    for (const auto &packet : output.packets)
    {
        if (!packet.cycle || packet.cycle < previousCycle || packet.cycle > output.elapsed || !validPacket(packet.bytes)) return false;
        previousCycle = packet.cycle;
        bytes += packet.bytes.size();
        if (bytes > 1048576) return false;
    }
    // Validation is complete before any state, memory, or graphics side effect.
    v.m_state = output.state;
    v.m_cycle = output.state.cycles;
    // Only VF/flag writes could be pending at capture. Keep inert buffers and
    // monotonic sequence metadata; all incoming deadlines are now in the past.
    for (auto &e : v.m_flagPipeline) e.valid = false;
    for (auto &e : v.m_vfWritePipeline) e.valid = false;
    v.m_flagPipelineMask = v.m_vfWritePipelineMask = 0;
    v.m_workingClip = output.state.clip;
    v.m_viBranchBackupValue = 0;
    v.m_viBranchBackupReg = 0;
    v.m_viBranchBackupValid = false;
    v.m_running = false;
    std::memcpy(data, output.data.data(), output.data.size());
    for (const auto &packet : output.packets)
    {
        const auto size = static_cast<uint32_t>(packet.bytes.size());
        if (VUReplay::observeGif(packet.bytes.data(), size, input.cycle + packet.cycle)) continue;
        if (memory) memory->submitGifPacket(GifPathId::Path1, packet.bytes.data(), size);
        else gs.processGIFPacket(packet.bytes.data(), size);
    }
    return true;
}
