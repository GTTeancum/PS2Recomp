#include "module_factories.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1u) & ~(alignment - 1u);
        }

        struct SampleAllocation
        {
            uint32_t spuAddress = 0u;
            uint32_t size = 0u;
            uint32_t uploadedBytes = 0u;
        };

        struct StreamAllocation
        {
            uint32_t flags = 0u;
            uint32_t channels = 1u;
        };

        class ZaudioService final : public IopService
        {
        public:
            ZaudioService(IopHost &host, ZaudioBindings bindings)
                : m_host(host), m_bindings(std::move(bindings)), m_sids{m_bindings.sid}
            {
                reset();
            }

            [[nodiscard]] std::string_view name() const override
            {
                return m_bindings.serviceName;
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return m_sids;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_samples.clear();
                m_streams.clear();
                m_nextHandle = m_bindings.firstHandle;
                m_nextStreamHandle = 0x00100000u;
                m_nextSpuAddress = m_bindings.firstSpuAddress;
                m_rpcCount = 0u;
                m_uploadCount = 0u;
                m_rejectedUploadCount = 0u;
                m_functionCalls.fill(0u);
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.sid != m_bindings.sid)
                {
                    return {};
                }

                RpcResult result{};
                result.handled = true;
                result.resultAddress = request.receive.address;
                result.signalNowaitCompletion = true;

                if (request.receive.address != 0u && request.receive.size != 0u)
                {
                    (void)m_host.zeroGuest(request.receive.address, request.receive.size);
                }

                if (request.function == m_bindings.initializeFunction)
                {
                    writeResponseWord(request, 0u, 1u);
                }
                else if (request.function == m_bindings.allocateSampleFunction)
                {
                    allocateSample(request);
                }
                else if (request.function == m_bindings.uploadSampleFunction)
                {
                    accountUpload(request);
                }
                else if (request.function == 8u)
                {
                    allocateStream(request);
                }
                else if (request.function == 9u)
                {
                    freeStream(request);
                }

                bool firstFunctionCall = false;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_rpcCount;
                    if (request.function < m_functionCalls.size())
                    {
                        firstFunctionCall = m_functionCalls[request.function]++ == 0u;
                    }
                }
                if (firstFunctionCall && request.function != m_bindings.uploadSampleFunction)
                {
                    m_host.log(LogLevel::Info,
                               "ZAUDIO function " + std::to_string(request.function) +
                                   " send=" + std::to_string(request.send.size) +
                                   " receive=" + std::to_string(request.receive.size));
                }
                m_host.audioCommand(request.sid, request.function, request.send, request.receive);
                return result;
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                metrics.push_back({"rpc_calls", m_rpcCount, false});
                metrics.push_back({"sample_banks", m_samples.size(), false});
                metrics.push_back({"streams", m_streams.size(), false});
                metrics.push_back({"upload_packets", m_uploadCount, false});
                metrics.push_back({"rejected_uploads", m_rejectedUploadCount, false});
            }

        private:
            void writeResponseWord(const RpcRequest &request, uint32_t offset, uint32_t value)
            {
                if (request.receive.address != 0u &&
                    request.receive.size >= offset + sizeof(value))
                {
                    (void)m_host.writeGuest(request.receive.address + offset, &value, sizeof(value));
                }
            }

            void allocateSample(const RpcRequest &request)
            {
                uint32_t size = 0u;
                if (request.send.address == 0u || request.send.size < sizeof(size) ||
                    !m_host.readGuest(request.send.address, &size, sizeof(size)) || size == 0u)
                {
                    return;
                }

                uint32_t handle = 0u;
                uint32_t spuAddress = 0u;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    handle = m_nextHandle;
                    m_nextHandle += m_bindings.handleStride;
                    spuAddress = alignUp(m_nextSpuAddress, 64u);
                    m_nextSpuAddress = spuAddress + alignUp(size, 64u);
                    m_samples.emplace(handle, SampleAllocation{spuAddress, size, 0u});
                }

                writeResponseWord(request, 0u, handle);
                writeResponseWord(request, sizeof(uint32_t), spuAddress);
                m_host.log(LogLevel::Info,
                           "ZAUDIO allocated bank handle=" + std::to_string(handle) +
                               " spu=" + std::to_string(spuAddress) +
                               " bytes=" + std::to_string(size));
            }

            void accountUpload(const RpcRequest &request)
            {
                if (request.send.address == 0u || request.send.size != m_bindings.uploadPacketBytes)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_rejectedUploadCount;
                    return;
                }

                std::array<uint32_t, 2> header{};
                if (!m_host.readGuest(request.send.address, header.data(), sizeof(header)))
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_rejectedUploadCount;
                    return;
                }

                std::lock_guard<std::mutex> lock(m_mutex);
                const auto sample = m_samples.find(header[0]);
                if (sample == m_samples.end() || header[1] >= sample->second.size)
                {
                    ++m_rejectedUploadCount;
                    return;
                }
                const uint32_t bytes = std::min(m_bindings.uploadPayloadBytes,
                                                sample->second.size - header[1]);
                sample->second.uploadedBytes = std::max(sample->second.uploadedBytes,
                                                        header[1] + bytes);
                ++m_uploadCount;
                if (header[1] == 0u || sample->second.uploadedBytes == sample->second.size)
                {
                    m_host.log(LogLevel::Info,
                               "ZAUDIO upload handle=" + std::to_string(header[0]) +
                                   " bytes=" + std::to_string(sample->second.uploadedBytes) +
                                   "/" + std::to_string(sample->second.size));
                }
            }

            void allocateStream(const RpcRequest &request)
            {
                uint32_t flags = 0u;
                if (request.send.address == 0u || request.send.size < 24u ||
                    !m_host.readGuest(request.send.address, &flags, sizeof(flags)))
                {
                    return;
                }

                const uint32_t channels = (flags & 0x20u) != 0u ? 4u
                                          : (flags & 0x02u) != 0u ? 2u
                                                                  : 1u;
                uint32_t handle = 0u;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    handle = m_nextStreamHandle;
                    m_nextStreamHandle += 0x100u;
                    m_streams.emplace(handle, StreamAllocation{flags, channels});
                }
                writeResponseWord(request, 0u, handle);
                m_host.log(LogLevel::Info,
                           "ZAUDIO allocated stream handle=" + std::to_string(handle) +
                               " channels=" + std::to_string(channels));
            }

            void freeStream(const RpcRequest &request)
            {
                uint32_t handle = 0u;
                if (request.send.address == 0u || request.send.size < sizeof(handle) ||
                    !m_host.readGuest(request.send.address, &handle, sizeof(handle)))
                {
                    return;
                }

                std::lock_guard<std::mutex> lock(m_mutex);
                m_streams.erase(handle);
            }

            IopHost &m_host;
            ZaudioBindings m_bindings;
            std::array<uint32_t, 1> m_sids;
            mutable std::mutex m_mutex;
            std::unordered_map<uint32_t, SampleAllocation> m_samples;
            std::unordered_map<uint32_t, StreamAllocation> m_streams;
            uint32_t m_nextHandle = 0u;
            uint32_t m_nextStreamHandle = 0u;
            uint32_t m_nextSpuAddress = 0u;
            uint64_t m_rpcCount = 0u;
            uint64_t m_uploadCount = 0u;
            uint64_t m_rejectedUploadCount = 0u;
            std::array<uint64_t, 32> m_functionCalls{};
        };
    }

    std::unique_ptr<IopService> createZaudioService(IopHost &host, ZaudioBindings bindings)
    {
        if (bindings.serviceName.empty() || bindings.sid == 0u ||
            bindings.uploadPacketBytes < bindings.uploadPayloadOffset + bindings.uploadPayloadBytes ||
            bindings.handleStride == 0u)
        {
            throw std::invalid_argument("invalid ZAUDIO bindings");
        }
        return std::make_unique<ZaudioService>(host, std::move(bindings));
    }
}
