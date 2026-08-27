#include "runtime/gs/ps2_gif_arbiter.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
    {
        static std::atomic<uint32_t> dropTraceCount{0u};
        const uint32_t dropTraceIndex = dropTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (dropTraceIndex < 32u)
        {
            std::fprintf(stderr,
                         "[gif-arbiter-submit-drop] idx=%u path=%u data=%p bytes=%u processFn=%u\n",
                         dropTraceIndex,
                         static_cast<unsigned>(pathId),
                         static_cast<const void *>(data),
                         sizeBytes,
                         m_processFn ? 1u : 0u);
        }
        return;
    }

    uint64_t tagLo = 0u;
    uint64_t tagHi = 0u;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    std::memcpy(&tagHi, data + sizeof(tagLo), sizeof(tagHi));
    static std::atomic<uint32_t> submitTraceCount{0u};
    const uint32_t submitTraceIndex = submitTraceCount.fetch_add(1u, std::memory_order_relaxed);
    if (submitTraceIndex < 128u)
    {
        std::fprintf(stderr,
                     "[gif-arbiter-submit] idx=%u queueBefore=%zu path=%u directHl=%u image=%u bytes=%u tagLo=0x%016llx tagHi=0x%016llx\n",
                     submitTraceIndex,
                     m_queue.size(),
                     static_cast<unsigned>(pathId),
                     path2DirectHl ? 1u : 0u,
                     ((pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes)) ? 1u : 0u,
                     sizeBytes,
                     static_cast<unsigned long long>(tagLo),
                     static_cast<unsigned long long>(tagHi));
    }

    static bool loggedXmenClutPacket = false;
    for (uint32_t offset = 8u;
         !loggedXmenClutPacket && offset + 8u <= sizeBytes;
         offset += 8u)
    {
        uint64_t value = 0u;
        uint64_t reg = 0u;
        std::memcpy(&value, data + offset - 8u, sizeof(value));
        std::memcpy(&reg, data + offset, sizeof(reg));
        const uint32_t dbp = static_cast<uint32_t>((value >> 32u) & 0x3FFFu);
        if ((reg & 0xFFu) == 0x50u && dbp == 12224u)
        {
            std::fprintf(stderr,
                         "[xmen-clut-gif-path] path=%u directHl=%u bytes=%u adOffset=0x%x head=",
                         static_cast<unsigned>(pathId), path2DirectHl ? 1u : 0u,
                         sizeBytes, offset - 8u);
            const uint32_t dumpBytes = std::min<uint32_t>(sizeBytes, 160u);
            for (uint32_t i = 0u; i < dumpBytes; ++i)
                std::fprintf(stderr, "%02x", static_cast<unsigned>(data[i]));
            std::fprintf(stderr, "\n");
            loggedXmenClutPacket = true;
        }
    }

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    static std::atomic<uint32_t> drainTraceCount{0u};
    const uint32_t drainTraceIndex = drainTraceCount.fetch_add(1u, std::memory_order_relaxed);
    if (drainTraceIndex < 128u)
    {
        std::fprintf(stderr,
                     "[gif-arbiter-drain] idx=%u queue=%zu processFn=%u\n",
                     drainTraceIndex,
                     m_queue.size(),
                     m_processFn ? 1u : 0u);
    }

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            static std::atomic<uint32_t> processTraceCount{0u};
            const uint32_t processTraceIndex = processTraceCount.fetch_add(1u, std::memory_order_relaxed);
            if (processTraceIndex < 128u)
            {
                uint64_t tagLo = 0u;
                uint64_t tagHi = 0u;
                std::memcpy(&tagLo, pkt.data.data(), sizeof(tagLo));
                std::memcpy(&tagHi, pkt.data.data() + sizeof(tagLo), sizeof(tagHi));
                std::fprintf(stderr,
                             "[gif-arbiter-process] idx=%u path=%u bytes=%zu tagLo=0x%016llx tagHi=0x%016llx\n",
                             processTraceIndex,
                             static_cast<unsigned>(pkt.pathId),
                             pkt.data.size(),
                             static_cast<unsigned long long>(tagLo),
                             static_cast<unsigned long long>(tagHi));
            }
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
