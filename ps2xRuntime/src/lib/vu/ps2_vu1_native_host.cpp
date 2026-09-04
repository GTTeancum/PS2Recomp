#include "runtime/ps2_vu1_native.h"

struct VUNativeAccess
{
    static void fmac(VU1Interpreter *vu, const uint8_t *flags, uint8_t dest, uint32_t sticky)
    {
        vu->updateFmacFlags(flags, dest, sticky);
    }
    static void clip(VU1Interpreter *vu, uint32_t flags) { vu->queueClip(flags); }
    static void reserved(VU1Interpreter *vu, bool upper, uint32_t word)
    {
        vu->reportReservedInstruction(upper, word);
    }
};

VUNative::Host VUNative::makeHost()
{
    Host host;
    host.fmac = VUNativeAccess::fmac;
    host.clip = VUNativeAccess::clip;
    host.reserved = VUNativeAccess::reserved;
    return host;
}
