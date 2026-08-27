#include "module_factories.h"

#include <array>
#include <cstdint>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kPortOpenSid = 0x80000901u;
        constexpr uint32_t kPortCloseSid = 0x80000902u;
        constexpr uint32_t kGetConnectionSid = 0x80000903u;
        constexpr uint32_t kChangeThreadPrioritySid = 0x80000904u;
        constexpr uint32_t kGetModuleVersionSid = 0x80000905u;
        constexpr uint32_t kRpcFunction = 1u;
        constexpr uint32_t kModuleVersion = 0x0300u;

        class MtapService final : public IopService
        {
        public:
            MtapService(IopHost &host, bool connected)
                : m_host(host), m_connected(connected)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "XMTAPMAN compatibility service";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return m_sids;
            }

            void reset() override
            {
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                if (request.function != kRpcFunction)
                {
                    return {};
                }

                uint32_t responseOffset = 0u;
                uint32_t responseValue = 0u;
                switch (request.sid)
                {
                case kPortOpenSid:
                case kPortCloseSid:
                    responseOffset = sizeof(uint32_t);
                    responseValue = 1u;
                    break;
                case kGetConnectionSid:
                    responseOffset = sizeof(uint32_t);
                    responseValue = m_connected ? 1u : 0u;
                    break;
                case kChangeThreadPrioritySid:
                    responseOffset = sizeof(uint32_t) * 2u;
                    responseValue = 0u;
                    break;
                case kGetModuleVersionSid:
                    responseValue = kModuleVersion;
                    break;
                default:
                    return {};
                }

                if (request.receive.address != 0u &&
                    request.receive.size >= responseOffset + sizeof(responseValue))
                {
                    (void)m_host.writeGuest(request.receive.address + responseOffset,
                                            &responseValue,
                                            sizeof(responseValue));
                }

                RpcResult result;
                result.handled = true;
                result.resultAddress = request.receive.address;
                result.signalNowaitCompletion = true;
                return result;
            }

        private:
            IopHost &m_host;
            bool m_connected;
            const std::array<uint32_t, 5> m_sids{
                kPortOpenSid,
                kPortCloseSid,
                kGetConnectionSid,
                kChangeThreadPrioritySid,
                kGetModuleVersionSid,
            };
        };
    }

    std::unique_ptr<IopService> createMtapService(IopHost &host, bool connected)
    {
        return std::make_unique<MtapService>(host, connected);
    }
}
