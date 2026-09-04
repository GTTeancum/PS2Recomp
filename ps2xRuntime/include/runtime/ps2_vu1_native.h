#pragma once

#include "runtime/ps2_vu1.h"
#include "vu_native_build_id.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

// Experimental, same-build module interface. Not a stable plugin ABI.
namespace VUNative
{
    struct Host
    {
        uint32_t version = 1u;
        uint32_t bytes = sizeof(Host);
        uint32_t compiler = _MSC_FULL_VER;
        uint32_t iteratorDebug = _ITERATOR_DEBUG_LEVEL;
        uint64_t interpreterBytes = sizeof(VU1Interpreter);
        uint64_t stateBytes = sizeof(VU1State);
        char fingerprint[65] = PS2X_VU_NATIVE_FINGERPRINT;
        char configuration[32] = PS2X_VU_NATIVE_CONFIGURATION;
        void (*fmac)(VU1Interpreter *, const uint8_t *, uint8_t, uint32_t) = nullptr;
        void (*clip)(VU1Interpreter *, uint32_t) = nullptr;
        void (*reserved)(VU1Interpreter *, bool, uint32_t) = nullptr;
    };

    inline bool compatible(const Host &host)
    {
        const Host expected;
        return host.version == expected.version && host.bytes == expected.bytes &&
            host.compiler == expected.compiler && host.iteratorDebug == expected.iteratorDebug &&
            host.interpreterBytes == expected.interpreterBytes && host.stateBytes == expected.stateBytes &&
            std::memcmp(host.fingerprint, expected.fingerprint, sizeof(host.fingerprint)) == 0 &&
            std::memcmp(host.configuration, expected.configuration, sizeof(host.configuration)) == 0 &&
            host.fmac && host.clip && host.reserved;
    }

    using Initialize = VU1Interpreter::UpperLookup (*)(const Host *, size_t);
    Host makeHost();
}
