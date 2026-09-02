#pragma once

#include "runtime/gs/gs_backend.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

class GSCpuBackend final : public GSRasterBackend
{
public:
    GSCpuBackend();

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    GSRasterDebugCounters GetDebugCounters() const override;

private:
    void ResetUnlocked();
    void LoadClutUnlocked(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    uint32_t ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;
    void WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);

    void DrawPrimitive(const GSPrimitiveBatch &batch);
    void DrawWhiteWireframe(const GSPrimitiveBatch &batch);
    void DrawSprite(const GSPrimitiveBatch &batch);
    void DrawTriangle(const GSPrimitiveBatch &batch);
    void DrawLine(const GSPrimitiveBatch &batch);
    void WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog);
    uint32_t SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t LookupCLUT(const GSDrawState &state, uint8_t index, uint8_t cpsm, uint8_t csa, uint8_t sourcePsm);

    void PerformLocalToLocalTransfer();
    void PerformLocalToHostTransfer();
    PresentationFrame PresentFromLocalMemory(const GSPresentationRequest &request);
    bool CopyFrameToHostRgba(const GSFrameReg &frame,
                             uint32_t width,
                             uint32_t height,
                             std::vector<uint8_t> &outPixels,
                             bool preserveAlpha,
                             bool useLocalMemoryLayout,
                             bool frameBaseIsPages,
                             uint32_t sourceOriginX,
                             uint32_t sourceOriginY) const;

    using WriteVramFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    using ReadVramFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);

    static constexpr size_t kPsmHandlerCount = 1u << 6u;
    mutable std::mutex m_mutex;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    std::array<ReadVramFunc, kPsmHandlerCount> m_readVramFuncs{};
    std::array<WriteVramFunc, kPsmHandlerCount> m_writeVramFuncs{};
    std::array<uint32_t, 768> m_clutCache{};
    std::array<uint32_t, 2> m_clutCbp{};

    GSTransferCommand m_transfer{};
    GSTransferSnapshot m_transferState{};
    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;
    std::atomic<uint64_t> m_debugSubmitCount{0u};
    std::atomic<uint64_t> m_debugFrame0SubmitCount{0u};
    std::atomic<uint64_t> m_debugFrame140SubmitCount{0u};
    std::atomic<uint64_t> m_debugOtherFrameSubmitCount{0u};
    std::atomic<uint64_t> m_debugViewportVertexCount{0u};
    std::array<std::atomic<uint64_t>, 7> m_debugPrimitiveSubmitCounts{};
    std::atomic<uint64_t> m_debugFullViewportSpriteCount{0u};
    std::atomic<uint64_t> m_debugBlackFullViewportSpriteCount{0u};
    std::atomic<uint64_t> m_debugLastWireframeSubmit{0u};
    std::atomic<uint64_t> m_debugLastFullViewportSpriteSubmit{0u};
    std::atomic<uint32_t> m_debugLastFullViewportSpriteFbp{0u};
    std::atomic<uint32_t> m_debugLastFullViewportSpriteRgba{0u};
    std::atomic<uint32_t> m_debugLastFullViewportSpriteFlags{0u};
    std::atomic<uint64_t> m_debugWireframeEdgeCount{0u};
    std::atomic<uint64_t> m_debugWireframeVerifiedEdgeCount{0u};
    std::atomic<uint64_t> m_debugPresentCount{0u};
    std::atomic<uint64_t> m_debugLastPresentNonblackPixelCount{0u};
    std::atomic<uint32_t> m_debugLastDisplayFbp{0u};
    std::atomic<uint32_t> m_debugLastSourceFbp{0u};
};
