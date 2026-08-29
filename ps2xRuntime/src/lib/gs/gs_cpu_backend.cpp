#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_psmct16.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace GSInternal;

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0) & 0xFF) >> 3;
        uint32_t g = ((c >> 8) & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0) & 0x1F) << 3;
        u32 g = ((c >> 5) & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};
    std::atomic<uint64_t> s_profileSubmitCount{0};
    std::atomic<uint64_t> s_profileRasterNanoseconds{0};
    std::atomic<uint32_t> s_submitProbeCount{0};
    std::atomic<uint32_t> s_presentSourceProbeCount{0};
    std::atomic<uint32_t> s_xmenLastDrawPresent{UINT32_MAX};
    std::atomic<uint64_t> s_xmenLastDrawTick{0u};
    std::atomic<uint32_t> s_xmenVramDrawTraceCount{0u};

    bool traceXmenVramRegion()
    {
        static const bool enabled = std::getenv("PS2X_TRACE_VRAM_REGION") != nullptr;
        return enabled;
    }

    uint64_t tracedRasterTick()
    {
        static const uint64_t tick = []()
        {
            const char *value = std::getenv("PS2X_TRACE_RASTER_TICK");
            if (!value || value[0] == '\0')
                return UINT64_MAX;

            char *end = nullptr;
            const uint64_t parsed = std::strtoull(value, &end, 0);
            return end && end != value && *end == '\0' ? parsed : UINT64_MAX;
        }();
        return tick;
    }

    bool isXmenVramRegionBlock(uint32_t block)
    {
        return block >= 11000u && block <= 11700u;
    }

    struct XmenRasterProbeStats
    {
        uint64_t covered = 0u;
        uint64_t alphaRejected = 0u;
        uint64_t destinationAlphaRejected = 0u;
        uint64_t depthRejected = 0u;
        uint64_t framebufferWrites = 0u;
        uint64_t nonzeroRgbWrites = 0u;
        uint64_t framebufferChangedWrites = 0u;
        uint64_t nonzeroFramebufferWrites = 0u;
        uint64_t sourceChannelSum = 0u;
        uint64_t depthWrites = 0u;
        uint64_t textureSamples = 0u;
        uint64_t nonzeroTextureRgbSamples = 0u;
        uint64_t nonzeroCombinedRgbSamples = 0u;
        uint64_t topChromaSourceWrites = 0u;
        uint64_t topChromaFramebufferWrites = 0u;
        uint32_t minIncomingZ = UINT32_MAX;
        uint32_t maxIncomingZ = 0u;
        uint32_t minStoredZ = UINT32_MAX;
        uint32_t maxStoredZ = 0u;
        uint32_t maxSourceChannel = 0u;
        uint32_t minCoveredX = UINT32_MAX;
        uint32_t maxCoveredX = 0u;
        uint32_t minCoveredY = UINT32_MAX;
        uint32_t maxCoveredY = 0u;
        uint32_t minTopChromaX = UINT32_MAX;
        uint32_t maxTopChromaX = 0u;
        uint32_t minTopChromaY = UINT32_MAX;
        uint32_t maxTopChromaY = 0u;
    };

    thread_local XmenRasterProbeStats *s_xmenActiveRasterProbe = nullptr;

    bool isHighChroma(uint8_t r, uint8_t g, uint8_t b)
    {
        const uint8_t minimum = std::min(r, std::min(g, b));
        const uint8_t maximum = std::max(r, std::max(g, b));
        return maximum >= 128u && static_cast<uint32_t>(maximum - minimum) >= 96u;
    }

    struct XmenTraceBounds
    {
        int32_t x0 = 0;
        int32_t y0 = 96;
        int32_t x1 = 639;
        int32_t y1 = 149;
    };

    const XmenTraceBounds &xmenChromaTraceBounds()
    {
        static const XmenTraceBounds bounds = []()
        {
            XmenTraceBounds parsed{};
            const char *value = std::getenv("PS2X_TRACE_CHROMA_BOUNDS");
            if (!value || value[0] == '\0')
                return parsed;

            int32_t x0 = 0;
            int32_t y0 = 0;
            int32_t x1 = 0;
            int32_t y1 = 0;
            if (std::sscanf(value, "%d,%d-%d,%d", &x0, &y0, &x1, &y1) == 4 &&
                x0 <= x1 && y0 <= y1)
            {
                parsed = {x0, y0, x1, y1};
            }
            return parsed;
        }();
        return bounds;
    }

    bool isInXmenChromaTraceBounds(int32_t x, int32_t y)
    {
        const XmenTraceBounds &bounds = xmenChromaTraceBounds();
        return x >= bounds.x0 && x <= bounds.x1 && y >= bounds.y0 && y <= bounds.y1;
    }

    XmenTraceBounds parseXmenTraceBounds(const char *environmentName)
    {
        XmenTraceBounds parsed{1, 1, 0, 0};
        const char *value = std::getenv(environmentName);
        if (!value || value[0] == '\0')
            return parsed;

        int32_t x0 = 0;
        int32_t y0 = 0;
        int32_t x1 = 0;
        int32_t y1 = 0;
        if (std::sscanf(value, "%d,%d-%d,%d", &x0, &y0, &x1, &y1) == 4 &&
            x0 <= x1 && y0 <= y1)
        {
            parsed = {x0, y0, x1, y1};
        }
        return parsed;
    }

    const XmenTraceBounds &xmenRasterTraceBounds()
    {
        static const XmenTraceBounds bounds =
            parseXmenTraceBounds("PS2X_TRACE_RASTER_BOUNDS");
        return bounds;
    }

    const XmenTraceBounds &xmenHudTraceBounds()
    {
        static const XmenTraceBounds bounds =
            parseXmenTraceBounds("PS2X_TRACE_HUD_BOUNDS");
        return bounds;
    }

    bool overlapsXmenTraceBounds(const XmenRasterProbeStats &stats,
                                 const XmenTraceBounds &bounds)
    {
        if (bounds.x0 > bounds.x1 || bounds.y0 > bounds.y1 || stats.minCoveredX == UINT32_MAX)
            return false;

        return static_cast<int32_t>(stats.maxCoveredX) >= bounds.x0 &&
               static_cast<int32_t>(stats.minCoveredX) <= bounds.x1 &&
               static_cast<int32_t>(stats.maxCoveredY) >= bounds.y0 &&
               static_cast<int32_t>(stats.minCoveredY) <= bounds.y1;
    }

    void accumulateXmenRasterStats(XmenRasterProbeStats &total, const XmenRasterProbeStats &batch)
    {
        total.covered += batch.covered;
        total.alphaRejected += batch.alphaRejected;
        total.destinationAlphaRejected += batch.destinationAlphaRejected;
        total.depthRejected += batch.depthRejected;
        total.framebufferWrites += batch.framebufferWrites;
        total.nonzeroRgbWrites += batch.nonzeroRgbWrites;
        total.framebufferChangedWrites += batch.framebufferChangedWrites;
        total.nonzeroFramebufferWrites += batch.nonzeroFramebufferWrites;
        total.sourceChannelSum += batch.sourceChannelSum;
        total.depthWrites += batch.depthWrites;
        total.textureSamples += batch.textureSamples;
        total.nonzeroTextureRgbSamples += batch.nonzeroTextureRgbSamples;
        total.nonzeroCombinedRgbSamples += batch.nonzeroCombinedRgbSamples;
        total.minIncomingZ = std::min(total.minIncomingZ, batch.minIncomingZ);
        total.maxIncomingZ = std::max(total.maxIncomingZ, batch.maxIncomingZ);
        total.minStoredZ = std::min(total.minStoredZ, batch.minStoredZ);
        total.maxStoredZ = std::max(total.maxStoredZ, batch.maxStoredZ);
        total.maxSourceChannel = std::max(total.maxSourceChannel, batch.maxSourceChannel);
        total.minCoveredX = std::min(total.minCoveredX, batch.minCoveredX);
        total.maxCoveredX = std::max(total.maxCoveredX, batch.maxCoveredX);
        total.minCoveredY = std::min(total.minCoveredY, batch.minCoveredY);
        total.maxCoveredY = std::max(total.maxCoveredY, batch.maxCoveredY);
    }

    struct XmenGameplayRasterAggregate
    {
        uint32_t present = UINT32_MAX;
        uint64_t firstTick = 0u;
        uint64_t lastTick = 0u;
        uint64_t batches = 0u;
        uint64_t vertices = 0u;
        uint64_t finiteVertices = 0u;
        uint64_t viewportVertices = 0u;
        uint64_t frame0Batches = 0u;
        uint64_t frame140Batches = 0u;
        uint64_t otherFrameBatches = 0u;
        uint64_t texturedBatches = 0u;
        uint64_t texturedVertices = 0u;
        std::array<uint64_t, 8> primitiveBatches{};
        int32_t minScreenX = INT32_MAX;
        int32_t minScreenY = INT32_MAX;
        int32_t maxScreenX = INT32_MIN;
        int32_t maxScreenY = INT32_MIN;
        int32_t minTexturedScreenX = INT32_MAX;
        int32_t minTexturedScreenY = INT32_MAX;
        int32_t maxTexturedScreenX = INT32_MIN;
        int32_t maxTexturedScreenY = INT32_MIN;
        XmenRasterProbeStats raster{};
        XmenRasterProbeStats texturedRaster{};
    };

    void logXmenGameplayRaster(const XmenGameplayRasterAggregate &total)
    {
        if (total.present == UINT32_MAX)
            return;

        const XmenRasterProbeStats &raster = total.raster;
        std::fprintf(stdout,
                     "[xmen-gameplay-raster] present=%u ticks=%llu-%llu batches=%llu vertices=%llu "
                     "finite=%llu viewport=%llu frame=0:%llu/140:%llu/other:%llu textured=%llu "
                     "prim=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu screen=%d,%d-%d,%d "
                     "texturedVertices=%llu texturedScreen=%d,%d-%d,%d "
                     "covered=%llu alphaReject=%llu destAlphaReject=%llu depthReject=%llu "
                     "fbWrites=%llu nonzeroSource=%llu changed=%llu storedNonzero=%llu "
                     "textureSamples=%llu nonzeroTexture=%llu nonzeroCombined=%llu "
                     "texturedCovered=%llu texturedAlphaReject=%llu texturedDepthReject=%llu "
                     "texturedWrites=%llu texturedChanged=%llu "
                     "coveredBounds=%u,%u-%u,%u incomingZ=%u-%u storedZ=%u-%u\n",
                     total.present,
                     static_cast<unsigned long long>(total.firstTick),
                     static_cast<unsigned long long>(total.lastTick),
                     static_cast<unsigned long long>(total.batches),
                     static_cast<unsigned long long>(total.vertices),
                     static_cast<unsigned long long>(total.finiteVertices),
                     static_cast<unsigned long long>(total.viewportVertices),
                     static_cast<unsigned long long>(total.frame0Batches),
                     static_cast<unsigned long long>(total.frame140Batches),
                     static_cast<unsigned long long>(total.otherFrameBatches),
                     static_cast<unsigned long long>(total.texturedBatches),
                     static_cast<unsigned long long>(total.primitiveBatches[0]),
                     static_cast<unsigned long long>(total.primitiveBatches[1]),
                     static_cast<unsigned long long>(total.primitiveBatches[2]),
                     static_cast<unsigned long long>(total.primitiveBatches[3]),
                     static_cast<unsigned long long>(total.primitiveBatches[4]),
                     static_cast<unsigned long long>(total.primitiveBatches[5]),
                     static_cast<unsigned long long>(total.primitiveBatches[6]),
                     static_cast<unsigned long long>(total.primitiveBatches[7]),
                     total.finiteVertices ? total.minScreenX : 0,
                     total.finiteVertices ? total.minScreenY : 0,
                     total.finiteVertices ? total.maxScreenX : 0,
                     total.finiteVertices ? total.maxScreenY : 0,
                     static_cast<unsigned long long>(total.texturedVertices),
                     total.texturedVertices ? total.minTexturedScreenX : 0,
                     total.texturedVertices ? total.minTexturedScreenY : 0,
                     total.texturedVertices ? total.maxTexturedScreenX : 0,
                     total.texturedVertices ? total.maxTexturedScreenY : 0,
                     static_cast<unsigned long long>(raster.covered),
                     static_cast<unsigned long long>(raster.alphaRejected),
                     static_cast<unsigned long long>(raster.destinationAlphaRejected),
                     static_cast<unsigned long long>(raster.depthRejected),
                     static_cast<unsigned long long>(raster.framebufferWrites),
                     static_cast<unsigned long long>(raster.nonzeroRgbWrites),
                     static_cast<unsigned long long>(raster.framebufferChangedWrites),
                     static_cast<unsigned long long>(raster.nonzeroFramebufferWrites),
                     static_cast<unsigned long long>(raster.textureSamples),
                     static_cast<unsigned long long>(raster.nonzeroTextureRgbSamples),
                     static_cast<unsigned long long>(raster.nonzeroCombinedRgbSamples),
                     static_cast<unsigned long long>(total.texturedRaster.covered),
                     static_cast<unsigned long long>(total.texturedRaster.alphaRejected),
                     static_cast<unsigned long long>(total.texturedRaster.depthRejected),
                     static_cast<unsigned long long>(total.texturedRaster.framebufferWrites),
                     static_cast<unsigned long long>(total.texturedRaster.framebufferChangedWrites),
                     raster.minCoveredX == UINT32_MAX ? 0u : raster.minCoveredX,
                     raster.minCoveredY == UINT32_MAX ? 0u : raster.minCoveredY,
                     raster.maxCoveredX,
                     raster.maxCoveredY,
                     raster.minIncomingZ == UINT32_MAX ? 0u : raster.minIncomingZ,
                     raster.maxIncomingZ,
                     raster.minStoredZ == UINT32_MAX ? 0u : raster.minStoredZ,
                     raster.maxStoredZ);
        std::fflush(stdout);
    }

    int wrapTextureCoordinate(int coordinate,
                              int textureSize,
                              uint8_t mode,
                              uint16_t regionMin,
                              uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0: // REPEAT
            return static_cast<int>(static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(textureSize - 1));
        case 1: // CLAMP
            return clampInt(coordinate, 0, textureSize - 1);
        case 2: // REGION_CLAMP
            return std::min(std::max(coordinate, static_cast<int>(regionMin)), static_cast<int>(regionMax));
        case 3: // REGION_REPEAT
            return static_cast<int>((static_cast<uint32_t>(coordinate) & static_cast<uint32_t>(regionMin)) | static_cast<uint32_t>(regionMax));
        default:
            return coordinate;
        }
    }

    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct PixelWriteMask
    {
        bool writeRgb = true;
        bool writeAlpha = true;
        bool writeDepth = true;

        bool writesFramebuffer() const
        {
            return writeRgb || writeAlpha;
        }

        bool writesAnything() const
        {
            return writesFramebuffer() || writeDepth;
        }
    };

    PixelWriteMask classifyAlphaTest(uint64_t testReg, uint8_t alpha, uint8_t framePsm)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, true, false};
        case 2: // ZB_ONLY
            return {false, false, true};
        case 3: // RGB_ONLY
            // RGB_ONLY is only distinct for RGBA32. The GS treats it as
            // FB_ONLY for RGB24 and RGBA16 framebuffers.
            if (framePsm == GS_PSM_CT32)
                return {true, false, false};
            return {true, true, false};
        case 0: // KEEP
        default:
            return {false, false, false};
        }
    }

    bool passesDestinationAlphaTest(uint64_t testReg, uint8_t framePsm, uint32_t rawFramebufferPixel)
    {
        const bool date = ((testReg >> 14) & 0x1u) != 0u;
        if (!date)
            return true;

        const bool datm = ((testReg >> 15) & 0x1u) != 0u;
        switch (framePsm)
        {
        case GS_PSM_CT32:
            return (((rawFramebufferPixel >> 31) & 0x1u) != 0u) == datm;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            return (((rawFramebufferPixel >> 15) & 0x1u) != 0u) == datm;
        case GS_PSM_CT24:
            // RGB24 has no destination alpha, so DATE always passes.
            return true;
        default:
            return true;
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        // CSM1 swaps address bits 3 and 4. Preserve the remaining bits:
        // 16-bit CLUTs expose a ninth address bit through CSA[4].
        return (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    // TODO: clut cache
    uint32_t resolveClutIndex(uint8_t index, uint8_t cpsm, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        // CSM2 addresses the source directly through TEXCLUT. CSA is required
        // to be zero there, so it must not offset the source coordinates.
        if (csm != 0u)
            return (sourcePsm == GS_PSM_T4 ||
                    sourcePsm == GS_PSM_T4HH ||
                    sourcePsm == GS_PSM_T4HL)
                       ? (clutIndex & 0x0Fu)
                       : clutIndex;

        const bool is16BitClut = cpsm == GS_PSM_CT16 || cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = is16BitClut ? 0x1Fu : 0x0Fu;
        const uint32_t clutIndexMask = is16BitClut ? 0x1FFu : 0x0FFu;
        const uint32_t clutBase = (static_cast<uint32_t>(csa) & csaMask) << 4u;

        switch (sourcePsm)
        {
        case GS_PSM_T4:
        case GS_PSM_T4HH:
        case GS_PSM_T4HL:
            clutIndex = clutBase + (clutIndex & 0x0Fu);
            break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            clutIndex = clutBase + clutIndex;
            break;
        default:
            return clutIndex;
        }

        return swizzleClutIndexCSM1(clutIndex & clutIndexMask);
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        return {
            static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu),
            static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu)};
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        return {
            (pmode64 & 0x1ull) != 0ull,
            (pmode64 & 0x2ull) != 0ull,
            ((pmode64 >> 5) & 0x1ull) != 0ull,
            ((pmode64 >> 6) & 0x1ull) != 0ull,
            ((pmode64 >> 7) & 0x1ull) != 0ull,
            static_cast<uint8_t>((pmode64 >> 8) & 0xFFu)};
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
                row[x * 4u + 3u] = 255u;
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + y * kHostFrameWidth * 4u;
            for (uint32_t x = 0; x < width; ++x)
            {
                if (row[x * 4u] != 0u || row[x * 4u + 1u] != 0u || row[x * 4u + 2u] != 0u)
                    ++count;
            }
        }
        return count;
    }

    void logBlackPresentationDiagnostics(const char *mode,
                                         const GSPresentationRequest &request,
                                         const PresentationFrame &result,
                                         const uint8_t *vram,
                                         uint32_t vramSize)
    {
        if (!result || countNonBlackPixels(result.pixels, result.width, result.height) != 0u)
            return;

        static std::atomic<uint32_t> s_blackPresentationLogCount{0u};
        const uint32_t index = s_blackPresentationLogCount.fetch_add(1u, std::memory_order_relaxed);
        if (index >= 24u)
            return;

        struct PageStat
        {
            uint32_t page = 0u;
            uint32_t count = 0u;
        };
        PageStat top[8]{};
        constexpr uint32_t pageSize = 8192u;
        const uint32_t pageCount = vramSize / pageSize;
        for (uint32_t page = 0u; page < pageCount; ++page)
        {
            const uint8_t *pageData = vram + page * pageSize;
            uint32_t nonzero = 0u;
            for (uint32_t i = 0u; i < pageSize; ++i)
                if (pageData[i] != 0u)
                    ++nonzero;
            if (nonzero == 0u)
                continue;
            for (PageStat &slot : top)
            {
                if (nonzero > slot.count)
                {
                    for (PageStat *move = top + 7; move != &slot; --move)
                        *move = *(move - 1);
                    slot = {page, nonzero};
                    break;
                }
            }
        }

        auto describeFrame = [](const GSFrameReg &frame) {
            std::ostringstream out;
            out << "fbp=" << frame.fbp << "/fbw=" << frame.fbw
                << "/psm=0x" << std::hex << static_cast<uint32_t>(frame.psm)
                << "/mask=0x" << frame.fbmsk << std::dec;
            return out.str();
        };

        const GSFrameReg display1 = decodeDisplayFrame(request.dispfb1);
        const GSFrameReg display2 = decodeDisplayFrame(request.dispfb2);
        std::cerr << "[gs:black-present] index=" << index
                  << " mode=" << mode
                  << " result=" << result.width << "x" << result.height
                  << " displayFbp=" << result.displayFbp
                  << " sourceFbp=" << result.sourceFbp
                  << " usedPreferred=" << (result.usedPreferred ? 1u : 0u)
                  << " pmode=0x" << std::hex << request.pmode
                  << " smode2=0x" << request.smode2
                  << " dispfb1=0x" << request.dispfb1
                  << " display1=0x" << request.display1
                  << " dispfb2=0x" << request.dispfb2
                  << " display2=0x" << request.display2
                  << std::dec
                  << " displayFrame1{" << describeFrame(display1) << "}"
                  << " displayFrame2{" << describeFrame(display2) << "}"
                  << " ctx0{" << describeFrame(request.contextFrames[0]) << "}"
                  << " ctx1{" << describeFrame(request.contextFrames[1]) << "}"
                  << " prefHas=" << (request.hasPreferredSource ? 1u : 0u)
                  << " prefDest=" << request.preferredDestFbp
                  << " pref{" << describeFrame(request.preferredSource) << "}"
                  << " topPages=";
        for (const PageStat &slot : top)
        {
            if (slot.count == 0u)
                continue;
            std::cerr << slot.page << ":" << slot.count << ",";
        }
        std::cerr << std::endl;
    }
}

GSCpuBackend::GSCpuBackend()
{
    using namespace GSMem;
    static std::once_flag lookupTablesOnce;
    std::call_once(lookupTablesOnce, []()
                   { InitLookupTables(); });
    for (size_t i = 0; i < kPsmHandlerCount; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_readVramFuncs[i] = ReadCT32;
            m_writeVramFuncs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_readVramFuncs[i] = ReadCT24;
            m_writeVramFuncs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_readVramFuncs[i] = ReadCT16;
            m_writeVramFuncs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_readVramFuncs[i] = ReadCT16S;
            m_writeVramFuncs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_readVramFuncs[i] = ReadP8;
            m_writeVramFuncs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_readVramFuncs[i] = ReadP8H;
            m_writeVramFuncs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_readVramFuncs[i] = ReadP4;
            m_writeVramFuncs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_readVramFuncs[i] = ReadP4HH;
            m_writeVramFuncs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_readVramFuncs[i] = ReadP4HL;
            m_writeVramFuncs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_readVramFuncs[i] = ReadZ32;
            m_writeVramFuncs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_readVramFuncs[i] = ReadZ24;
            m_writeVramFuncs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_readVramFuncs[i] = ReadZ16;
            m_writeVramFuncs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_readVramFuncs[i] = ReadZ16S;
            m_writeVramFuncs[i] = WriteZ16S;
            break;
        default:
            m_readVramFuncs[i] = ReadNull;
            m_writeVramFuncs[i] = WriteNull;
            break;
        }
    }
    Reset();
}

void GSCpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_vram = vram;
    m_vramSize = vramSize;
    ResetUnlocked();
}

void GSCpuBackend::Reset()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ResetUnlocked();
}

void GSCpuBackend::ResetUnlocked()
{
    m_transfer = {};
    m_transfer.direction = 3u;
    m_transferState = {};
    m_transferState.direction = 3u;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
}

void GSCpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || batch.vertexCount == 0u)
    {
        static std::atomic<uint32_t> s_submitDropProbeCount{0u};
        const uint32_t dropIndex = s_submitDropProbeCount.fetch_add(1u, std::memory_order_relaxed);
        if (dropIndex < 16u)
        {
            std::fprintf(stderr,
                         "[gs:cpu-submit-drop] idx=%u vram=%u vertices=%u\n",
                         dropIndex,
                         m_vram ? 1u : 0u,
                         static_cast<unsigned>(batch.vertexCount));
        }
        return;
    }
    const bool traceXmenLegal = batch.state.context.frame.fbp == 140u &&
                                batch.state.prim.type != GS_PRIM_SPRITE;
    const uint32_t submitProbeIndex = s_submitProbeCount.fetch_add(1u, std::memory_order_relaxed);
    const bool traceSubmitProbe = submitProbeIndex < 256u ||
                                  (submitProbeIndex < 4096u && (submitProbeIndex & 127u) == 0u);
    const GSContext &submitCtx = batch.state.context;
    const int submitOfx = submitCtx.xyoffset.ofx >> 4;
    const int submitOfy = submitCtx.xyoffset.ofy >> 4;
    const uint32_t submitFrameBase = GSInternal::framePageBaseToBlock(submitCtx.frame.fbp);
    const uint32_t submitFbw = std::max<uint32_t>(submitCtx.frame.fbw, 1u);
    std::array<int, 8> sampleX{};
    std::array<int, 8> sampleY{};
    const auto screenX = [&](uint32_t i) -> int {
        return static_cast<int>(std::lround(batch.vertices[std::min<uint32_t>(i, batch.vertexCount - 1u)].x)) - submitOfx;
    };
    const auto screenY = [&](uint32_t i) -> int {
        return static_cast<int>(std::lround(batch.vertices[std::min<uint32_t>(i, batch.vertexCount - 1u)].y)) - submitOfy;
    };
    sampleX[0] = 0; sampleY[0] = 0;
    sampleX[1] = 100; sampleY[1] = 100;
    sampleX[2] = 320; sampleY[2] = 224;
    sampleX[3] = 639; sampleY[3] = 447;
    sampleX[4] = screenX(0u); sampleY[4] = screenY(0u);
    sampleX[5] = screenX(1u); sampleY[5] = screenY(1u);
    sampleX[6] = (screenX(0u) + screenX(1u)) / 2; sampleY[6] = (screenY(0u) + screenY(1u)) / 2;
    sampleX[7] = screenX(2u); sampleY[7] = screenY(2u);
    const auto readSubmitSample = [&](int x, int y) -> uint32_t {
        const uint32_t sx = static_cast<uint32_t>(GSInternal::clampInt(x, 0, 1023));
        const uint32_t sy = static_cast<uint32_t>(GSInternal::clampInt(y, 0, 1023));
        return ReadVramUnlocked(submitCtx.frame.psm, submitFrameBase, submitFbw, sx, sy);
    };
    std::array<uint32_t, 8> beforeSubmitSamples{};
    if (traceSubmitProbe)
    {
        for (size_t i = 0; i < beforeSubmitSamples.size(); ++i)
            beforeSubmitSamples[i] = readSubmitSample(sampleX[i], sampleY[i]);
    }
    const auto readTracePixel = [&]() -> uint32_t
    {
        const GSContext &ctx = batch.state.context;
        return ReadVramUnlocked(ctx.frame.psm,
                                GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                std::max<uint32_t>(ctx.frame.fbw, 1u), 100u, 100u);
    };
    const uint32_t before = traceXmenLegal ? readTracePixel() : 0u;
    const GSContext &xmenContext = batch.state.context;
    const bool isXmenWorldStrip = batch.debugVsyncTick >= 1910u &&
                                  (xmenContext.frame.fbp == 0u || xmenContext.frame.fbp == 140u) &&
                                  batch.state.prim.type == GS_PRIM_TRISTRIP;
    const bool traceXmenUntextured = isXmenWorldStrip &&
                                     !batch.state.prim.tme &&
                                     batch.vertices[0].z >= 28000.0 &&
                                     batch.vertices[0].z <= 40000.0;
    const bool traceXmenTextured = isXmenWorldStrip &&
                                   batch.state.prim.tme &&
                                   xmenContext.tex0.tbp0 == 12800u &&
                                   xmenContext.tex0.psm == GS_PSM_T8;
    const bool matchesTargetRaster =
        batch.debugVsyncTick == tracedRasterTick() &&
        xmenContext.frame.fbp == 0u &&
        batch.state.prim.type == GS_PRIM_TRISTRIP;
    static std::atomic<uint32_t> s_targetRasterTraceCount{0u};
    const uint32_t targetRasterTraceIndex = matchesTargetRaster
        ? s_targetRasterTraceCount.fetch_add(1u, std::memory_order_relaxed)
        : UINT32_MAX;
    const bool traceTargetRaster = targetRasterTraceIndex < 256u;
    const uint32_t xmenTitleRasterGroup = traceXmenUntextured ? 0u : (traceXmenTextured ? 1u : UINT32_MAX);
    static std::array<std::atomic<uint32_t>, 2> s_xmenTitleRasterTraceCounts{};
    static std::array<XmenRasterProbeStats, 2> s_xmenTitleRasterTotals{};
    const uint32_t xmenTitleRasterTraceIndex = xmenTitleRasterGroup < s_xmenTitleRasterTraceCounts.size()
        ? s_xmenTitleRasterTraceCounts[xmenTitleRasterGroup].fetch_add(1u, std::memory_order_relaxed)
        : UINT32_MAX;
    static XmenGameplayRasterAggregate s_xmenGameplayRaster{};
    if (s_xmenGameplayRaster.present != UINT32_MAX &&
        s_xmenGameplayRaster.present != batch.debugPresentCount)
    {
        logXmenGameplayRaster(s_xmenGameplayRaster);
        s_xmenGameplayRaster = {};
    }
    static const std::pair<uint32_t, uint32_t> xmenGameplayRasterRange = []
    {
        uint32_t start = 636u;
        uint32_t end = 644u;
        const char *value = std::getenv("PS2X_XMEN_GAMEPLAY_RASTER_RANGE");
        if (value && std::sscanf(value, "%u-%u", &start, &end) == 2 && start <= end)
            return std::pair<uint32_t, uint32_t>{start, end};
        return std::pair<uint32_t, uint32_t>{636u, 644u};
    }();
    const bool traceXmenGameplayRaster =
        batch.debugPresentCount >= xmenGameplayRasterRange.first &&
        batch.debugPresentCount <= xmenGameplayRasterRange.second;
    static const uint32_t traceTopChromaPresent = []
    {
        const char *value = std::getenv("PS2X_TRACE_TOP_CHROMA_PRESENT");
        if (!value)
            return UINT32_MAX;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end != value && *end == '\0'
            ? static_cast<uint32_t>(std::min<unsigned long>(parsed, UINT32_MAX))
            : UINT32_MAX;
    }();
    const bool traceTopChroma = batch.debugPresentCount == traceTopChromaPresent;
    const bool traceRasterTick = batch.debugVsyncTick == tracedRasterTick();
    if (traceXmenGameplayRaster && s_xmenGameplayRaster.present == UINT32_MAX)
    {
        s_xmenGameplayRaster.present = batch.debugPresentCount;
        s_xmenGameplayRaster.firstTick = batch.debugVsyncTick;
    }
    if (traceXmenGameplayRaster)
    {
        s_xmenGameplayRaster.lastTick = batch.debugVsyncTick;
        ++s_xmenGameplayRaster.batches;
        s_xmenGameplayRaster.vertices += batch.vertexCount;
        if (xmenContext.frame.fbp == 0u)
            ++s_xmenGameplayRaster.frame0Batches;
        else if (xmenContext.frame.fbp == 140u)
            ++s_xmenGameplayRaster.frame140Batches;
        else
            ++s_xmenGameplayRaster.otherFrameBatches;
        if (batch.state.prim.tme)
            ++s_xmenGameplayRaster.texturedBatches;
        const uint32_t primitiveType = static_cast<uint32_t>(batch.state.prim.type);
        if (primitiveType < s_xmenGameplayRaster.primitiveBatches.size())
            ++s_xmenGameplayRaster.primitiveBatches[primitiveType];
        for (uint32_t vertexIndex = 0u; vertexIndex < batch.vertexCount; ++vertexIndex)
        {
            const GSVertex &vertex = batch.vertices[vertexIndex];
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y))
                continue;
            ++s_xmenGameplayRaster.finiteVertices;
            const int32_t x = static_cast<int32_t>(std::lround(vertex.x)) - submitOfx;
            const int32_t y = static_cast<int32_t>(std::lround(vertex.y)) - submitOfy;
            s_xmenGameplayRaster.minScreenX = std::min(s_xmenGameplayRaster.minScreenX, x);
            s_xmenGameplayRaster.minScreenY = std::min(s_xmenGameplayRaster.minScreenY, y);
            s_xmenGameplayRaster.maxScreenX = std::max(s_xmenGameplayRaster.maxScreenX, x);
            s_xmenGameplayRaster.maxScreenY = std::max(s_xmenGameplayRaster.maxScreenY, y);
            if (x >= 0 && x < 640 && y >= 0 && y < 448)
                ++s_xmenGameplayRaster.viewportVertices;
            if (batch.state.prim.tme)
            {
                ++s_xmenGameplayRaster.texturedVertices;
                s_xmenGameplayRaster.minTexturedScreenX =
                    std::min(s_xmenGameplayRaster.minTexturedScreenX, x);
                s_xmenGameplayRaster.minTexturedScreenY =
                    std::min(s_xmenGameplayRaster.minTexturedScreenY, y);
                s_xmenGameplayRaster.maxTexturedScreenX =
                    std::max(s_xmenGameplayRaster.maxTexturedScreenX, x);
                s_xmenGameplayRaster.maxTexturedScreenY =
                    std::max(s_xmenGameplayRaster.maxTexturedScreenY, y);
            }
        }
    }
    XmenRasterProbeStats xmenRasterStats{};
    if (xmenTitleRasterTraceIndex != UINT32_MAX || traceXmenGameplayRaster ||
        traceTopChroma || traceRasterTick || traceTargetRaster)
        s_xmenActiveRasterProbe = &xmenRasterStats;
    static const bool profileCpuRaster =
        std::getenv("PS2X_GS_CPU_PROFILE") != nullptr;
    const auto rasterStart = profileCpuRaster
        ? std::chrono::steady_clock::now()
        : std::chrono::steady_clock::time_point{};
    static const bool skipCpuRaster = std::getenv("PS2X_SKIP_CPU_RASTER") != nullptr;
    static const uint32_t skipCpuRasterBeforePresent = []
    {
        const char *value = std::getenv("PS2X_SKIP_CPU_RASTER_BEFORE_PRESENT");
        if (!value)
            return 0u;

        char *end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        return end != value && *end == '\0'
            ? static_cast<uint32_t>(std::min<unsigned long>(parsed, UINT32_MAX))
            : 0u;
    }();
    if (!skipCpuRaster && batch.debugPresentCount >= skipCpuRasterBeforePresent)
        DrawPrimitive(batch);
    s_xmenActiveRasterProbe = nullptr;
    static uint64_t s_xmenCerebroCaptureTick = UINT64_MAX;
    static uint32_t s_xmenCerebroCaptureBatchCount = 0u;
    static const bool captureXmenCerebro =
        std::getenv("PS2X_XMEN_CEREBRO_CAPTURE") != nullptr;
    if (captureXmenCerebro && batch.debugVsyncTick >= 2255u &&
        s_xmenCerebroCaptureTick == UINT64_MAX)
        s_xmenCerebroCaptureTick = batch.debugVsyncTick;
    if (batch.debugVsyncTick == s_xmenCerebroCaptureTick)
    {
        const uint32_t captureBatch = ++s_xmenCerebroCaptureBatchCount;
        const bool capture = captureBatch == 512u || captureBatch == 2048u ||
                             captureBatch == 4096u || captureBatch == 8192u ||
                             captureBatch == 16384u;
        if (capture)
        {
            constexpr uint32_t width = 640u;
            constexpr uint32_t height = 448u;
            const uint32_t frameBase = GSInternal::framePageBaseToBlock(xmenContext.frame.fbp);
            const uint32_t frameWidth = std::max<uint32_t>(xmenContext.frame.fbw, 1u);
            const std::string path = "xmen-cerebro-tick-" +
                                     std::to_string(s_xmenCerebroCaptureTick) +
                                     "-batch-" + std::to_string(captureBatch) + ".ppm";
            FILE *frameFile = std::fopen(path.c_str(), "wb");
            if (frameFile)
                std::fprintf(frameFile, "P6\n%u %u\n255\n", width, height);
            uint64_t nonzeroPixels = 0u;
            uint32_t minX = width;
            uint32_t minY = height;
            uint32_t maxX = 0u;
            uint32_t maxY = 0u;
            for (uint32_t y = 0u; y < height; ++y)
            {
                for (uint32_t x = 0u; x < width; ++x)
                {
                    const uint32_t pixel = ReadVramUnlocked(xmenContext.frame.psm,
                                                            frameBase,
                                                            frameWidth,
                                                            x,
                                                            y);
                    const uint8_t rgb[3] = {
                        static_cast<uint8_t>(pixel),
                        static_cast<uint8_t>(pixel >> 8u),
                        static_cast<uint8_t>(pixel >> 16u),
                    };
                    if ((rgb[0] | rgb[1] | rgb[2]) != 0u)
                    {
                        ++nonzeroPixels;
                        minX = std::min(minX, x);
                        minY = std::min(minY, y);
                        maxX = std::max(maxX, x);
                        maxY = std::max(maxY, y);
                    }
                    if (frameFile)
                        std::fwrite(rgb, sizeof(rgb), 1u, frameFile);
                }
            }
            if (frameFile)
                std::fclose(frameFile);
            std::fprintf(stdout,
                         "[xmen-cerebro-vram] tick=%llu batch=%u frame=%u/%u/%u "
                         "nonzero=%llu bounds=%u,%u-%u,%u path=%s wrote=%u\n",
                         static_cast<unsigned long long>(s_xmenCerebroCaptureTick),
                         captureBatch,
                         xmenContext.frame.fbp,
                         xmenContext.frame.fbw,
                         static_cast<unsigned>(xmenContext.frame.psm),
                         static_cast<unsigned long long>(nonzeroPixels),
                         minX,
                         minY,
                         maxX,
                         maxY,
                         path.c_str(),
                         frameFile ? 1u : 0u);
            std::fflush(stdout);
        }
    }
    if (xmenTitleRasterGroup < s_xmenTitleRasterTotals.size())
        accumulateXmenRasterStats(s_xmenTitleRasterTotals[xmenTitleRasterGroup], xmenRasterStats);
    if (traceXmenGameplayRaster)
    {
        accumulateXmenRasterStats(s_xmenGameplayRaster.raster, xmenRasterStats);
        if (batch.state.prim.tme)
            accumulateXmenRasterStats(s_xmenGameplayRaster.texturedRaster, xmenRasterStats);
    }
    const bool darkRasterCandidate =
        traceRasterTick &&
        (xmenContext.frame.fbp == 0u || xmenContext.frame.fbp == 140u) &&
        batch.state.prim.type == GS_PRIM_TRISTRIP &&
        xmenRasterStats.framebufferWrites >= 64u &&
        ((batch.state.prim.tme && xmenRasterStats.textureSamples >= 64u &&
          xmenRasterStats.nonzeroCombinedRgbSamples * 20u < xmenRasterStats.textureSamples) ||
         xmenRasterStats.nonzeroRgbWrites * 20u < xmenRasterStats.framebufferWrites);
    if (darkRasterCandidate)
    {
        static std::atomic<uint32_t> s_darkRasterCandidateCount{0u};
        const uint32_t candidateIndex =
            s_darkRasterCandidateCount.fetch_add(1u, std::memory_order_relaxed);
        if (candidateIndex < 256u)
        {
            std::fprintf(stdout,
                         "[xmen-dark-raster] index=%u present=%u tick=%llu gifTag=0x%016llx "
                         "prim=%u/%u/%u/%u/%u frame=%u/%u/%u/0x%x zbuf=%u/%u/%u "
                         "tex0=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u texclut=%u/%u/%u "
                         "texa=%u/%u/%u alpha=0x%llx colclamp=0x%llx clamp=0x%llx "
                         "v0=%d,%d,%.0f/%u,%u,%u,%u "
                         "v1=%d,%d,%.0f/%u,%u,%u,%u "
                         "v2=%d,%d,%.0f/%u,%u,%u,%u "
                         "covered=%llu alphaReject=%llu depthReject=%llu writes=%llu changed=%llu "
                         "nonzeroSource=%llu texture=%llu nonzeroTexture=%llu combined=%llu "
                         "bounds=%u,%u-%u,%u\n",
                         candidateIndex,
                         batch.debugPresentCount,
                         static_cast<unsigned long long>(batch.debugVsyncTick),
                         static_cast<unsigned long long>(batch.debugGifTagLo),
                         static_cast<unsigned>(batch.state.prim.type),
                         batch.state.prim.tme ? 1u : 0u,
                         batch.state.prim.abe ? 1u : 0u,
                         batch.state.prim.fst ? 1u : 0u,
                         batch.state.prim.iip ? 1u : 0u,
                         xmenContext.frame.fbp,
                         xmenContext.frame.fbw,
                         static_cast<unsigned>(xmenContext.frame.psm),
                         xmenContext.frame.fbmsk,
                         xmenContext.zbuf.zbp,
                         static_cast<unsigned>(xmenContext.zbuf.psm),
                         xmenContext.zbuf.zmask ? 1u : 0u,
                         xmenContext.tex0.tbp0,
                         static_cast<unsigned>(xmenContext.tex0.tbw),
                         static_cast<unsigned>(xmenContext.tex0.psm),
                         static_cast<unsigned>(xmenContext.tex0.tw),
                         static_cast<unsigned>(xmenContext.tex0.th),
                         static_cast<unsigned>(xmenContext.tex0.tcc),
                         static_cast<unsigned>(xmenContext.tex0.tfx),
                         xmenContext.tex0.cbp,
                         static_cast<unsigned>(xmenContext.tex0.cpsm),
                         static_cast<unsigned>(xmenContext.tex0.csm),
                         static_cast<unsigned>(xmenContext.tex0.csa),
                         static_cast<unsigned>(batch.state.texclut.cbw),
                         static_cast<unsigned>(batch.state.texclut.cou),
                         static_cast<unsigned>(batch.state.texclut.cov),
                         static_cast<unsigned>(batch.state.texa.ta0),
                         batch.state.texa.aem ? 1u : 0u,
                         static_cast<unsigned>(batch.state.texa.ta1),
                         static_cast<unsigned long long>(xmenContext.alpha),
                         static_cast<unsigned long long>(batch.state.colclamp),
                         static_cast<unsigned long long>(xmenContext.clamp),
                         screenX(0u),
                         screenY(0u),
                         batch.vertices[0u].z,
                         static_cast<unsigned>(batch.vertices[0u].r),
                         static_cast<unsigned>(batch.vertices[0u].g),
                         static_cast<unsigned>(batch.vertices[0u].b),
                         static_cast<unsigned>(batch.vertices[0u].a),
                         screenX(1u),
                         screenY(1u),
                         batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)].z,
                         static_cast<unsigned>(batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)].r),
                         static_cast<unsigned>(batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)].g),
                         static_cast<unsigned>(batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)].b),
                         static_cast<unsigned>(batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)].a),
                         screenX(2u),
                         screenY(2u),
                         batch.vertices[batch.vertexCount - 1u].z,
                         static_cast<unsigned>(batch.vertices[batch.vertexCount - 1u].r),
                         static_cast<unsigned>(batch.vertices[batch.vertexCount - 1u].g),
                         static_cast<unsigned>(batch.vertices[batch.vertexCount - 1u].b),
                         static_cast<unsigned>(batch.vertices[batch.vertexCount - 1u].a),
                         static_cast<unsigned long long>(xmenRasterStats.covered),
                         static_cast<unsigned long long>(xmenRasterStats.alphaRejected),
                         static_cast<unsigned long long>(xmenRasterStats.depthRejected),
                         static_cast<unsigned long long>(xmenRasterStats.framebufferWrites),
                         static_cast<unsigned long long>(xmenRasterStats.framebufferChangedWrites),
                         static_cast<unsigned long long>(xmenRasterStats.nonzeroRgbWrites),
                         static_cast<unsigned long long>(xmenRasterStats.textureSamples),
                         static_cast<unsigned long long>(xmenRasterStats.nonzeroTextureRgbSamples),
                         static_cast<unsigned long long>(xmenRasterStats.nonzeroCombinedRgbSamples),
                         xmenRasterStats.minCoveredX,
                         xmenRasterStats.minCoveredY,
                         xmenRasterStats.maxCoveredX,
                         xmenRasterStats.maxCoveredY);
            std::fflush(stdout);
        }
    }
    const bool traceWorldRegion = overlapsXmenTraceBounds(
        xmenRasterStats, xmenRasterTraceBounds());
    const bool traceHudRegion = overlapsXmenTraceBounds(
        xmenRasterStats, xmenHudTraceBounds());
    const bool regionRasterCandidate =
        traceRasterTick &&
        (traceWorldRegion || traceHudRegion);
    if (regionRasterCandidate)
    {
        static std::atomic<uint32_t> s_regionRasterCandidateCount{0u};
        const uint32_t candidateIndex =
            s_regionRasterCandidateCount.fetch_add(1u, std::memory_order_relaxed);
        if (candidateIndex < 2048u)
        {
            const GSVertex &v0 = batch.vertices[0u];
            const GSVertex &v1 = batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)];
            const GSVertex &v2 = batch.vertices[batch.vertexCount - 1u];
            std::fprintf(stdout,
                         "[xmen-region-raster] index=%u region=%u/%u present=%u tick=%llu "
                         "gifTag=0x%016llx "
                         "prim=%u/%u/%u/%u/%u/%u frame=%u/%u/%u/0x%x zbuf=%u/%u/%u "
                         "tex0=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u texclut=%u/%u/%u "
                         "texa=%u/%u/%u alpha=0x%llx test=0x%llx colclamp=0x%llx clamp=0x%llx "
                         "v0=%d,%d,%.0f/%u,%u,%u,%u "
                         "v1=%d,%d,%.0f/%u,%u,%u,%u "
                         "v2=%d,%d,%.0f/%u,%u,%u,%u "
                         "covered=%llu alphaReject=%llu destAlphaReject=%llu depthReject=%llu "
                         "writes=%llu changed=%llu nonzeroSource=%llu texture=%llu "
                         "nonzeroTexture=%llu combined=%llu bounds=%u,%u-%u,%u\n",
                         candidateIndex,
                         traceWorldRegion ? 1u : 0u,
                         traceHudRegion ? 1u : 0u,
                         batch.debugPresentCount,
                         static_cast<unsigned long long>(batch.debugVsyncTick),
                         static_cast<unsigned long long>(batch.debugGifTagLo),
                         static_cast<unsigned>(batch.state.prim.type),
                         batch.state.prim.tme ? 1u : 0u,
                         batch.state.prim.abe ? 1u : 0u,
                         batch.state.prim.fst ? 1u : 0u,
                         batch.state.prim.iip ? 1u : 0u,
                         batch.state.prim.fge ? 1u : 0u,
                         xmenContext.frame.fbp,
                         xmenContext.frame.fbw,
                         static_cast<unsigned>(xmenContext.frame.psm),
                         xmenContext.frame.fbmsk,
                         xmenContext.zbuf.zbp,
                         static_cast<unsigned>(xmenContext.zbuf.psm),
                         xmenContext.zbuf.zmask ? 1u : 0u,
                         xmenContext.tex0.tbp0,
                         static_cast<unsigned>(xmenContext.tex0.tbw),
                         static_cast<unsigned>(xmenContext.tex0.psm),
                         static_cast<unsigned>(xmenContext.tex0.tw),
                         static_cast<unsigned>(xmenContext.tex0.th),
                         static_cast<unsigned>(xmenContext.tex0.tcc),
                         static_cast<unsigned>(xmenContext.tex0.tfx),
                         xmenContext.tex0.cbp,
                         static_cast<unsigned>(xmenContext.tex0.cpsm),
                         static_cast<unsigned>(xmenContext.tex0.csm),
                         static_cast<unsigned>(xmenContext.tex0.csa),
                         static_cast<unsigned>(batch.state.texclut.cbw),
                         static_cast<unsigned>(batch.state.texclut.cou),
                         static_cast<unsigned>(batch.state.texclut.cov),
                         static_cast<unsigned>(batch.state.texa.ta0),
                         batch.state.texa.aem ? 1u : 0u,
                         static_cast<unsigned>(batch.state.texa.ta1),
                         static_cast<unsigned long long>(xmenContext.alpha),
                         static_cast<unsigned long long>(xmenContext.test),
                         static_cast<unsigned long long>(batch.state.colclamp),
                         static_cast<unsigned long long>(xmenContext.clamp),
                         screenX(0u), screenY(0u), v0.z,
                         static_cast<unsigned>(v0.r), static_cast<unsigned>(v0.g),
                         static_cast<unsigned>(v0.b), static_cast<unsigned>(v0.a),
                         screenX(1u), screenY(1u), v1.z,
                         static_cast<unsigned>(v1.r), static_cast<unsigned>(v1.g),
                         static_cast<unsigned>(v1.b), static_cast<unsigned>(v1.a),
                         screenX(2u), screenY(2u), v2.z,
                         static_cast<unsigned>(v2.r), static_cast<unsigned>(v2.g),
                         static_cast<unsigned>(v2.b), static_cast<unsigned>(v2.a),
                         static_cast<unsigned long long>(xmenRasterStats.covered),
                         static_cast<unsigned long long>(xmenRasterStats.alphaRejected),
                         static_cast<unsigned long long>(xmenRasterStats.destinationAlphaRejected),
                         static_cast<unsigned long long>(xmenRasterStats.depthRejected),
                         static_cast<unsigned long long>(xmenRasterStats.framebufferWrites),
                         static_cast<unsigned long long>(xmenRasterStats.framebufferChangedWrites),
                         static_cast<unsigned long long>(xmenRasterStats.nonzeroRgbWrites),
                         static_cast<unsigned long long>(xmenRasterStats.textureSamples),
                         static_cast<unsigned long long>(xmenRasterStats.nonzeroTextureRgbSamples),
                         static_cast<unsigned long long>(xmenRasterStats.nonzeroCombinedRgbSamples),
                         xmenRasterStats.minCoveredX,
                         xmenRasterStats.minCoveredY,
                         xmenRasterStats.maxCoveredX,
                         xmenRasterStats.maxCoveredY);
            std::fflush(stdout);
        }
    }
    if (traceTargetRaster)
    {
        const uint32_t minIncomingZ = xmenRasterStats.minIncomingZ == UINT32_MAX
            ? 0u
            : xmenRasterStats.minIncomingZ;
        const uint32_t minStoredZ = xmenRasterStats.minStoredZ == UINT32_MAX
            ? 0u
            : xmenRasterStats.minStoredZ;
        std::fprintf(stdout,
                     "[gs-raster-target] index=%u present=%u tick=%llu "
                     "prim=%u/%u/%u/%u/%u frame=%u/%u/%u/0x%x zbuf=%u/%u/%u "
                     "test=0x%016llx tex0=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u "
                     "offset=%d,%d v0=%d,%d,%.0f v1=%d,%d,%.0f v2=%d,%d,%.0f "
                     "covered=%llu alphaReject=%llu destAlphaReject=%llu depthReject=%llu "
                     "fbWrites=%llu changed=%llu nonzeroSource=%llu sourceSum=%llu sourceMax=%u "
                     "depthWrites=%llu textureSamples=%llu nonzeroTexture=%llu nonzeroCombined=%llu "
                     "bounds=%u,%u-%u,%u incomingZ=%u-%u storedZ=%u-%u\n",
                     targetRasterTraceIndex,
                     batch.debugPresentCount,
                     static_cast<unsigned long long>(batch.debugVsyncTick),
                     static_cast<unsigned>(batch.state.prim.type),
                     batch.state.prim.tme ? 1u : 0u,
                     batch.state.prim.abe ? 1u : 0u,
                     batch.state.prim.fst ? 1u : 0u,
                     batch.state.prim.iip ? 1u : 0u,
                     xmenContext.frame.fbp,
                     xmenContext.frame.fbw,
                     static_cast<unsigned>(xmenContext.frame.psm),
                     xmenContext.frame.fbmsk,
                     xmenContext.zbuf.zbp,
                     static_cast<unsigned>(xmenContext.zbuf.psm),
                     xmenContext.zbuf.zmask ? 1u : 0u,
                     static_cast<unsigned long long>(xmenContext.test),
                     xmenContext.tex0.tbp0,
                     static_cast<unsigned>(xmenContext.tex0.tbw),
                     static_cast<unsigned>(xmenContext.tex0.psm),
                     static_cast<unsigned>(xmenContext.tex0.tw),
                     static_cast<unsigned>(xmenContext.tex0.th),
                     static_cast<unsigned>(xmenContext.tex0.tcc),
                     static_cast<unsigned>(xmenContext.tex0.tfx),
                     xmenContext.tex0.cbp,
                     static_cast<unsigned>(xmenContext.tex0.cpsm),
                     static_cast<unsigned>(xmenContext.tex0.csa),
                     submitOfx,
                     submitOfy,
                     screenX(0u),
                     screenY(0u),
                     batch.vertices[0u].z,
                     screenX(1u),
                     screenY(1u),
                     batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)].z,
                     screenX(2u),
                     screenY(2u),
                     batch.vertices[batch.vertexCount - 1u].z,
                     static_cast<unsigned long long>(xmenRasterStats.covered),
                     static_cast<unsigned long long>(xmenRasterStats.alphaRejected),
                     static_cast<unsigned long long>(xmenRasterStats.destinationAlphaRejected),
                     static_cast<unsigned long long>(xmenRasterStats.depthRejected),
                     static_cast<unsigned long long>(xmenRasterStats.framebufferWrites),
                     static_cast<unsigned long long>(xmenRasterStats.framebufferChangedWrites),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroRgbWrites),
                     static_cast<unsigned long long>(xmenRasterStats.sourceChannelSum),
                     xmenRasterStats.maxSourceChannel,
                     static_cast<unsigned long long>(xmenRasterStats.depthWrites),
                     static_cast<unsigned long long>(xmenRasterStats.textureSamples),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroTextureRgbSamples),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroCombinedRgbSamples),
                     xmenRasterStats.minCoveredX == UINT32_MAX ? 0u : xmenRasterStats.minCoveredX,
                     xmenRasterStats.minCoveredY == UINT32_MAX ? 0u : xmenRasterStats.minCoveredY,
                     xmenRasterStats.maxCoveredX,
                     xmenRasterStats.maxCoveredY,
                     minIncomingZ,
                     xmenRasterStats.maxIncomingZ,
                     minStoredZ,
                     xmenRasterStats.maxStoredZ);
        std::fflush(stdout);
    }
    if (traceTopChroma && xmenRasterStats.topChromaFramebufferWrites != 0u)
    {
        const GSContext &ctx = batch.state.context;
        std::fprintf(stdout,
                     "[xmen-top-chroma] submit=%u present=%u tick=%llu writes=%llu source=%llu "
                     "bounds=%u,%u-%u,%u frame=%u/%u/0x%x/0x%x prim=%u/%u/%u/%u/%u "
                     "tex0=%u/%u/0x%x/%u/%u/%u/%u cbp=%u/0x%x/%u/%u/%u "
                     "tex1=0x%llx clamp=0x%llx alpha=0x%llx test=0x%llx texa=%u/%u/%u "
                     "texclut=%u/%u/%u colclamp=0x%llx "
                     "v0=%.2f,%.2f,%.0f/%.5g,%.5g,%.5g/%u,%u/%u,%u,%u,%u "
                     "v1=%.2f,%.2f,%.0f/%.5g,%.5g,%.5g/%u,%u/%u,%u,%u,%u "
                     "v2=%.2f,%.2f,%.0f/%.5g,%.5g,%.5g/%u,%u/%u,%u,%u,%u\n",
                     submitProbeIndex,
                     batch.debugPresentCount,
                     static_cast<unsigned long long>(batch.debugVsyncTick),
                     static_cast<unsigned long long>(xmenRasterStats.topChromaFramebufferWrites),
                     static_cast<unsigned long long>(xmenRasterStats.topChromaSourceWrites),
                     xmenRasterStats.minTopChromaX,
                     xmenRasterStats.minTopChromaY,
                     xmenRasterStats.maxTopChromaX,
                     xmenRasterStats.maxTopChromaY,
                     ctx.frame.fbp,
                     ctx.frame.fbw,
                     static_cast<unsigned>(ctx.frame.psm),
                     ctx.frame.fbmsk,
                     static_cast<unsigned>(batch.state.prim.type),
                     batch.state.prim.tme ? 1u : 0u,
                     batch.state.prim.fst ? 1u : 0u,
                     batch.state.prim.abe ? 1u : 0u,
                     batch.state.prim.fge ? 1u : 0u,
                     ctx.tex0.tbp0,
                     static_cast<unsigned>(ctx.tex0.tbw),
                     static_cast<unsigned>(ctx.tex0.psm),
                     static_cast<unsigned>(ctx.tex0.tw),
                     static_cast<unsigned>(ctx.tex0.th),
                     static_cast<unsigned>(ctx.tex0.tcc),
                     static_cast<unsigned>(ctx.tex0.tfx),
                     ctx.tex0.cbp,
                     static_cast<unsigned>(ctx.tex0.cpsm),
                     static_cast<unsigned>(ctx.tex0.csm),
                     static_cast<unsigned>(ctx.tex0.csa),
                     static_cast<unsigned>(ctx.tex0.cld),
                     static_cast<unsigned long long>(ctx.tex1),
                     static_cast<unsigned long long>(ctx.clamp),
                     static_cast<unsigned long long>(ctx.alpha),
                     static_cast<unsigned long long>(ctx.test),
                     static_cast<unsigned>(batch.state.texa.ta0),
                     batch.state.texa.aem ? 1u : 0u,
                     static_cast<unsigned>(batch.state.texa.ta1),
                     static_cast<unsigned>(batch.state.texclut.cbw),
                     static_cast<unsigned>(batch.state.texclut.cou),
                     static_cast<unsigned>(batch.state.texclut.cov),
                     static_cast<unsigned long long>(batch.state.colclamp),
                     batch.vertices[0].x - submitOfx,
                     batch.vertices[0].y - submitOfy,
                     batch.vertices[0].z,
                     batch.vertices[0].s,
                     batch.vertices[0].t,
                     batch.vertices[0].q,
                     static_cast<unsigned>(batch.vertices[0].u),
                     static_cast<unsigned>(batch.vertices[0].v),
                     static_cast<unsigned>(batch.vertices[0].r),
                     static_cast<unsigned>(batch.vertices[0].g),
                     static_cast<unsigned>(batch.vertices[0].b),
                     static_cast<unsigned>(batch.vertices[0].a),
                     batch.vertices[1].x - submitOfx,
                     batch.vertices[1].y - submitOfy,
                     batch.vertices[1].z,
                     batch.vertices[1].s,
                     batch.vertices[1].t,
                     batch.vertices[1].q,
                     static_cast<unsigned>(batch.vertices[1].u),
                     static_cast<unsigned>(batch.vertices[1].v),
                     static_cast<unsigned>(batch.vertices[1].r),
                     static_cast<unsigned>(batch.vertices[1].g),
                     static_cast<unsigned>(batch.vertices[1].b),
                     static_cast<unsigned>(batch.vertices[1].a),
                     batch.vertices[2].x - submitOfx,
                     batch.vertices[2].y - submitOfy,
                     batch.vertices[2].z,
                     batch.vertices[2].s,
                     batch.vertices[2].t,
                     batch.vertices[2].q,
                     static_cast<unsigned>(batch.vertices[2].u),
                     static_cast<unsigned>(batch.vertices[2].v),
                     static_cast<unsigned>(batch.vertices[2].r),
                     static_cast<unsigned>(batch.vertices[2].g),
                     static_cast<unsigned>(batch.vertices[2].b),
                     static_cast<unsigned>(batch.vertices[2].a));
        std::fflush(stdout);
    }
    static bool dumpedTopChromaTexture = false;
    if (traceTopChroma && !dumpedTopChromaTexture &&
        batch.state.prim.tme && xmenRasterStats.topChromaSourceWrites != 0u &&
        (xmenContext.tex0.psm == GS_PSM_T8 ||
         xmenContext.tex0.psm == GS_PSM_T8H ||
         xmenContext.tex0.psm == GS_PSM_T4 ||
         xmenContext.tex0.psm == GS_PSM_T4HL ||
         xmenContext.tex0.psm == GS_PSM_T4HH))
    {
        dumpedTopChromaTexture = true;
        const uint32_t width = 1u << std::min<uint32_t>(xmenContext.tex0.tw, 10u);
        const uint32_t height = 1u << std::min<uint32_t>(xmenContext.tex0.th, 10u);
        std::FILE *imageFile = std::fopen("xmen-top-chroma-source.ppm", "wb");
        std::FILE *indexFile = std::fopen("xmen-top-chroma-indices.pgm", "wb");
        std::FILE *paletteFile = std::fopen("xmen-top-chroma-palette.ppm", "wb");
        if (imageFile)
            std::fprintf(imageFile, "P6\n%u %u\n255\n", width, height);
        if (indexFile)
            std::fprintf(indexFile, "P5\n%u %u\n255\n", width, height);
        if (paletteFile)
            std::fprintf(paletteFile, "P6\n16 16\n255\n");

        uint32_t highChromaPaletteEntries = 0u;
        for (uint32_t index = 0u; index < 256u; ++index)
        {
            const uint32_t color = LookupCLUT(batch.state, static_cast<uint8_t>(index),
                                              xmenContext.tex0.cbp, xmenContext.tex0.cpsm,
                                              xmenContext.tex0.csm, xmenContext.tex0.csa,
                                              xmenContext.tex0.psm);
            const uint8_t rgb[3] = {
                static_cast<uint8_t>(color),
                static_cast<uint8_t>(color >> 8u),
                static_cast<uint8_t>(color >> 16u),
            };
            if (isHighChroma(rgb[0], rgb[1], rgb[2]))
                ++highChromaPaletteEntries;
            if (paletteFile)
                std::fwrite(rgb, sizeof(rgb), 1u, paletteFile);
        }

        uint64_t highChromaTexels = 0u;
        for (uint32_t y = 0u; y < height; ++y)
        {
            for (uint32_t x = 0u; x < width; ++x)
            {
                const uint8_t index = static_cast<uint8_t>(
                    ReadVramUnlocked(xmenContext.tex0.psm, xmenContext.tex0.tbp0,
                                     xmenContext.tex0.tbw, x, y));
                const uint32_t color = LookupCLUT(batch.state, index,
                                                  xmenContext.tex0.cbp, xmenContext.tex0.cpsm,
                                                  xmenContext.tex0.csm, xmenContext.tex0.csa,
                                                  xmenContext.tex0.psm);
                const uint8_t rgb[3] = {
                    static_cast<uint8_t>(color),
                    static_cast<uint8_t>(color >> 8u),
                    static_cast<uint8_t>(color >> 16u),
                };
                if (isHighChroma(rgb[0], rgb[1], rgb[2]))
                    ++highChromaTexels;
                if (imageFile)
                    std::fwrite(rgb, sizeof(rgb), 1u, imageFile);
                if (indexFile)
                    std::fwrite(&index, sizeof(index), 1u, indexFile);
            }
        }
        if (imageFile)
            std::fclose(imageFile);
        if (indexFile)
            std::fclose(indexFile);
        if (paletteFile)
            std::fclose(paletteFile);
        std::fprintf(stdout,
                     "[xmen-top-chroma-dump] tex0=%u/%u/%u/%u/%u cbp=%u/%u/%u/%u "
                     "size=%ux%u highPalette=%u highTexels=%llu wrote=%u/%u/%u\n",
                     xmenContext.tex0.tbp0,
                     static_cast<unsigned>(xmenContext.tex0.tbw),
                     static_cast<unsigned>(xmenContext.tex0.psm),
                     static_cast<unsigned>(xmenContext.tex0.tw),
                     static_cast<unsigned>(xmenContext.tex0.th),
                     xmenContext.tex0.cbp,
                     static_cast<unsigned>(xmenContext.tex0.cpsm),
                     static_cast<unsigned>(xmenContext.tex0.csm),
                     static_cast<unsigned>(xmenContext.tex0.csa),
                     width, height, highChromaPaletteEntries,
                     static_cast<unsigned long long>(highChromaTexels),
                     imageFile ? 1u : 0u, indexFile ? 1u : 0u, paletteFile ? 1u : 0u);
        std::fflush(stdout);
    }
    if (xmenTitleRasterGroup == 1u && xmenTitleRasterTraceIndex == 264u)
    {
        for (uint32_t group = 0u; group < s_xmenTitleRasterTotals.size(); ++group)
        {
            const XmenRasterProbeStats &total = s_xmenTitleRasterTotals[group];
            std::fprintf(stdout,
                         "[xmen-title-world-total] kind=%s batches=%u frame=%u/%u/%u/0x%x covered=%llu alphaReject=%llu destAlphaReject=%llu depthReject=%llu fbWrites=%llu nonzeroSource=%llu changed=%llu storedNonzero=%llu sourceSum=%llu sourceMax=%u depthWrites=%llu textureSamples=%llu nonzeroTexture=%llu nonzeroCombined=%llu bounds=%u,%u-%u,%u\n",
                         group == 0u ? "untextured" : "textured",
                         s_xmenTitleRasterTraceCounts[group].load(std::memory_order_relaxed),
                         xmenContext.frame.fbp,
                         xmenContext.frame.fbw,
                         static_cast<unsigned>(xmenContext.frame.psm),
                         xmenContext.frame.fbmsk,
                         static_cast<unsigned long long>(total.covered),
                         static_cast<unsigned long long>(total.alphaRejected),
                         static_cast<unsigned long long>(total.destinationAlphaRejected),
                         static_cast<unsigned long long>(total.depthRejected),
                         static_cast<unsigned long long>(total.framebufferWrites),
                         static_cast<unsigned long long>(total.nonzeroRgbWrites),
                         static_cast<unsigned long long>(total.framebufferChangedWrites),
                         static_cast<unsigned long long>(total.nonzeroFramebufferWrites),
                         static_cast<unsigned long long>(total.sourceChannelSum),
                         total.maxSourceChannel,
                         static_cast<unsigned long long>(total.depthWrites),
                         static_cast<unsigned long long>(total.textureSamples),
                         static_cast<unsigned long long>(total.nonzeroTextureRgbSamples),
                         static_cast<unsigned long long>(total.nonzeroCombinedRgbSamples),
                         total.minCoveredX == UINT32_MAX ? 0u : total.minCoveredX,
                         total.minCoveredY == UINT32_MAX ? 0u : total.minCoveredY,
                         total.maxCoveredX,
                         total.maxCoveredY);
        }
        constexpr uint32_t width = 640u;
        constexpr uint32_t height = 448u;
        const uint32_t frameBase = GSInternal::framePageBaseToBlock(xmenContext.frame.fbp);
        const uint32_t frameWidth = std::max<uint32_t>(xmenContext.frame.fbw, 1u);
        uint64_t nonzeroPixels = 0u;
        uint64_t channelSum = 0u;
        uint32_t maxChannel = 0u;
        uint32_t minX = width;
        uint32_t minY = height;
        uint32_t maxX = 0u;
        uint32_t maxY = 0u;
        FILE *frameFile = std::fopen("xmen-title-world-final.ppm", "wb");
        if (frameFile)
            std::fprintf(frameFile, "P6\n%u %u\n255\n", width, height);
        for (uint32_t y = 0u; y < height; ++y)
        {
            for (uint32_t x = 0u; x < width; ++x)
            {
                const uint32_t pixel = ReadVramUnlocked(xmenContext.frame.psm, frameBase, frameWidth, x, y);
                const uint8_t rgb[3] = {
                    static_cast<uint8_t>(pixel),
                    static_cast<uint8_t>(pixel >> 8u),
                    static_cast<uint8_t>(pixel >> 16u),
                };
                const uint32_t peak = std::max<uint32_t>(rgb[0], std::max<uint32_t>(rgb[1], rgb[2]));
                channelSum += static_cast<uint64_t>(rgb[0]) + rgb[1] + rgb[2];
                maxChannel = std::max(maxChannel, peak);
                if ((rgb[0] | rgb[1] | rgb[2]) != 0u)
                {
                    ++nonzeroPixels;
                    minX = std::min(minX, x);
                    minY = std::min(minY, y);
                    maxX = std::max(maxX, x);
                    maxY = std::max(maxY, y);
                }
                if (frameFile)
                    std::fwrite(rgb, sizeof(rgb), 1u, frameFile);
            }
        }
        if (frameFile)
            std::fclose(frameFile);
        std::fprintf(stdout,
                     "[xmen-title-world-final] present=%u tick=%llu nonzero=%llu channelSum=%llu maxChannel=%u bounds=%u,%u-%u,%u wrote=%u\n",
                     batch.debugPresentCount,
                     static_cast<unsigned long long>(batch.debugVsyncTick),
                     static_cast<unsigned long long>(nonzeroPixels),
                     static_cast<unsigned long long>(channelSum),
                     maxChannel,
                     minX,
                     minY,
                     maxX,
                     maxY,
                     frameFile ? 1u : 0u);
        std::fflush(stdout);
    }
    if (xmenTitleRasterTraceIndex < 32u)
    {
        const uint32_t minIncomingZ = xmenRasterStats.minIncomingZ == UINT32_MAX ? 0u : xmenRasterStats.minIncomingZ;
        const uint32_t minStoredZ = xmenRasterStats.minStoredZ == UINT32_MAX ? 0u : xmenRasterStats.minStoredZ;
        std::fprintf(stdout,
                     "[xmen-title-raster] kind=%s index=%u present=%u tick=%llu frame=%u/%u/%u/0x%x tme=%u tex0=%u/%u/%u/%u/%u cbp=%u/%u/%u/%u covered=%llu alphaReject=%llu destAlphaReject=%llu depthReject=%llu fbWrites=%llu nonzeroSource=%llu changed=%llu storedNonzero=%llu sourceSum=%llu sourceMax=%u depthWrites=%llu textureSamples=%llu nonzeroTexture=%llu nonzeroCombined=%llu bounds=%u,%u-%u,%u incomingZ=%u-%u storedZ=%u-%u\n",
                     xmenTitleRasterGroup == 0u ? "untextured" : "textured",
                     xmenTitleRasterTraceIndex,
                     batch.debugPresentCount,
                     static_cast<unsigned long long>(batch.debugVsyncTick),
                     batch.state.context.frame.fbp,
                     batch.state.context.frame.fbw,
                     static_cast<unsigned>(batch.state.context.frame.psm),
                     batch.state.context.frame.fbmsk,
                     batch.state.prim.tme ? 1u : 0u,
                     batch.state.context.tex0.tbp0,
                     static_cast<unsigned>(batch.state.context.tex0.tbw),
                     static_cast<unsigned>(batch.state.context.tex0.psm),
                     static_cast<unsigned>(batch.state.context.tex0.tw),
                     static_cast<unsigned>(batch.state.context.tex0.th),
                     batch.state.context.tex0.cbp,
                     static_cast<unsigned>(batch.state.context.tex0.cpsm),
                     static_cast<unsigned>(batch.state.context.tex0.csm),
                     static_cast<unsigned>(batch.state.context.tex0.csa),
                     static_cast<unsigned long long>(xmenRasterStats.covered),
                     static_cast<unsigned long long>(xmenRasterStats.alphaRejected),
                     static_cast<unsigned long long>(xmenRasterStats.destinationAlphaRejected),
                     static_cast<unsigned long long>(xmenRasterStats.depthRejected),
                     static_cast<unsigned long long>(xmenRasterStats.framebufferWrites),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroRgbWrites),
                     static_cast<unsigned long long>(xmenRasterStats.framebufferChangedWrites),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroFramebufferWrites),
                     static_cast<unsigned long long>(xmenRasterStats.sourceChannelSum),
                     xmenRasterStats.maxSourceChannel,
                     static_cast<unsigned long long>(xmenRasterStats.depthWrites),
                     static_cast<unsigned long long>(xmenRasterStats.textureSamples),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroTextureRgbSamples),
                     static_cast<unsigned long long>(xmenRasterStats.nonzeroCombinedRgbSamples),
                     xmenRasterStats.minCoveredX == UINT32_MAX ? 0u : xmenRasterStats.minCoveredX,
                     xmenRasterStats.minCoveredY == UINT32_MAX ? 0u : xmenRasterStats.minCoveredY,
                     xmenRasterStats.maxCoveredX,
                     xmenRasterStats.maxCoveredY,
                     minIncomingZ,
                     xmenRasterStats.maxIncomingZ,
                     minStoredZ,
                     xmenRasterStats.maxStoredZ);
        std::fflush(stdout);
    }
    if (profileCpuRaster)
    {
        const uint64_t rasterNanoseconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - rasterStart).count());
        const uint64_t submitCount =
            s_profileSubmitCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
        const uint64_t totalRasterNanoseconds =
            s_profileRasterNanoseconds.fetch_add(rasterNanoseconds, std::memory_order_relaxed) +
            rasterNanoseconds;
        if ((submitCount & 511u) == 0u)
        {
            std::fprintf(stderr,
                         "[gs:cpu-profile] submits=%llu raster-ms=%.3f average-us=%.3f\n",
                         static_cast<unsigned long long>(submitCount),
                         static_cast<double>(totalRasterNanoseconds) / 1000000.0,
                         static_cast<double>(totalRasterNanoseconds) /
                             static_cast<double>(submitCount) / 1000.0);
        }
    }
    if (traceSubmitProbe)
    {
        uint32_t changed = 0u;
        uint32_t nonzeroAfter = 0u;
        std::array<uint32_t, 8> afterSubmitSamples{};
        for (size_t i = 0; i < afterSubmitSamples.size(); ++i)
        {
            afterSubmitSamples[i] = readSubmitSample(sampleX[i], sampleY[i]);
            if (afterSubmitSamples[i] != beforeSubmitSamples[i])
                ++changed;
            if (afterSubmitSamples[i] != 0u)
                ++nonzeroAfter;
        }
        const GSVertex &v0 = batch.vertices[0u];
        const GSVertex &v1 = batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)];
        std::fprintf(stderr,
                     "[gs:cpu-submit-probe] idx=%u prim=%u tme=%u abe=%u fst=%u ctxt=%u vertices=%u "
                     "frame=%u/%u/%u/%08x tex=%u/%u/%u/%u/%u cbp=%u test=%016llx alpha=%016llx "
                     "xy=%d,%d->%d,%d changed=%u nzAfter=%u "
                     "p0=%08x/%08x p1=%08x/%08x p2=%08x/%08x p3=%08x/%08x "
                     "p4=%08x/%08x p5=%08x/%08x p6=%08x/%08x p7=%08x/%08x\n",
                     submitProbeIndex,
                     static_cast<unsigned>(batch.state.prim.type),
                     static_cast<unsigned>(batch.state.prim.tme),
                     static_cast<unsigned>(batch.state.prim.abe),
                     static_cast<unsigned>(batch.state.prim.fst),
                     static_cast<unsigned>(batch.state.prim.ctxt),
                     static_cast<unsigned>(batch.vertexCount),
                     submitCtx.frame.fbp,
                     submitCtx.frame.fbw,
                     submitCtx.frame.psm,
                     submitCtx.frame.fbmsk,
                     submitCtx.tex0.tbp0,
                     static_cast<unsigned>(submitCtx.tex0.tbw),
                     static_cast<unsigned>(submitCtx.tex0.psm),
                     static_cast<unsigned>(submitCtx.tex0.tw),
                     static_cast<unsigned>(submitCtx.tex0.th),
                     submitCtx.tex0.cbp,
                     static_cast<unsigned long long>(submitCtx.test),
                     static_cast<unsigned long long>(submitCtx.alpha),
                     static_cast<int>(std::lround(v0.x)) - submitOfx,
                     static_cast<int>(std::lround(v0.y)) - submitOfy,
                     static_cast<int>(std::lround(v1.x)) - submitOfx,
                     static_cast<int>(std::lround(v1.y)) - submitOfy,
                     changed,
                     nonzeroAfter,
                     beforeSubmitSamples[0], afterSubmitSamples[0],
                     beforeSubmitSamples[1], afterSubmitSamples[1],
                     beforeSubmitSamples[2], afterSubmitSamples[2],
                     beforeSubmitSamples[3], afterSubmitSamples[3],
                     beforeSubmitSamples[4], afterSubmitSamples[4],
                     beforeSubmitSamples[5], afterSubmitSamples[5],
                     beforeSubmitSamples[6], afterSubmitSamples[6],
                     beforeSubmitSamples[7], afterSubmitSamples[7]);
    }
    if (traceXmenLegal)
    {
        const GSDrawState &state = batch.state;
        const GSContext &ctx = state.context;
        static bool dumpedLegalTexture = false;
        if (!dumpedLegalTexture)
        {
            dumpedLegalTexture = true;
            const auto dumpTexel = [&](uint32_t x, uint32_t y)
            {
                const uint32_t index = ReadVramUnlocked(ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, x, y);
                const uint32_t color = LookupCLUT(state, static_cast<uint8_t>(index), ctx.tex0.cbp,
                                                  ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa, ctx.tex0.psm);
                std::fprintf(stderr, "[xmen-gs-legal-sample] xy=%u,%u index=%u color=%08x\n",
                             x, y, index, color);
            };
            dumpTexel(0u, 0u);
            dumpTexel(100u, 0u);
            dumpTexel(100u, 100u);
            dumpTexel(256u, 200u);
            dumpTexel(511u, 383u);
            std::fprintf(stderr,
                         "[xmen-gs-legal-clut] cbp=%u cpsm=%u csm=%u csa=%u texclut=%u/%u/%u "
                         "raw0=%08x raw37=%08x raw104=%08x raw205=%08x\n",
                         ctx.tex0.cbp, static_cast<unsigned>(ctx.tex0.cpsm),
                         static_cast<unsigned>(ctx.tex0.csm), static_cast<unsigned>(ctx.tex0.csa),
                         static_cast<unsigned>(state.texclut.cbw), static_cast<unsigned>(state.texclut.cou),
                         static_cast<unsigned>(state.texclut.cov),
                         GSMem::ReadCT32(m_vram, ctx.tex0.cbp, 1u, 0u, 0u),
                         GSMem::ReadCT32(m_vram, ctx.tex0.cbp, 1u, 5u, 2u),
                         GSMem::ReadCT32(m_vram, ctx.tex0.cbp, 1u, 8u, 6u),
                         GSMem::ReadCT32(m_vram, ctx.tex0.cbp, 1u, 13u, 12u));
        }
        static bool dumpedLegalTextureImage = false;
        if (!dumpedLegalTextureImage && ctx.tex0.tbp0 == 11200u && ctx.tex0.cbp == 12224u)
        {
            dumpedLegalTextureImage = true;
            std::FILE *imageFile = std::fopen("xmen-legal-texture.ppm", "wb");
            std::FILE *indexFile = std::fopen("xmen-legal-texture-indices.pgm", "wb");
            if (imageFile)
                std::fprintf(imageFile, "P6\n512 512\n255\n");
            if (indexFile)
                std::fprintf(indexFile, "P5\n512 512\n255\n");
            for (uint32_t y = 0u; y < 512u; ++y)
            {
                for (uint32_t x = 0u; x < 512u; ++x)
                {
                    const uint8_t index = static_cast<uint8_t>(
                        ReadVramUnlocked(ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw, x, y));
                    const uint32_t color = LookupCLUT(state, index, ctx.tex0.cbp,
                                                      ctx.tex0.cpsm, ctx.tex0.csm,
                                                      ctx.tex0.csa, ctx.tex0.psm);
                    const uint8_t rgb[3] = {
                        static_cast<uint8_t>(color),
                        static_cast<uint8_t>(color >> 8u),
                        static_cast<uint8_t>(color >> 16u),
                    };
                    if (imageFile)
                        std::fwrite(rgb, sizeof(rgb), 1u, imageFile);
                    if (indexFile)
                        std::fwrite(&index, sizeof(index), 1u, indexFile);
                }
            }
            if (imageFile)
                std::fclose(imageFile);
            if (indexFile)
                std::fclose(indexFile);
            std::fprintf(stderr,
                         "[xmen-gs-legal-texture-dump] tbp=%u cbp=%u wrote=%u/%u\n",
                         ctx.tex0.tbp0, ctx.tex0.cbp,
                         imageFile ? 1u : 0u, indexFile ? 1u : 0u);
        }
        static bool dumpedMissingLegalPalette = false;
        if (!dumpedMissingLegalPalette && ctx.tex0.tbp0 == 12256u && ctx.tex0.cbp == 12512u)
        {
            dumpedMissingLegalPalette = true;
            std::fprintf(stderr,
                         "[xmen-gs-legal-missing-palette] tbp=%u tbw=%u psm=%u cbp=%u cpsm=%u csm=%u csa=%u texclut=%u/%u/%u\n",
                         ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
                         static_cast<unsigned>(ctx.tex0.psm), ctx.tex0.cbp,
                         static_cast<unsigned>(ctx.tex0.cpsm), static_cast<unsigned>(ctx.tex0.csm),
                         static_cast<unsigned>(ctx.tex0.csa), static_cast<unsigned>(state.texclut.cbw),
                         static_cast<unsigned>(state.texclut.cou), static_cast<unsigned>(state.texclut.cov));
            constexpr uint32_t sampleCoords[][2] = {
                {0u, 0u}, {100u, 0u}, {100u, 100u}, {256u, 200u}, {511u, 383u}
            };
            for (const auto &coord : sampleCoords)
            {
                const uint32_t index = ReadVramUnlocked(ctx.tex0.psm, ctx.tex0.tbp0, ctx.tex0.tbw,
                                                        coord[0], coord[1]);
                const uint32_t color = LookupCLUT(state, static_cast<uint8_t>(index), ctx.tex0.cbp,
                                                  ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa,
                                                  ctx.tex0.psm);
                std::fprintf(stderr,
                             "[xmen-gs-legal-missing-palette-sample] xy=%u,%u index=%u color=%08x\n",
                             coord[0], coord[1], index, color);
            }
            for (uint32_t index = 0u; index < 256u; index += 16u)
            {
                const uint32_t color = LookupCLUT(state, static_cast<uint8_t>(index), ctx.tex0.cbp,
                                                  ctx.tex0.cpsm, ctx.tex0.csm, ctx.tex0.csa,
                                                  ctx.tex0.psm);
                std::fprintf(stderr, "[xmen-gs-legal-missing-palette-entry] index=%u color=%08x\n",
                             index, color);
            }
        }
        static std::atomic<uint32_t> xmenLegalRasterLogCount{0u};
        if (xmenLegalRasterLogCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            const GSVertex &v0 = batch.vertices[0u];
            const GSVertex &v1 = batch.vertices[std::min<uint32_t>(1u, batch.vertexCount - 1u)];
            const GSVertex &v2 = batch.vertices[batch.vertexCount - 1u];
            const int ofx = ctx.xyoffset.ofx >> 4;
            const int ofy = ctx.xyoffset.ofy >> 4;
            const auto readFrame = [&](uint32_t x, uint32_t y) {
                return ReadVramUnlocked(ctx.frame.psm, GSInternal::framePageBaseToBlock(ctx.frame.fbp),
                                        std::max<uint32_t>(ctx.frame.fbw, 1u), x, y);
            };
            std::fprintf(stderr,
                     "[xmen-gs-legal-raster] prim=%u tme=%u abe=%u fst=%u iip=%u fge=%u test=%016llx alpha=%016llx "
                     "frame=%u/%u/%u/%08x zbuf=%u/%u/%u tex0=%u/%u/%u/%u/%u before=%08x after=%08x "
                     "offset=%d,%d scissor=%u,%u-%u,%u "
                     "v0=%.1f,%.1f(%.1f,%.1f)/%.3f,%.3f,%.3f "
                     "v1=%.1f,%.1f(%.1f,%.1f)/%.3f,%.3f,%.3f "
                     "v2=%.1f,%.1f(%.1f,%.1f),%.0f/%u,%u,%u,%u/%.3f,%.3f,%.3f "
                     "pixels=%08x,%08x,%08x,%08x\n",
                     static_cast<unsigned>(state.prim.type), static_cast<unsigned>(state.prim.tme),
                     static_cast<unsigned>(state.prim.abe), static_cast<unsigned>(state.prim.fst),
                     static_cast<unsigned>(state.prim.iip), static_cast<unsigned>(state.prim.fge),
                     static_cast<unsigned long long>(ctx.test), static_cast<unsigned long long>(ctx.alpha),
                     ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, ctx.frame.fbmsk,
                     ctx.zbuf.zbp, ctx.zbuf.psm, static_cast<unsigned>(ctx.zbuf.zmask),
                     ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw), static_cast<unsigned>(ctx.tex0.psm),
                     static_cast<unsigned>(ctx.tex0.tw), static_cast<unsigned>(ctx.tex0.th),
                     before, readTracePixel(), ofx, ofy,
                     ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1,
                     v0.x, v0.y, v0.x - ofx, v0.y - ofy, v0.s, v0.t, v0.q,
                     v1.x, v1.y, v1.x - ofx, v1.y - ofy, v1.s, v1.t, v1.q,
                     v2.x, v2.y, v2.x - ofx, v2.y - ofy, v2.z, v2.r, v2.g, v2.b, v2.a,
                     v2.s, v2.t, v2.q,
                     readFrame(0u, 0u), readFrame(100u, 100u),
                         readFrame(320u, 224u), readFrame(639u, 447u));
        }
    }
}

void GSCpuBackend::Flush()
{
    // CPU backend is immediate. GPU backends may submit command buffers here.
}

void GSCpuBackend::TextureFlush()
{
    // CPU texture reads are coherent with local memory. Future cached/GPU
    // backends use this boundary to invalidate texture views.
}

void GSCpuBackend::Sync(GSSyncReason)
{
    // CPU backend is immediate. GPU backends may wait on fences/readbacks here.
}

uint32_t GSCpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return ReadVramUnlocked(psm, base, bw, x, y);
}

uint32_t GSCpuBackend::ReadVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    if (!m_vram)
        return 0u;
    return m_readVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y);
}

void GSCpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    WriteVramUnlocked(psm, base, bw, x, y, value);
}

void GSCpuBackend::WriteVramUnlocked(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    if (!m_vram)
        return;
    m_writeVramFuncs[psm & 0x3Fu](m_vram, base, bw, x, y, value);
}

void GSCpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || m_vramSize == 0u)
    {
        out.clear();
        return;
    }
    out.resize(m_vramSize);
    std::memcpy(out.data(), m_vram, m_vramSize);
}

GSTransferSnapshot GSCpuBackend::GetTransferSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    GSTransferSnapshot result = m_transferState;
    result.localToHostPendingBytes = m_localToHostReadPos < m_localToHostBuffer.size()
                                         ? m_localToHostBuffer.size() - m_localToHostReadPos
                                         : 0u;
    return result;
}

void GSCpuBackend::DrawPrimitive(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    s_xmenLastDrawPresent.store(batch.debugPresentCount, std::memory_order_relaxed);
    s_xmenLastDrawTick.store(batch.debugVsyncTick, std::memory_order_relaxed);
    const uint32_t xmenFrameBase = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    if (traceXmenVramRegion() && isXmenVramRegionBlock(xmenFrameBase))
    {
        const uint32_t traceIndex = s_xmenVramDrawTraceCount.fetch_add(1u, std::memory_order_relaxed);
        if (traceIndex < 256u)
        {
            std::fprintf(stdout,
                         "[xmen-vram-draw] index=%u present=%u tick=%llu frame=%u/%u/%u/0x%x "
                         "base=%u prim=%u/%u/%u tex0=%u/%u/0x%x cbp=%u\n",
                         traceIndex, batch.debugPresentCount,
                         static_cast<unsigned long long>(batch.debugVsyncTick),
                         ctx.frame.fbp, ctx.frame.fbw, static_cast<unsigned>(ctx.frame.psm),
                         ctx.frame.fbmsk, xmenFrameBase, static_cast<unsigned>(state.prim.type),
                         state.prim.tme ? 1u : 0u, state.prim.abe ? 1u : 0u,
                         ctx.tex0.tbp0, static_cast<unsigned>(ctx.tex0.tbw),
                         static_cast<unsigned>(ctx.tex0.psm), ctx.tex0.cbp);
            std::fflush(stdout);
        }
    }
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << batch.vertices[0].x << "," << batch.vertices[0].y << ")"
                      << " uv0=(" << (batch.vertices[0].u >> 4) << "," << (batch.vertices[0].v >> 4) << ")"
                      << " stq0=(" << batch.vertices[0].s << "," << batch.vertices[0].t << "," << batch.vertices[0].q << ")"
                      << " v1=(" << batch.vertices[1].x << "," << batch.vertices[1].y << ")"
                      << " uv1=(" << (batch.vertices[1].u >> 4) << "," << (batch.vertices[1].v >> 4) << ")"
                      << " stq1=(" << batch.vertices[1].s << "," << batch.vertices[1].t << "," << batch.vertices[1].q << ")"
                      << " v2=(" << batch.vertices[2].x << "," << batch.vertices[2].y << ")"
                      << " uv2=(" << (batch.vertices[2].u >> 4) << "," << (batch.vertices[2].v >> 4) << ")"
                      << " stq2=(" << batch.vertices[2].s << "," << batch.vertices[2].t << "," << batch.vertices[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(batch.vertices[0].r) << ","
                      << static_cast<uint32_t>(batch.vertices[0].g) << ","
                      << static_cast<uint32_t>(batch.vertices[0].b) << ","
                      << static_cast<uint32_t>(batch.vertices[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(batch.vertices[1].r) << ","
                      << static_cast<uint32_t>(batch.vertices[1].g) << ","
                      << static_cast<uint32_t>(batch.vertices[1].b) << ","
                      << static_cast<uint32_t>(batch.vertices[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(batch.vertices[2].r) << ","
                      << static_cast<uint32_t>(batch.vertices[2].g) << ","
                      << static_cast<uint32_t>(batch.vertices[2].b) << ","
                      << static_cast<uint32_t>(batch.vertices[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((state.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(state.prim.type)
                      << " tme=" << static_cast<uint32_t>(state.prim.tme)
                      << " abe=" << static_cast<uint32_t>(state.prim.abe)
                      << " fst=" << static_cast<uint32_t>(state.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(state.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(state.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(state.texclut.cou)
                      << " cov=" << state.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    switch (state.prim.type)
    {
    case GS_PRIM_SPRITE:
        DrawSprite(batch);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        DrawTriangle(batch);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        DrawLine(batch);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = batch.vertices[0];
        const auto &ctx = state.context;
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        WritePixel(state, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a, v.fog);
        break;
    }
    default:
        break;
    }
}

void GSCpuBackend::WritePixel(const GSDrawState &state, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog)
{
    const auto &ctx = state.context;
    if (x < ctx.scissor.x0 || x > ctx.scissor.x1 || y < ctx.scissor.y0 || y > ctx.scissor.y1)
        return;

    if (state.prim.fge)
    {
        const uint32_t inverseFog = 255u - fog;
        auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
        {
            return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
        };

        r = applyFog(r, state.fogR);
        g = applyFog(g, state.fogG);
        b = applyFog(b, state.fogB);
    }

    const u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm;

    const PixelWriteMask writeMask = classifyAlphaTest(ctx.test, a, static_cast<uint8_t>(fpsm));
    if (!writeMask.writesAnything())
    {
        if (s_xmenActiveRasterProbe)
            ++s_xmenActiveRasterProbe->alphaRejected;
        return;
    }

    if (s_xmenActiveRasterProbe)
    {
        ++s_xmenActiveRasterProbe->covered;
        const uint32_t coveredX = static_cast<uint32_t>(x);
        const uint32_t coveredY = static_cast<uint32_t>(y);
        s_xmenActiveRasterProbe->minCoveredX = std::min(s_xmenActiveRasterProbe->minCoveredX, coveredX);
        s_xmenActiveRasterProbe->maxCoveredX = std::max(s_xmenActiveRasterProbe->maxCoveredX, coveredX);
        s_xmenActiveRasterProbe->minCoveredY = std::min(s_xmenActiveRasterProbe->minCoveredY, coveredY);
        s_xmenActiveRasterProbe->maxCoveredY = std::max(s_xmenActiveRasterProbe->maxCoveredY, coveredY);
        s_xmenActiveRasterProbe->minIncomingZ = std::min(s_xmenActiveRasterProbe->minIncomingZ, static_cast<uint32_t>(z));
        s_xmenActiveRasterProbe->maxIncomingZ = std::max(s_xmenActiveRasterProbe->maxIncomingZ, static_cast<uint32_t>(z));
    }

    const bool ztestEnabled = ((ctx.test >> 16) & 1u) != 0u;
    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool alphaBlendEnabled = state.prim.abe;
    const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
    const bool destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((ctx.frame.fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

    u32 rawFramebufferPixel = 0;
    u32 fbrgba = 0;
    if (frmw)
    {
        rawFramebufferPixel = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
        fbrgba = rawFramebufferPixel;

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
        else if (fpsm == GS_PSM_CT24)
        {
            // The GS supplies 0x80 as destination alpha for RGB24 blending.
            fbrgba |= 0x80000000u;
        }
    }

    if (!passesDestinationAlphaTest(ctx.test, static_cast<uint8_t>(fpsm), rawFramebufferPixel))
    {
        if (s_xmenActiveRasterProbe)
            ++s_xmenActiveRasterProbe->destinationAlphaRejected;
        return;
    }

    bool zpass = !ztestEnabled;
    uint32_t storedZ = 0u;
    if (ztestEnabled)
    {
        switch (ztestMethod)
        {
        case 0:
            zpass = false;
            break;
        case 1:
            zpass = true;
            break;
        case 2:
            storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
            zpass = static_cast<uint32_t>(z) >= storedZ;
            break;
        case 3:
            storedZ = ReadVramUnlocked(zpsm, zbp, fbw, x, y);
            zpass = static_cast<uint32_t>(z) > storedZ;
            break;
        }
        if (s_xmenActiveRasterProbe && ztestMethod >= 2u)
        {
            s_xmenActiveRasterProbe->minStoredZ = std::min(s_xmenActiveRasterProbe->minStoredZ, storedZ);
            s_xmenActiveRasterProbe->maxStoredZ = std::max(s_xmenActiveRasterProbe->maxStoredZ, storedZ);
        }
    }

    if (!zpass)
    {
        if (s_xmenActiveRasterProbe)
            ++s_xmenActiveRasterProbe->depthRejected;
        return;
    }

    if (writeMask.writesFramebuffer())
    {
        const u8 srcR = r;
        const u8 srcG = g;
        const u8 srcB = b;
        const u32 framebufferBefore = s_xmenActiveRasterProbe
            ? (frmw ? rawFramebufferPixel : ReadVramUnlocked(fpsm, fbp, fbw, x, y))
            : 0u;

        if (s_xmenActiveRasterProbe)
        {
            s_xmenActiveRasterProbe->sourceChannelSum +=
                static_cast<uint64_t>(srcR) + static_cast<uint64_t>(srcG) + static_cast<uint64_t>(srcB);
            s_xmenActiveRasterProbe->maxSourceChannel = std::max<uint32_t>(
                s_xmenActiveRasterProbe->maxSourceChannel,
                std::max<uint32_t>(srcR, std::max<uint32_t>(srcG, srcB)));
            if (isInXmenChromaTraceBounds(x, y) && isHighChroma(srcR, srcG, srcB))
                ++s_xmenActiveRasterProbe->topChromaSourceWrites;
        }

        if (state.prim.abe)
        {
            uint8_t dr = fbrgba & 0xFF;
            uint8_t dg = (fbrgba >> 8) & 0xFF;
            uint8_t db = (fbrgba >> 16) & 0xFF;
            uint8_t da = (fbrgba >> 24) & 0xFF;

            // PABE disables alpha blending when the source alpha MSB is clear.
            if (!(state.pabe && (a & 0x80u) == 0u))
            {
                uint64_t alphaReg = ctx.alpha;
                uint8_t asel = alphaReg & 3;
                uint8_t bsel = (alphaReg >> 2) & 3;
                uint8_t csel = (alphaReg >> 4) & 3;
                uint8_t dsel = (alphaReg >> 6) & 3;
                uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                {
                    if (sel == 0)
                        return cs;
                    if (sel == 1)
                        return cd;
                    return 0;
                };
                int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                           : fix;
                auto finalizeBlendChannel = [&](int value) -> uint8_t
                {
                    return (state.colclamp & 0x1u) != 0u
                        ? clampU8(value)
                        : static_cast<uint8_t>(value);
                };

                r = finalizeBlendChannel(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr));
                g = finalizeBlendChannel(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg));
                b = finalizeBlendChannel(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db));
            }
            else
            {
                r = srcR;
                g = srcG;
                b = srcB;
            }
        }

        if (writeMask.writeAlpha && (ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        u32 pixel = pack32(r, g, b, a);

        if (ctx.frame.fbmsk != 0)
        {
            pixel = (pixel & ~ctx.frame.fbmsk) | (fbrgba & ctx.frame.fbmsk);
        }

        if (preserveDestinationAlpha)
        {
            pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
        }

        // format conversion
        if (bitsPerPixel(fpsm) == 16)
        {
            pixel = Rgba8888ToRgba5551(pixel);
        }

        WriteVramUnlocked(fpsm, fbp, fbw, x, y, pixel);
        if (s_xmenActiveRasterProbe)
        {
            ++s_xmenActiveRasterProbe->framebufferWrites;
            if (srcR != 0u || srcG != 0u || srcB != 0u)
                ++s_xmenActiveRasterProbe->nonzeroRgbWrites;

            const u32 framebufferAfter = ReadVramUnlocked(fpsm, fbp, fbw, x, y);
            if (framebufferAfter != framebufferBefore)
                ++s_xmenActiveRasterProbe->framebufferChangedWrites;

            const u32 storedRgba = bitsPerPixel(fpsm) == 16
                ? Rgba5551ToRgba8888(static_cast<u16>(framebufferAfter))
                : framebufferAfter;
            if ((storedRgba & 0x00FFFFFFu) != 0u)
                ++s_xmenActiveRasterProbe->nonzeroFramebufferWrites;
            const uint8_t storedR = static_cast<uint8_t>(storedRgba);
            const uint8_t storedG = static_cast<uint8_t>(storedRgba >> 8u);
            const uint8_t storedB = static_cast<uint8_t>(storedRgba >> 16u);
            if (isInXmenChromaTraceBounds(x, y) && isHighChroma(storedR, storedG, storedB))
            {
                ++s_xmenActiveRasterProbe->topChromaFramebufferWrites;
                const uint32_t chromaX = static_cast<uint32_t>(x);
                const uint32_t chromaY = static_cast<uint32_t>(y);
                s_xmenActiveRasterProbe->minTopChromaX = std::min(s_xmenActiveRasterProbe->minTopChromaX, chromaX);
                s_xmenActiveRasterProbe->maxTopChromaX = std::max(s_xmenActiveRasterProbe->maxTopChromaX, chromaX);
                s_xmenActiveRasterProbe->minTopChromaY = std::min(s_xmenActiveRasterProbe->minTopChromaY, chromaY);
                s_xmenActiveRasterProbe->maxTopChromaY = std::max(s_xmenActiveRasterProbe->maxTopChromaY, chromaY);
            }
        }
    }

    if (writeMask.writeDepth && !ctx.zbuf.zmask)
    {
        WriteVramUnlocked(zpsm, zbp, fbw, x, y, z);
        if (s_xmenActiveRasterProbe)
            ++s_xmenActiveRasterProbe->depthWrites;
    }
}

uint32_t GSCpuBackend::LookupCLUT(const GSDrawState &state,
                                  uint8_t index,
                                  uint32_t cbp,
                                  uint8_t cpsm,
                                  uint8_t csm,
                                  uint8_t csa,
                                  uint8_t sourcePsm)
{
    const uint32_t clutIndex = resolveClutIndex(index, cpsm, csm, csa, sourcePsm);
    const bool csm2 = csm != 0u;
    const uint32_t clutWidth = csm2 && state.texclut.cbw != 0u
        ? static_cast<uint32_t>(state.texclut.cbw)
        : 1u;
    const uint32_t clutX = (csm2 ? static_cast<uint32_t>(state.texclut.cou) << 4u : 0u) +
                           (clutIndex & 0x0Fu);
    const uint32_t clutY = (csm2 ? static_cast<uint32_t>(state.texclut.cov) : 0u) +
                           (clutIndex >> 4);

    switch (cpsm)
    {
    case GS_PSM_CT32:
        return applyTexa(state.texa, cpsm, GSMem::ReadCT32(m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT24:
        return applyTexa(state.texa, cpsm, GSMem::ReadCT24(m_vram, cbp, clutWidth, clutX, clutY));
    case GS_PSM_CT16:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16(m_vram, cbp, clutWidth, clutX, clutY)));
    case GS_PSM_CT16S:
        return applyTexa(state.texa, cpsm, Rgba5551ToRgba8888(GSMem::ReadCT16S(m_vram, cbp, clutWidth, clutX, clutY)));
    default:
        break;
    }

    return 0xFFFF00FFu;
}

uint32_t GSCpuBackend::SampleTexture(const GSDrawState &state, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;

    const int texW = state.textureWidth;
    const int texH = state.textureHeight;
    const uint64_t clamp = ctx.clamp;
    const uint8_t wrapU = static_cast<uint8_t>(clamp & 0x3u);
    const uint8_t wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
    const uint16_t minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
    const uint16_t maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
    const uint16_t minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
    const uint16_t maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);

    float texUf, texVf;
    if (state.prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
        sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);

        u32 out = ReadVramUnlocked(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(state.texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(state.texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            return LookupCLUT(state, static_cast<u8>(out), tex.cbp, tex.cpsm, tex.csm, tex.csa, tex.psm);
        }

        return 0xFFFF00FFu;
    };

    if (!state.linearFilter)
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSCpuBackend::DrawSprite(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (state.prim.tme)
    {
        const auto &tex = ctx.tex0;
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;

        float u0f, v0f, u1f, v1f;
        if (state.prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (state.prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = SampleTexture(state, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = SampleTexture(state, texUf / static_cast<float>(texW), texVf / static_cast<float>(texH), 1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                WritePixel(state, x, y, z1, color.r, color.g, color.b, color.a, v1.fog);
            }
        }
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                WritePixel(state, x, y, z1, r, g, b, a, v1.fog);
    }
}

void GSCpuBackend::DrawTriangle(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const GSVertex &v2 = batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    for (int y = minY; y <= maxY; ++y)
    {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (state.prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (state.prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (state.prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    // The GS DDA interpolates the homogeneous S, T and Q
                    // values. Texel coordinates are calculated from S/Q and
                    // T/Q only after interpolation.
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = SampleTexture(state, is, it, iq, iu, iv);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);
                if (s_xmenActiveRasterProbe)
                {
                    ++s_xmenActiveRasterProbe->textureSamples;
                    if ((texel & 0x00FFFFFFu) != 0u)
                        ++s_xmenActiveRasterProbe->nonzeroTextureRgbSamples;
                }

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);
                if (s_xmenActiveRasterProbe && (color.r != 0u || color.g != 0u || color.b != 0u))
                    ++s_xmenActiveRasterProbe->nonzeroCombinedRgbSamples;

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            const uint8_t fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
            WritePixel(state, x, y, static_cast<u32>(z + 0.5), r, g, b, a, fog);
        }
    }
}

void GSCpuBackend::DrawLine(const GSPrimitiveBatch &batch)
{
    const GSDrawState &state = batch.state;
    const GSVertex &v0 = batch.vertices[0];
    const GSVertex &v1 = batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (state.prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);
        const uint8_t fog = clampU8(static_cast<int>(v0.fog + (v1.fog - v0.fog) * t));
        WritePixel(state, x0, y0, static_cast<u32>(z), r, g, b, a, fog);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}

void GSCpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transfer = command;
    m_transferState.x = command.trxpos.dsax;
    m_transferState.y = command.trxpos.dsay;
    m_transferState.totalPixels = static_cast<uint32_t>(command.trxreg.rrw) * static_cast<uint32_t>(command.trxreg.rrh);
    m_transferState.copiedPixels = 0u;
    m_transferState.direction = command.direction;
    m_transferState.localToHostPendingBytes = 0u;

    if (traceXmenVramRegion() &&
        (isXmenVramRegionBlock(command.bitbltbuf.sbp) ||
         isXmenVramRegionBlock(command.bitbltbuf.dbp)))
    {
        std::fprintf(stdout,
                     "[xmen-vram-transfer] phase=begin present=%u tick=%llu dir=%u "
                     "src=%u/%u/0x%x dst=%u/%u/0x%x pos=%u,%u-%u,%u size=%u,%u\n",
                     s_xmenLastDrawPresent.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(s_xmenLastDrawTick.load(std::memory_order_relaxed)),
                     static_cast<unsigned>(command.direction), command.bitbltbuf.sbp,
                     static_cast<unsigned>(command.bitbltbuf.sbw),
                     static_cast<unsigned>(command.bitbltbuf.spsm), command.bitbltbuf.dbp,
                     static_cast<unsigned>(command.bitbltbuf.dbw),
                     static_cast<unsigned>(command.bitbltbuf.dpsm), command.trxpos.ssax,
                     command.trxpos.ssay, command.trxpos.dsax, command.trxpos.dsay,
                     command.trxreg.rrw, command.trxreg.rrh);
        std::fflush(stdout);
    }

    if (command.bitbltbuf.dbp == 11200u || command.bitbltbuf.dbp == 12224u ||
        command.bitbltbuf.dbp == 12256u || command.bitbltbuf.dbp == 12512u ||
        command.bitbltbuf.dbp == 12544u)
    {
        std::fprintf(stderr,
                     "[xmen-gs-legal-upload] begin dbp=%u dbw=%u dpsm=%u ds=%u,%u size=%u,%u dir=%u\n",
                     command.bitbltbuf.dbp, static_cast<unsigned>(command.bitbltbuf.dbw),
                     static_cast<unsigned>(command.bitbltbuf.dpsm), command.trxpos.dsax,
                     command.trxpos.dsay, command.trxreg.rrw, command.trxreg.rrh, command.direction);
    }

    if (command.direction == 2u)
        PerformLocalToLocalTransfer();
    else if (command.direction == 1u)
        PerformLocalToHostTransfer();
}

void GSCpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!data || sizeBytes == 0u || !m_vram || m_transferState.direction != 0u)
        return;
    if (m_transfer.trxreg.rrw == 0u || m_transfer.trxreg.rrh == 0u || m_transferState.totalPixels == 0u)
        return;

    if (traceXmenVramRegion() && isXmenVramRegionBlock(m_transfer.bitbltbuf.dbp))
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0u; i < sizeBytes; ++i)
        {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        std::fprintf(stdout,
                     "[xmen-vram-transfer] phase=data present=%u tick=%llu dst=%u/%u/0x%x "
                     "copied=%u/%u bytes=%u hash=%016llx head=",
                     s_xmenLastDrawPresent.load(std::memory_order_relaxed),
                     static_cast<unsigned long long>(s_xmenLastDrawTick.load(std::memory_order_relaxed)),
                     m_transfer.bitbltbuf.dbp, static_cast<unsigned>(m_transfer.bitbltbuf.dbw),
                     static_cast<unsigned>(m_transfer.bitbltbuf.dpsm), m_transferState.copiedPixels,
                     m_transferState.totalPixels, sizeBytes, static_cast<unsigned long long>(hash));
        for (uint32_t i = 0u; i < std::min<uint32_t>(sizeBytes, 16u); ++i)
            std::fprintf(stdout, "%02x", static_cast<unsigned>(data[i]));
        std::fprintf(stdout, "\n");
        std::fflush(stdout);
    }

    if ((m_transfer.bitbltbuf.dbp == 11200u || m_transfer.bitbltbuf.dbp == 12224u ||
         m_transfer.bitbltbuf.dbp == 12256u || m_transfer.bitbltbuf.dbp == 12512u ||
         m_transfer.bitbltbuf.dbp == 12544u) &&
        m_transferState.copiedPixels == 0u)
    {
        std::fprintf(stderr, "[xmen-gs-legal-upload] data dbp=%u bytes=%u head=",
                     m_transfer.bitbltbuf.dbp, sizeBytes);
        const uint32_t dumpBytes = std::min<uint32_t>(sizeBytes, 32u);
        for (uint32_t i = 0u; i < dumpBytes; ++i)
            std::fprintf(stderr, "%02x", static_cast<unsigned>(data[i]));
        std::fprintf(stderr, "\n");
    }

    const uint32_t dbp = m_transfer.bitbltbuf.dbp;
    const uint32_t dbw = std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u);
    const uint8_t dpsm = m_transfer.bitbltbuf.dpsm;
    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t dsax = m_transfer.trxpos.dsax;
    uint32_t offset = 0u;

    auto advancePixel = [&](uint32_t count)
    {
        const uint32_t totalPixels = m_transferState.totalPixels;
        m_transferState.copiedPixels =
            std::min<uint32_t>(totalPixels, m_transferState.copiedPixels + count);

        if (m_transferState.copiedPixels >= totalPixels)
        {
            m_transferState.direction = 3u;
            m_transferState.totalPixels = 0u;
            return;
        }

        m_transferState.x = dsax + (m_transferState.copiedPixels % rrw);
        m_transferState.y = m_transfer.trxpos.dsay + (m_transferState.copiedPixels / rrw);
    };

    while (offset < sizeBytes && m_transferState.direction == 0u)
    {
        switch (dpsm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            if (sizeBytes - offset < 4u)
                return;
            uint32_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 4u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            if (sizeBytes - offset < 3u)
                return;
            const uint32_t value = static_cast<uint32_t>(data[offset]) |
                                   (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
                                   (static_cast<uint32_t>(data[offset + 2u]) << 16u);
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 3u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
        {
            if (sizeBytes - offset < 2u)
                return;
            uint16_t value = 0u;
            std::memcpy(&value, data + offset, sizeof(value));
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, value);
            offset += 2u;
            advancePixel(1u);
            break;
        }
        case GS_PSM_T8:
        case GS_PSM_T8H:
            WriteVramUnlocked(dpsm, dbp, dbw, m_transferState.x, m_transferState.y, data[offset++]);
            advancePixel(1u);
            break;
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        {
            const uint8_t packed = data[offset++];
            const uint32_t firstPixel = m_transferState.copiedPixels;
            WriteVramUnlocked(dpsm, dbp, dbw,
                              dsax + (firstPixel % rrw),
                              m_transfer.trxpos.dsay + (firstPixel / rrw),
                              packed & 0x0Fu);
            if (firstPixel + 1u < m_transferState.totalPixels)
            {
                const uint32_t secondPixel = firstPixel + 1u;
                WriteVramUnlocked(dpsm, dbp, dbw,
                                  dsax + (secondPixel % rrw),
                                  m_transfer.trxpos.dsay + (secondPixel / rrw),
                                  (packed >> 4u) & 0x0Fu);
            }
            advancePixel(std::min<uint32_t>(2u, m_transferState.totalPixels - firstPixel));
            break;
        }
        default:
            return;
        }
    }
}

void GSCpuBackend::PerformLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t total = rrw * rrh;
    if (total == 0u)
    {
        m_transferState.direction = 3u;
        return;
    }

    for (uint32_t pixel = 0; pixel < total; ++pixel)
    {
        uint32_t x = pixel % rrw;
        uint32_t y = pixel / rrw;
        if ((m_transfer.trxpos.dir & 0x2u) != 0u)
            x = rrw - x - 1u;
        if ((m_transfer.trxpos.dir & 0x1u) != 0u)
            y = rrh - y - 1u;

        const uint32_t value = ReadVramUnlocked(m_transfer.bitbltbuf.spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u),
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        WriteVramUnlocked(m_transfer.bitbltbuf.dpsm,
                          m_transfer.bitbltbuf.dbp,
                          std::max<uint32_t>(m_transfer.bitbltbuf.dbw, 1u),
                          x + m_transfer.trxpos.dsax,
                          y + m_transfer.trxpos.dsay,
                          value);
    }

    m_transferState.copiedPixels = total;
    m_transferState.direction = 3u;
}

void GSCpuBackend::PerformLocalToHostTransfer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0u;
    if (!m_vram)
        return;

    const uint32_t rrw = m_transfer.trxreg.rrw;
    const uint32_t rrh = m_transfer.trxreg.rrh;
    const uint32_t sbw = std::max<uint32_t>(m_transfer.bitbltbuf.sbw, 1u);
    const uint8_t spsm = m_transfer.bitbltbuf.spsm;
    const uint32_t bpp = static_cast<uint32_t>(GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm)));
    const uint32_t total = rrw * rrh;
    m_localToHostBuffer.reserve((static_cast<size_t>(total) * bpp + 7u) / 8u);

    for (uint32_t pixel = 0u; pixel < total; ++pixel)
    {
        const uint32_t x = pixel % rrw;
        const uint32_t y = pixel / rrw;
        const uint32_t value = ReadVramUnlocked(spsm,
                                                m_transfer.bitbltbuf.sbp,
                                                sbw,
                                                x + m_transfer.trxpos.ssax,
                                                y + m_transfer.trxpos.ssay);
        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 24u));
            break;
        case 24:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 16u));
            break;
        case 16:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value >> 8u));
            break;
        case 8:
            m_localToHostBuffer.push_back(static_cast<uint8_t>(value));
            break;
        case 4:
        {
            if ((pixel & 1u) != 0u)
                break;
            uint32_t next = 0u;
            if (pixel + 1u < total)
            {
                const uint32_t nextPixel = pixel + 1u;
                const uint32_t nextX = nextPixel % rrw;
                const uint32_t nextY = nextPixel / rrw;
                next = ReadVramUnlocked(spsm, m_transfer.bitbltbuf.sbp, sbw,
                                        nextX + m_transfer.trxpos.ssax,
                                        nextY + m_transfer.trxpos.ssay);
            }
            m_localToHostBuffer.push_back(static_cast<uint8_t>((value & 0x0Fu) | ((next & 0x0Fu) << 4u)));
            break;
        }
        default:
            break;
        }
    }

    m_transferState.copiedPixels = total;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size();
}

uint32_t GSCpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!dst || maxBytes == 0u || m_localToHostReadPos >= m_localToHostBuffer.size())
        return 0u;
    const size_t count = std::min<size_t>(maxBytes, m_localToHostBuffer.size() - m_localToHostReadPos);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, count);
    m_localToHostReadPos += count;
    m_transferState.localToHostPendingBytes = m_localToHostBuffer.size() - m_localToHostReadPos;
    return static_cast<uint32_t>(count);
}

bool GSCpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_vram || context.frame.fbw == 0u)
        return false;

    const uint32_t x0 = context.scissor.x0;
    const uint32_t x1 = std::max<uint32_t>(x0, context.scissor.x1);
    const uint32_t y0 = context.scissor.y0;
    const uint32_t y1 = std::max<uint32_t>(y0, context.scissor.y1);
    uint8_t r = static_cast<uint8_t>(rgba);
    uint8_t g = static_cast<uint8_t>(rgba >> 8u);
    uint8_t b = static_cast<uint8_t>(rgba >> 16u);
    uint8_t a = static_cast<uint8_t>(rgba >> 24u);
    if ((context.fba & 1ull) != 0ull && context.frame.psm != GS_PSM_CT24)
        a |= 0x80u;

    const uint32_t fbp = GSInternal::framePageBaseToBlock(context.frame.fbp);
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    if (context.frame.psm == GS_PSM_CT32 || context.frame.psm == GS_PSM_CT24)
    {
        const uint32_t source = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8u) |
                                (static_cast<uint32_t>(b) << 16u) |
                                (static_cast<uint32_t>(a) << 24u);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint32_t pixel = source;
                if (context.frame.fbmsk != 0u)
                {
                    const uint32_t old = ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y);
                    pixel = (pixel & ~context.frame.fbmsk) | (old & context.frame.fbmsk);
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }

    if (context.frame.psm == GS_PSM_CT16 || context.frame.psm == GS_PSM_CT16S)
    {
        const uint16_t source = encodeFramePixelPSMCT16(r, g, b, a);
        const uint16_t mask = static_cast<uint16_t>(context.frame.fbmsk);
        for (uint32_t y = y0; y <= y1; ++y)
            for (uint32_t x = x0; x <= x1; ++x)
            {
                uint16_t pixel = source;
                if (mask != 0u)
                {
                    const uint16_t old = static_cast<uint16_t>(ReadVramUnlocked(context.frame.psm, fbp, fbw, x, y));
                    pixel = static_cast<uint16_t>((pixel & ~mask) | (old & mask));
                }
                WriteVramUnlocked(context.frame.psm, fbp, fbw, x, y, pixel);
            }
        return true;
    }
    return false;
}

bool GSCpuBackend::CopyFrameToHostRgba(const GSFrameReg &frame,
                                       uint32_t width,
                                       uint32_t height,
                                       std::vector<uint8_t> &outPixels,
                                       bool preserveAlpha,
                                       bool useLocalMemoryLayout,
                                       bool frameBaseIsPages,
                                       uint32_t sourceOriginX,
                                       uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
        return false;

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
    const uint32_t baseBytes = frameBaseIsPages ? frame.fbp * 8192u : frame.fbp * 256u;
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbw = frame.fbw ? frame.fbw : kHostFrameWidth / 64u;
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t stride = fbw * 64u * bytesPerPixel;

    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *dst = outPixels.data() + y * kHostFrameWidth * 4u;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t sx = sourceOriginX + x;
            const uint32_t sy = sourceOriginY + y;
            if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
            {
                uint32_t color = 0u;
                if (useLocalMemoryLayout)
                    color = ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy);
                else
                {
                    const uint32_t pixelBytes = frame.psm == GS_PSM_CT24 ? 3u : 4u;
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * pixelBytes;
                    if (offset + pixelBytes > m_vramSize)
                        return false;
                    color = m_vram[offset] | (static_cast<uint32_t>(m_vram[offset + 1u]) << 8u) |
                            (static_cast<uint32_t>(m_vram[offset + 2u]) << 16u);
                    if (pixelBytes == 4u)
                        color |= static_cast<uint32_t>(m_vram[offset + 3u]) << 24u;
                }
                dst[x * 4u] = static_cast<uint8_t>(color);
                dst[x * 4u + 1u] = static_cast<uint8_t>(color >> 8u);
                dst[x * 4u + 2u] = static_cast<uint8_t>(color >> 16u);
                dst[x * 4u + 3u] = preserveAlpha && frame.psm != GS_PSM_CT24 ? static_cast<uint8_t>(color >> 24u) : 255u;
            }
            else if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
            {
                uint16_t color = 0u;
                if (useLocalMemoryLayout)
                    color = static_cast<uint16_t>(ReadVramUnlocked(frame.psm, basePtr, fbw, sx, sy));
                else
                {
                    const uint64_t offset = static_cast<uint64_t>(baseBytes) + static_cast<uint64_t>(sy) * stride + static_cast<uint64_t>(sx) * 2u;
                    if (offset + 2u > m_vramSize)
                        return false;
                    std::memcpy(&color, m_vram + offset, sizeof(color));
                }
                const uint32_t r = color & 31u;
                const uint32_t g = (color >> 5u) & 31u;
                const uint32_t b = (color >> 10u) & 31u;
                dst[x * 4u] = static_cast<uint8_t>((r << 3u) | (r >> 2u));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3u) | (g >> 2u));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3u) | (b >> 2u));
                dst[x * 4u + 3u] = preserveAlpha ? ((color & 0x8000u) ? 0x80u : 0u) : 255u;
            }
            else
            {
                outPixels.clear();
                return false;
            }
        }
    }
    return true;
}

PresentationFrame GSCpuBackend::Present(const GSPresentationRequest &request)
{
    // Snapshot local memory under the backend lock, then perform the expensive
    // display conversion without holding the producer-side raster lock.
    thread_local std::vector<uint8_t> snapshot;
    SnapshotVram(snapshot);
    if (snapshot.empty())
        return {};

    thread_local GSCpuBackend snapshotBackend;
    snapshotBackend.Initialize(snapshot.data(), static_cast<uint32_t>(snapshot.size()));
    return snapshotBackend.PresentFromLocalMemory(request);
}

PresentationFrame GSCpuBackend::PresentFromLocalMemory(const GSPresentationRequest &request)
{
    PresentationFrame result{};
    const GSPmodeState pmode = decodePmode(request.pmode);
    const GSFrameReg displayFrame1 = decodeDisplayFrame(request.dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(request.dispfb2);
    const GSDisplayReadOrigin origin1 = decodeDisplayReadOrigin(request.dispfb1);
    const GSDisplayReadOrigin origin2 = decodeDisplayReadOrigin(request.dispfb2);
    uint32_t width1 = 0u, height1 = 0u, width2 = 0u, height2 = 0u;
    decodeDisplaySize(request.display1, width1, height1);
    decodeDisplaySize(request.display2, width2, height2);
    const bool valid1 = pmode.enableCrt1 && hasDisplaySetup(request.display1, displayFrame1);
    const bool valid2 = pmode.enableCrt2 && hasDisplaySetup(request.display2, displayFrame2);
    if (!valid1 && !valid2)
        return result;

    auto copySource = [&](const GSFrameReg &displayFrame,
                          const GSDisplayReadOrigin &origin,
                          uint32_t width,
                          uint32_t height,
                          bool allowPreferred,
                          bool preserveAlpha,
                          GSFrameReg &selected,
                          std::vector<uint8_t> &pixels,
                          bool &usedPreferred) -> bool
    {
        selected = displayFrame;
        pixels.clear();
        usedPreferred = false;
        const uint32_t sourceProbeIndex = s_presentSourceProbeCount.fetch_add(1u, std::memory_order_relaxed);
        const bool traceSourceProbe = sourceProbeIndex < 96u;
        const auto logSourceProbe = [&](const char *phase, const GSFrameReg &frame, const std::vector<uint8_t> &probePixels)
        {
            if (!traceSourceProbe)
                return;
            const uint32_t nonblack = probePixels.empty() ? 0u : countNonBlackPixels(probePixels, width, height);
            std::fprintf(stderr,
                         "[gs:present-source] idx=%u phase=%s size=%ux%u frame=%u/%u/%u/%08x "
                         "origin=%u,%u nonblack=%u empty=%u allowPref=%u prefHas=%u prefDest=%u\n",
                         sourceProbeIndex,
                         phase,
                         width,
                         height,
                         frame.fbp,
                         frame.fbw,
                         frame.psm,
                         frame.fbmsk,
                         origin.x,
                         origin.y,
                         nonblack,
                         probePixels.empty() ? 1u : 0u,
                         allowPreferred ? 1u : 0u,
                         request.hasPreferredSource ? 1u : 0u,
                         request.preferredDestFbp);
        };
        if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == displayFrame.fbp &&
            (request.preferredSource.fbw != 0u || request.preferredSource.fbp != displayFrame.fbp) &&
            CopyFrameToHostRgba(request.preferredSource, width, height, pixels, preserveAlpha, true, false, 0u, 0u))
        {
            selected = request.preferredSource;
            usedPreferred = true;
            logSourceProbe("preferred", selected, pixels);
        }
        if (pixels.empty())
        {
            (void)CopyFrameToHostRgba(displayFrame, width, height, pixels, preserveAlpha, true, true, origin.x, origin.y);
            logSourceProbe("display", displayFrame, pixels);
        }

        if (!usedPreferred && (pixels.empty() || countNonBlackPixels(pixels, width, height) == 0u))
        {
            for (const GSFrameReg &candidate : request.contextFrames)
            {
                if (candidate.fbp == selected.fbp && candidate.fbw == selected.fbw && candidate.psm == selected.psm)
                    continue;
                std::vector<uint8_t> candidatePixels;
                if (!CopyFrameToHostRgba(candidate, width, height, candidatePixels, preserveAlpha, true, true, 0u, 0u))
                    continue;
                logSourceProbe("context", candidate, candidatePixels);
                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                    continue;
                selected = candidate;
                pixels.swap(candidatePixels);
                logSourceProbe("selected-context", selected, pixels);
                break;
            }
        }
        if (pixels.empty())
            return false;
        return true;
    };

    if (valid1 && valid2)
    {
        GSFrameReg selected1{}, selected2{};
        std::vector<uint8_t> crt1, crt2;
        bool preferred1 = false, preferred2 = false;
        if (copySource(displayFrame1, origin1, width1, height1, false, true, selected1, crt1, preferred1) &&
            copySource(displayFrame2, origin2, width2, height2, false, true, selected2, crt2, preferred2))
        {
            result.width = std::max(width1, width2);
            result.height = std::max(height1, height2);
            result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            const uint8_t bgR = static_cast<uint8_t>(request.bgcolor);
            const uint8_t bgG = static_cast<uint8_t>(request.bgcolor >> 8u);
            const uint8_t bgB = static_cast<uint8_t>(request.bgcolor >> 16u);
            for (uint32_t y = 0; y < result.height; ++y)
                for (uint32_t x = 0; x < result.width; ++x)
                {
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    dst[0] = bgR;
                    dst[1] = bgG;
                    dst[2] = bgB;
                    dst[3] = pmode.alp;
                }
            if (!pmode.slbg)
                for (uint32_t y = 0; y < height2; ++y)
                    std::memcpy(result.pixels.data() + y * kHostFrameWidth * 4u, crt2.data() + y * kHostFrameWidth * 4u, width2 * 4u);
            for (uint32_t y = 0; y < height1; ++y)
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t *src = crt1.data() + (y * kHostFrameWidth + x) * 4u;
                    uint8_t *dst = result.pixels.data() + (y * kHostFrameWidth + x) * 4u;
                    const uint32_t factor = pmode.mmod ? pmode.alp : std::min<uint32_t>(255u, static_cast<uint32_t>(src[3]) * 2u);
                    dst[0] = blendPresentationChannel(src[0], dst[0], factor);
                    dst[1] = blendPresentationChannel(src[1], dst[1], factor);
                    dst[2] = blendPresentationChannel(src[2], dst[2], factor);
                    dst[3] = pmode.amod ? dst[3] : src[3];
                }
            normalizePresentationAlpha(result.pixels, result.width, result.height);
            result.displayFbp = displayFrame1.fbp;
            result.sourceFbp = selected1.fbp;
            logBlackPresentationDiagnostics("dual", request, result, m_vram, m_vramSize);
            return result;
        }
    }

    const GSFrameReg &displayFrame = valid1 ? displayFrame1 : displayFrame2;
    const GSDisplayReadOrigin &origin = valid1 ? origin1 : origin2;
    result.width = valid1 ? width1 : width2;
    result.height = valid1 ? height1 : height2;
    GSFrameReg selected = displayFrame;
    if (!copySource(displayFrame, origin, result.width, result.height, true, false, selected, result.pixels, result.usedPreferred))
        return {};
    normalizePresentationAlpha(result.pixels, result.width, result.height);
    result.displayFbp = displayFrame.fbp;
    result.sourceFbp = selected.fbp;
    logBlackPresentationDiagnostics("single", request, result, m_vram, m_vramSize);
    return result;
}
