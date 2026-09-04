#include "runtime/ps2_vu1_native.h"
#include <mutex>

namespace
{
    VUNative::Host callbacks;
    std::once_flag initialized;
}

#define VU_NATIVE_KERNEL(Index, Word) void vuNativeKernel_##Index(VU1Interpreter *);
#include "vu_native_dispatch.inc"
#undef VU_NATIVE_KERNEL

void VU1Interpreter::updateFmacFlags(const uint8_t flags[4], uint8_t dest, uint32_t sticky)
{
    callbacks.fmac(this, flags, dest, sticky);
}
void VU1Interpreter::queueClip(uint32_t flags) { callbacks.clip(this, flags); }
void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t word)
{
    callbacks.reserved(this, upper, word);
}

namespace
{
    VU1Interpreter::UpperKernel lookup(uint32_t word)
    {
        switch (word)
        {
#define VU_NATIVE_KERNEL(Index, Word) case Word: return &vuNativeKernel_##Index;
#include "vu_native_dispatch.inc"
#undef VU_NATIVE_KERNEL
        default: return nullptr;
        }
    }
}

extern "C" __declspec(dllexport) VU1Interpreter::UpperLookup
ps2x_vu_native_initialize(const VUNative::Host *host, size_t bytes)
{
    if (!host || bytes != sizeof(VUNative::Host) || !VUNative::compatible(*host))
        return nullptr;
    std::call_once(initialized, [host] { callbacks = *host; });
    if (callbacks.fmac != host->fmac || callbacks.clip != host->clip || callbacks.reserved != host->reserved)
        return nullptr;
    return lookup;
}
