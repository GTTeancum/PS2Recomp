#include "ReplaySampler.h"

#include <cstdio>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <unordered_map>

struct ReplaySampler::Impl
{
    struct CloseThread
    {
        void operator()(void *handle) const { CloseHandle(handle); }
    };
    std::unique_ptr<void, CloseThread> target;
    std::jthread worker;
    std::unordered_map<uintptr_t, uint32_t> hits;
    uint32_t samples = 0, external = 0, dropped = 0, failures = 0;

    Impl()
    {
        HANDLE handle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                             &handle, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0u))
            throw std::runtime_error("Cannot duplicate the replay thread handle");
        target.reset(handle);
        hits.reserve(4096u);
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const auto end = base + nt->OptionalHeader.SizeOfImage;
        std::printf("[vu-sampler:image] timestamp=0x%08x\n", static_cast<unsigned>(nt->FileHeader.TimeDateStamp));
        worker = std::jthread([this, base, end](std::stop_token stop)
        {
            while (!stop.stop_requested() && samples < 8192u)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (stop.stop_requested())
                    break;
                if (SuspendThread(target.get()) == static_cast<DWORD>(-1))
                {
                    ++failures;
                    continue;
                }
                // Do not allocate, log, or take CRT locks while the owner is suspended.
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;
                const bool captured = GetThreadContext(target.get(), &context) != FALSE;
                if (ResumeThread(target.get()) == static_cast<DWORD>(-1))
                {
                    // A broken profiler must never strand its own test thread.
                    TerminateProcess(GetCurrentProcess(), 3u);
                    return;
                }
                if (!captured)
                {
                    ++failures;
                    continue;
                }
                ++samples;
                const auto ip = static_cast<uintptr_t>(context.Rip);
                if (ip < base || ip >= end)
                {
                    ++external;
                    continue;
                }
                const auto rva = ip - base;
                const auto existing = hits.find(rva);
                if (existing != hits.end())
                    ++existing->second;
                else if (hits.size() < 4096u)
                    hits.emplace(rva, 1u);
                else
                    ++dropped;
            }
        });
    }

    ~Impl()
    {
        worker.request_stop();
        worker.join();
        std::printf("[vu-sampler:summary] samples=%u external=%u unique=%zu dropped=%u failures=%u timings-instrumented=1\n",
                    samples, external, hits.size(), dropped, failures);
        for (const auto &[rva, count] : hits)
            std::printf("[vu-sampler:ip] rva=0x%llx hits=%u\n", static_cast<unsigned long long>(rva), count);
    }
};

ReplaySampler::ReplaySampler(bool enabled)
{
    if (enabled)
        m_impl = std::make_unique<Impl>();
}
#else
struct ReplaySampler::Impl {};

ReplaySampler::ReplaySampler(bool enabled)
{
    if (enabled)
        std::fputs("[vu-sampler:unsupported] requires Windows x64\n", stderr);
}
#endif

ReplaySampler::~ReplaySampler() = default;
