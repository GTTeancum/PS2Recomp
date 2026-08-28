#include "MiniTest.h"
#include "runtime/ps2_audio_vag.h"

#include <array>
#include <cstdint>
#include <vector>

void register_ps2_audio_vag_tests()
{
    MiniTest::Case("PS2AudioVag", [](TestCase &tc)
    {
        tc.Run("decodes headerless PS2 ADPCM blocks", [](TestCase &t)
        {
            std::array<uint8_t, 32> blocks{};
            blocks[0] = 0x00u;
            blocks[1] = 0x00u;
            blocks[16] = 0x00u;
            blocks[17] = 0x01u;
            for (size_t i = 2u; i < 16u; ++i)
            {
                blocks[i] = 0xF1u;
                blocks[16u + i] = 0xF1u;
            }

            std::vector<int16_t> pcm;
            t.IsTrue(ps2_vag::decodeBlocks(blocks.data(), static_cast<uint32_t>(blocks.size()), pcm),
                     "headerless blocks should decode");
            t.Equals(pcm.size(), static_cast<size_t>(56u),
                     "each PS2 ADPCM block should produce 28 samples");
            t.Equals(pcm[0], static_cast<int16_t>(4096),
                     "low nibbles should decode first");
            t.Equals(pcm[1], static_cast<int16_t>(-4096),
                     "signed high nibbles should decode second");
        });

        tc.Run("deinterleaves PS2 ADPCM channel blocks", [](TestCase &t)
        {
            std::array<uint8_t, 64> blocks{};
            for (size_t i = 2u; i < 16u; ++i)
            {
                blocks[i] = 0x11u;
                blocks[16u + i] = 0xFFu;
                blocks[32u + i] = 0x22u;
                blocks[48u + i] = 0xEEu;
            }

            std::vector<int16_t> pcm;
            t.IsTrue(ps2_vag::decodeInterleavedBlocks(
                         blocks.data(), static_cast<uint32_t>(blocks.size()), 2u, 16u, pcm),
                     "stereo blocks should decode");
            t.Equals(pcm.size(), static_cast<size_t>(112u),
                     "two stereo blocks should produce 56 frames");
            t.Equals(pcm[0], static_cast<int16_t>(4096),
                     "left channel should come from the first interleave");
            t.Equals(pcm[1], static_cast<int16_t>(-4096),
                     "right channel should come from the second interleave");
            t.Equals(pcm[56], static_cast<int16_t>(8192),
                     "the next left block should retain channel ordering");
            t.Equals(pcm[57], static_cast<int16_t>(-8192),
                     "the next right block should retain channel ordering");
        });
    });
}
