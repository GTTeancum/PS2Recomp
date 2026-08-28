#include "runtime/ps2_audio.h"
#include "runtime/ps2_audio_vag.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    uint16_t readLe16(const uint8_t *data)
    {
        return static_cast<uint16_t>(data[0]) |
               static_cast<uint16_t>(data[1] << 8u);
    }

    uint32_t readLe32(const uint8_t *data)
    {
        return static_cast<uint32_t>(data[0]) |
               (static_cast<uint32_t>(data[1]) << 8u) |
               (static_cast<uint32_t>(data[2]) << 16u) |
               (static_cast<uint32_t>(data[3]) << 24u);
    }

    std::filesystem::path resolveZaudioPath(std::string guestPath)
    {
        const size_t versionSuffix = guestPath.find(';');
        if (versionSuffix != std::string::npos)
            guestPath.resize(versionSuffix);
        std::replace(guestPath.begin(), guestPath.end(), '\\', '/');

        const size_t deviceSeparator = guestPath.find(':');
        if (deviceSeparator != std::string::npos)
            guestPath.erase(0u, deviceSeparator + 1u);
        while (!guestPath.empty() && guestPath.front() == '/')
            guestPath.erase(guestPath.begin());
        if (guestPath.empty())
            return {};

        std::array<std::filesystem::path, 3> roots{
            PS2Runtime::getIoPaths().cdRoot,
            PS2Runtime::getIoPaths().elfDirectory,
            std::filesystem::current_path()};
        std::error_code ec;
        for (const std::filesystem::path &root : roots)
        {
            if (root.empty())
                continue;
            const std::filesystem::path candidate = (root / guestPath).lexically_normal();
            if (std::filesystem::is_regular_file(candidate, ec) && !ec)
                return candidate;
            ec.clear();
        }
        return {};
    }

    bool loadZsndPs2Stream(const std::string &guestPath, uint32_t streamOffset,
                           uint32_t streamSize, uint32_t channels,
                           std::vector<int16_t> &outPcm, uint32_t &outSampleRate,
                           std::filesystem::path &outHostPath)
    {
        outHostPath = resolveZaudioPath(guestPath);
        if (outHostPath.empty() || channels == 0u)
            return false;

        std::ifstream file(outHostPath, std::ios::binary | std::ios::ate);
        if (!file)
            return false;
        const std::streamoff fileSize = file.tellg();
        if (fileSize <= 0 || streamOffset >= static_cast<uint64_t>(fileSize))
            return false;

        const uint32_t available = static_cast<uint32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(fileSize) - streamOffset,
                               std::numeric_limits<uint32_t>::max()));
        const uint32_t encodedSize = std::min(streamSize, available) & ~15u;
        if (encodedSize < channels * 16u)
            return false;

        const size_t headerBytes = static_cast<size_t>(
            std::min<std::streamoff>(fileSize, 0x800));
        std::vector<uint8_t> header(headerBytes);
        file.seekg(0, std::ios::beg);
        file.read(reinterpret_cast<char *>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        outSampleRate = 44100u;
        if (file && header.size() >= 0x28u &&
            std::memcmp(header.data(), "ZSND", 4u) == 0 &&
            std::memcmp(header.data() + 4u, "PS2 ", 4u) == 0)
        {
            const uint32_t streamHeaderCount = readLe32(header.data() + 0x1cu);
            const uint32_t streamHeadersOffset = readLe32(header.data() + 0x24u);
            if (streamHeaderCount != 0u && streamHeadersOffset + 6u <= header.size())
            {
                const uint32_t pitch = readLe16(header.data() + streamHeadersOffset + 2u);
                if (pitch != 0u)
                    outSampleRate = (pitch * 44100u + 2048u) / 4096u;
            }
        }

        file.clear();
        file.seekg(streamOffset, std::ios::beg);
        std::vector<uint8_t> encoded(encodedSize);
        file.read(reinterpret_cast<char *>(encoded.data()),
                  static_cast<std::streamsize>(encoded.size()));
        if (file.gcount() != static_cast<std::streamsize>(encoded.size()))
            return false;

        constexpr uint32_t kZsndPs2InterleaveBytes = 0x800u;
        return ps2_vag::decodeInterleavedBlocks(encoded.data(), encodedSize, channels,
                                                kZsndPs2InterleaveBytes, outPcm);
    }

    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount,
                                         uint32_t sampleRate, uint16_t channels = 1u)
    {
        channels = std::max<uint16_t>(channels, 1u);
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = static_cast<uint8_t>(channels);
        p[23] = static_cast<uint8_t>(channels >> 8u);
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * channels * 2u;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        const uint16_t blockAlign = channels * 2u;
        p[32] = static_cast<uint8_t>(blockAlign);
        p[33] = static_cast<uint8_t>(blockAlign >> 8u);
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

struct PS2AudioBackend::Impl
{
    static constexpr uint32_t kZaudioVoiceCount = 32u;

    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
        uint32_t voiceId = 0xFFFFFFFFu;
    };
    struct ZaudioBank
    {
        uint32_t spuAddress = 0;
        uint32_t uploadedBytes = 0;
        std::vector<uint8_t> data;
    };
    struct ZaudioVoice
    {
        uint32_t sampleAddress = 0u;
        uint16_t pitch = 0x1000u;
        uint16_t volumeLeft = 0u;
        uint16_t volumeRight = 0u;
    };
    struct ZaudioStream
    {
        uint32_t channels = 1u;
        uint32_t sampleRate = 44100u;
        uint16_t pitch = 0x1000u;
        std::string guestPath;
        std::filesystem::path hostPath;
        std::vector<int16_t> pcm;
    };
#if !defined(PLATFORM_VITA)
    struct TrackedMusic
    {
        Music music;
        std::vector<uint8_t> sourceWav;
        uint32_t streamHandle = 0u;
    };
#endif
    std::vector<TrackedSound> activeSounds;
    std::unordered_map<uint32_t, ZaudioBank> zaudioBanks;
    std::unordered_map<uint32_t, ZaudioStream> zaudioStreams;
    std::array<ZaudioVoice, kZaudioVoiceCount> zaudioVoices{};
#if !defined(PLATFORM_VITA)
    std::vector<TrackedMusic> activeMusic;
#endif
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid == 0x47u)
    {
        static const bool traceZaudio = std::getenv("PS2X_TRACE_ZAUDIO") != nullptr;
        static uint32_t traceCount = 0u;
        static uint32_t traceUploadCount = 0u;
        const bool traceThisCommand = rpcNum != 4u || traceUploadCount++ < 2u;
        if (traceZaudio && traceThisCommand && traceCount++ < 64u)
        {
            std::cerr << "[ZAUDIO command] function=" << rpcNum
                      << " send=" << sendSize << " receive=" << recvSize << " bytes=";
            for (uint32_t i = 0u; i < std::min(sendSize, 64u); ++i)
            {
                static constexpr char kHex[] = "0123456789abcdef";
                const uint8_t value = sendBuf ? sendBuf[i] : 0u;
                std::cerr << kHex[value >> 4u] << kHex[value & 0x0Fu];
            }
            std::cerr << '\n';
        }

        constexpr uint32_t kAllocateSample = 2u;
        constexpr uint32_t kUploadSample = 4u;
        constexpr uint32_t kConfigureVoice = 5u;
        constexpr uint32_t kKeyOnVoices = 6u;
        constexpr uint32_t kKeyOffVoices = 7u;
        constexpr uint32_t kAllocateStream = 8u;
        constexpr uint32_t kFreeStream = 9u;
        constexpr uint32_t kConfigureStream = 10u;
        constexpr uint32_t kStartStream = 11u;
        if (rpcNum == kAllocateSample && sendBuf && sendSize >= sizeof(uint32_t) &&
            recvBuf && recvSize >= sizeof(uint32_t) * 2u)
        {
            uint32_t size = 0u;
            uint32_t handle = 0u;
            uint32_t spuAddress = 0u;
            std::memcpy(&size, sendBuf, sizeof(size));
            std::memcpy(&handle, recvBuf, sizeof(handle));
            std::memcpy(&spuAddress, recvBuf + sizeof(uint32_t), sizeof(spuAddress));
            if (size != 0u && handle != 0u)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                Impl::ZaudioBank bank{};
                bank.spuAddress = spuAddress;
                bank.data.resize(size);
                m_impl->zaudioBanks[handle] = std::move(bank);
            }
        }
        else if (rpcNum == kUploadSample && sendBuf && sendSize >= 0x408u)
        {
            uint32_t handle = 0u;
            uint32_t offset = 0u;
            std::memcpy(&handle, sendBuf, sizeof(handle));
            std::memcpy(&offset, sendBuf + sizeof(uint32_t), sizeof(offset));
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto bank = m_impl->zaudioBanks.find(handle);
            if (bank != m_impl->zaudioBanks.end() && offset < bank->second.data.size())
            {
                const uint32_t bytes = std::min<uint32_t>(0x400u,
                                                          static_cast<uint32_t>(bank->second.data.size()) - offset);
                std::memcpy(bank->second.data.data() + offset, sendBuf + 8u, bytes);
                bank->second.uploadedBytes = std::max(bank->second.uploadedBytes, offset + bytes);
                if (traceZaudio && bank->second.uploadedBytes == bank->second.data.size())
                {
                    size_t vagHeaders = 0u;
                    for (size_t i = 0u; i + 4u <= bank->second.data.size(); ++i)
                    {
                        if (std::memcmp(bank->second.data.data() + i, "VAGp", 4u) == 0)
                        {
                            ++vagHeaders;
                        }
                    }
                    std::cerr << "[ZAUDIO bank] handle=0x" << std::hex << handle
                              << " spu=0x" << bank->second.spuAddress << std::dec
                              << " bytes=" << bank->second.data.size()
                              << " vagHeaders=" << vagHeaders << '\n';
                }
            }
        }
        else if (rpcNum == kAllocateStream && sendBuf && sendSize >= 24u &&
                 recvBuf && recvSize >= sizeof(uint32_t))
        {
            const uint32_t flags = readLe32(sendBuf);
            const uint32_t handle = readLe32(recvBuf);
            if (handle != 0u)
            {
                Impl::ZaudioStream stream{};
                stream.channels = (flags & 0x20u) != 0u ? 4u
                                    : (flags & 0x02u) != 0u ? 2u
                                                                    : 1u;
                std::lock_guard<std::mutex> lock(m_mutex);
                m_impl->zaudioStreams[handle] = std::move(stream);
            }
        }
        else if (rpcNum == kFreeStream && sendBuf && sendSize >= sizeof(uint32_t))
        {
            const uint32_t handle = readLe32(sendBuf);
            std::lock_guard<std::mutex> lock(m_mutex);
#if !defined(PLATFORM_VITA)
            for (auto it = m_impl->activeMusic.begin(); it != m_impl->activeMusic.end();)
            {
                if (it->streamHandle == handle)
                {
                    StopMusicStream(it->music);
                    UnloadMusicStream(it->music);
                    it = m_impl->activeMusic.erase(it);
                }
                else
                {
                    ++it;
                }
            }
#endif
            m_impl->zaudioStreams.erase(handle);
        }
        else if (rpcNum == kConfigureStream && sendBuf && sendSize >= 8u)
        {
            const uint32_t handle = readLe32(sendBuf);
            const uint16_t parameterMask = readLe16(sendBuf + 4u);
            if ((parameterMask & 1u) != 0u && sendSize >= 8u)
            {
                const uint16_t pitch = readLe16(sendBuf + 6u);
                if (pitch != 0u)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_impl->zaudioStreams[handle].pitch = pitch;
                }
            }

            if ((parameterMask & 4u) != 0u && sendSize > 20u)
            {
                const uint32_t streamSize = readLe32(sendBuf + 12u);
                const uint32_t streamOffset = readLe32(sendBuf + 16u);
                const char *pathBegin = reinterpret_cast<const char *>(sendBuf + 20u);
                const char *pathEnd = reinterpret_cast<const char *>(sendBuf + sendSize);
                const char *terminator = std::find(pathBegin, pathEnd, '\0');
                const std::string guestPath(pathBegin, terminator);

                uint32_t channels = 2u;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    const auto stream = m_impl->zaudioStreams.find(handle);
                    if (stream != m_impl->zaudioStreams.end())
                        channels = stream->second.channels;
                }

                std::vector<int16_t> pcm;
                uint32_t sampleRate = 44100u;
                std::filesystem::path hostPath;
                const bool loaded = loadZsndPs2Stream(guestPath, streamOffset, streamSize,
                                                      channels, pcm, sampleRate, hostPath);
                if (loaded)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    Impl::ZaudioStream &stream = m_impl->zaudioStreams[handle];
                    stream.channels = channels;
                    stream.sampleRate = sampleRate;
                    stream.guestPath = guestPath;
                    stream.hostPath = std::move(hostPath);
                    stream.pcm = std::move(pcm);
                    if (traceZaudio)
                    {
                        const uint64_t frames = stream.pcm.size() / stream.channels;
                        std::cerr << "[ZAUDIO stream] handle=0x" << std::hex << handle << std::dec
                                  << " path=" << stream.hostPath.string()
                                  << " channels=" << stream.channels
                                  << " rate=" << stream.sampleRate
                                  << " frames=" << frames << '\n';
                    }
                }
                else if (traceZaudio)
                {
                    std::cerr << "[ZAUDIO stream] failed path=" << guestPath
                              << " offset=0x" << std::hex << streamOffset
                              << " bytes=0x" << streamSize << std::dec << '\n';
                }
            }
        }
        else if (rpcNum == kStartStream && sendBuf && sendSize >= sizeof(uint32_t))
        {
            const uint32_t handle = readLe32(sendBuf);
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto stream = m_impl->zaudioStreams.find(handle);
            if (stream != m_impl->zaudioStreams.end() && !stream->second.pcm.empty())
            {
                if (sendSize >= 12u)
                {
                    const uint16_t pitch = readLe16(sendBuf + 8u);
                    if (pitch != 0u)
                        stream->second.pitch = pitch;
                }
#if !defined(PLATFORM_VITA)
                for (auto it = m_impl->activeMusic.begin(); it != m_impl->activeMusic.end();)
                {
                    if (it->streamHandle == handle)
                    {
                        StopMusicStream(it->music);
                        UnloadMusicStream(it->music);
                        it = m_impl->activeMusic.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                if (m_audioReady)
                {
                    std::vector<uint8_t> wav = buildWavFromPcm(
                        stream->second.pcm.data(), stream->second.pcm.size(),
                        stream->second.sampleRate,
                        static_cast<uint16_t>(stream->second.channels));
                    Music music = LoadMusicStreamFromMemory(
                        ".wav", wav.data(), static_cast<int>(wav.size()));
                    if (music.frameCount > 0u)
                    {
                        music.looping = true;
                        SetMusicPitch(music, std::max(0.01f,
                            static_cast<float>(stream->second.pitch) / 4096.0f));
                        SetMusicVolume(music, 1.0f);
                        PlayMusicStream(music);
                        m_impl->activeMusic.push_back({music, std::move(wav), handle});
                        if (traceZaudio)
                            std::cerr << "[ZAUDIO stream] playing handle=0x"
                                      << std::hex << handle << std::dec << '\n';
                    }
                }
#endif
            }
        }
        else if (rpcNum == kConfigureVoice && sendBuf && sendSize >= 24u)
        {
            uint32_t voiceId = 0u;
            uint32_t parameterMask = 0u;
            uint32_t sampleAddress = 0u;
            uint32_t pitch = 0u;
            uint16_t volumeLeft = 0u;
            uint16_t volumeRight = 0u;
            std::memcpy(&voiceId, sendBuf, sizeof(voiceId));
            std::memcpy(&parameterMask, sendBuf + 4u, sizeof(parameterMask));
            std::memcpy(&sampleAddress, sendBuf + 8u, sizeof(sampleAddress));
            std::memcpy(&pitch, sendBuf + 12u, sizeof(pitch));
            std::memcpy(&volumeLeft, sendBuf + 16u, sizeof(volumeLeft));
            std::memcpy(&volumeRight, sendBuf + 18u, sizeof(volumeRight));

            if (voiceId < Impl::kZaudioVoiceCount)
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                Impl::ZaudioVoice &voice = m_impl->zaudioVoices[voiceId];
                if ((parameterMask & 1u) != 0u)
                    voice.sampleAddress = sampleAddress;
                if ((parameterMask & 2u) != 0u)
                    voice.pitch = static_cast<uint16_t>(pitch);
                if ((parameterMask & 4u) != 0u)
                {
                    voice.volumeLeft = volumeLeft;
                    voice.volumeRight = volumeRight;
                }

#if !defined(PLATFORM_VITA)
                const float hostPitch = std::max(0.01f, static_cast<float>(voice.pitch) / 4096.0f);
                const float left = static_cast<float>(voice.volumeLeft & 0x3FFFu) / 16383.0f;
                const float right = static_cast<float>(voice.volumeRight & 0x3FFFu) / 16383.0f;
                const float hostVolume = std::max(left, right);
                const float hostPan = (left + right) > 0.0f ? right / (left + right) : 0.5f;
                for (Impl::TrackedSound &tracked : m_impl->activeSounds)
                {
                    if (tracked.voiceId == voiceId)
                    {
                        SetSoundPitch(tracked.snd, hostPitch);
                        SetSoundVolume(tracked.snd, hostVolume);
                        SetSoundPan(tracked.snd, hostPan);
                    }
                }
#endif
            }
        }
        else if (rpcNum == kKeyOnVoices && sendBuf && sendSize >= 8u)
        {
            uint32_t voiceMask = 0u;
            std::memcpy(&voiceMask, sendBuf, sizeof(voiceMask));
            std::lock_guard<std::mutex> lock(m_mutex);

            for (uint32_t voiceId = 0u; voiceId < Impl::kZaudioVoiceCount; ++voiceId)
            {
                if ((voiceMask & (1u << voiceId)) == 0u)
                    continue;

#if !defined(PLATFORM_VITA)
                for (auto it = m_impl->activeSounds.begin(); it != m_impl->activeSounds.end();)
                {
                    if (it->voiceId == voiceId)
                    {
                        StopSound(it->snd);
                        UnloadSound(it->snd);
                        it = m_impl->activeSounds.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
#endif

                const Impl::ZaudioVoice &voice = m_impl->zaudioVoices[voiceId];
                if (voice.sampleAddress == 0u)
                    continue;

                DecodedSample *sample = nullptr;
                auto cached = m_sampleBank.find(voice.sampleAddress);
                if (cached != m_sampleBank.end())
                {
                    sample = &cached->second;
                }
                else
                {
                    const uint8_t *encoded = nullptr;
                    uint32_t availableBytes = 0u;
                    for (const auto &[handle, bank] : m_impl->zaudioBanks)
                    {
                        (void)handle;
                        const uint64_t bankBegin = bank.spuAddress;
                        const uint64_t bankEnd = bankBegin + bank.uploadedBytes;
                        if (voice.sampleAddress >= bankBegin && voice.sampleAddress < bankEnd)
                        {
                            const uint32_t bankOffset = voice.sampleAddress - bank.spuAddress;
                            encoded = bank.data.data() + bankOffset;
                            availableBytes = bank.uploadedBytes - bankOffset;
                            break;
                        }
                    }
                    if (!encoded)
                        continue;

                    uint32_t encodedBytes = 0u;
                    bool looping = false;
                    for (uint32_t offset = 0u; offset + 16u <= availableBytes; offset += 16u)
                    {
                        const uint8_t flags = encoded[offset + 1u];
                        if ((flags & 1u) != 0u)
                        {
                            encodedBytes = offset + 16u;
                            looping = (flags & 2u) != 0u;
                            break;
                        }
                    }
                    if (encodedBytes == 0u)
                        continue;

                    DecodedSample decoded{};
                    decoded.sampleRate = 44100u;
                    if (!ps2_vag::decodeBlocks(encoded, encodedBytes, decoded.pcm))
                        continue;
                    auto [inserted, wasInserted] = m_sampleBank.emplace(voice.sampleAddress, std::move(decoded));
                    (void)wasInserted;
                    sample = &inserted->second;

                    if (traceZaudio)
                    {
                        std::cerr << "[ZAUDIO sample] voice=" << voiceId
                                  << " address=0x" << std::hex << voice.sampleAddress << std::dec
                                  << " bytes=" << encodedBytes
                                  << " samples=" << sample->pcm.size()
                                  << " loop=" << (looping ? 1 : 0) << '\n';
                    }
                }

                const float hostPitch = std::max(0.01f, static_cast<float>(voice.pitch) / 4096.0f);
                const float left = static_cast<float>(voice.volumeLeft & 0x3FFFu) / 16383.0f;
                const float right = static_cast<float>(voice.volumeRight & 0x3FFFu) / 16383.0f;
                const float hostVolume = std::max(left, right);
                const size_t soundCountBeforePlay = m_impl->activeSounds.size();
                playDecodedSample(voice.sampleAddress, *sample, hostPitch, hostVolume, false, voiceId);
#if !defined(PLATFORM_VITA)
                if (m_impl->activeSounds.size() > soundCountBeforePlay)
                {
                    const float hostPan = (left + right) > 0.0f ? right / (left + right) : 0.5f;
                    SetSoundPan(m_impl->activeSounds.back().snd, hostPan);
                }
#endif
            }
        }
        else if (rpcNum == kKeyOffVoices && sendBuf && sendSize >= 8u)
        {
            uint32_t voiceMask = 0u;
            std::memcpy(&voiceMask, sendBuf, sizeof(voiceMask));
            std::lock_guard<std::mutex> lock(m_mutex);
#if !defined(PLATFORM_VITA)
            for (auto it = m_impl->activeSounds.begin(); it != m_impl->activeSounds.end();)
            {
                if (it->voiceId < Impl::kZaudioVoiceCount &&
                    (voiceMask & (1u << it->voiceId)) != 0u)
                {
                    StopSound(it->snd);
                    UnloadSound(it->snd);
                    it = m_impl->activeSounds.erase(it);
                }
                else
                {
                    ++it;
                }
            }
#endif
        }
        return;
    }

    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm, voiceIndex);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm, uint32_t voiceId)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    (void)voiceId;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        const bool samePlayback = voiceId != 0xFFFFFFFFu
                                      ? t.voiceId == voiceId
                                      : t.sampleKey == sampleKey;
        if (samePlayback && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey, voiceId});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    (void)voiceId;
#else
    for (auto it = m_impl->activeSounds.begin(); it != m_impl->activeSounds.end();)
    {
        if (it->voiceId == voiceId)
        {
            StopSound(it->snd);
            UnloadSound(it->snd);
            it = m_impl->activeSounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeMusic)
    {
        StopMusicStream(t.music);
        UnloadMusicStream(t.music);
    }
    m_impl->activeMusic.clear();
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}

void PS2AudioBackend::update()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    pruneFinishedSounds();
    for (Impl::TrackedMusic &tracked : m_impl->activeMusic)
        UpdateMusicStream(tracked.music);
#endif
}
