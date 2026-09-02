#pragma once

#include "runtime/gs/gs_types.h"

#include <array>
#include <cstdint>
#include <vector>

struct GSRasterDebugCounters
{
    uint64_t submits = 0u;
    uint64_t frame0Submits = 0u;
    uint64_t frame140Submits = 0u;
    uint64_t otherFrameSubmits = 0u;
    uint64_t viewportVertices = 0u;
    std::array<uint64_t, 7> primitiveSubmits{};
    uint64_t fullViewportSprites = 0u;
    uint64_t blackFullViewportSprites = 0u;
    uint64_t lastWireframeSubmit = 0u;
    uint64_t lastFullViewportSpriteSubmit = 0u;
    uint32_t lastFullViewportSpriteFbp = 0u;
    uint32_t lastFullViewportSpriteRgba = 0u;
    uint32_t lastFullViewportSpriteFlags = 0u;
    uint64_t wireframeEdges = 0u;
    uint64_t wireframeVerifiedEdges = 0u;
    uint64_t presents = 0u;
    uint64_t lastPresentNonblackPixels = 0u;
    uint32_t lastDisplayFbp = 0u;
    uint32_t lastSourceFbp = 0u;
};

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void Flush() = 0;
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;
    virtual GSRasterDebugCounters GetDebugCounters() const { return {}; }
};
