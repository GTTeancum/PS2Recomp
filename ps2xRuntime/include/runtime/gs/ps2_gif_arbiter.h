#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifArbiterPacket
{
    GifPathId pathId;
    bool path2DirectHl = false;
    bool path3Image = false;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn = std::function<void(const uint8_t *, uint32_t)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl = false);

    void drain();
    bool empty() const { return m_queue.empty(); }

    struct DebugCounters
    {
        std::array<uint64_t, 3> submitted{};
        std::array<uint64_t, 3> processed{};
    };

    DebugCounters debugCounters() const;

private:
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;
    std::array<std::atomic<uint64_t>, 3> m_debugSubmitted{};
    std::array<std::atomic<uint64_t>, 3> m_debugProcessed{};

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
