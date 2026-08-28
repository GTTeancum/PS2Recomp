#ifndef PS2_AUDIO_VAG_H
#define PS2_AUDIO_VAG_H

#include <cstdint>
#include <vector>

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
    bool decodeBlocks(const uint8_t *data, uint32_t sizeBytes,
                      std::vector<int16_t> &outPcm);
    bool decodeInterleavedBlocks(const uint8_t *data, uint32_t sizeBytes,
                                 uint32_t channels, uint32_t interleaveBytes,
                                 std::vector<int16_t> &outPcm);
}

#endif
