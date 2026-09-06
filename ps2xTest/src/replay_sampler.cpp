#include "ReplaySampler.h"
#include "MiniTest.h"

#include <cstdio>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    struct ModuleSample
    {
        std::wstring path;
        uint32_t timestamp, size, rva;
    };

    std::optional<ModuleSample> resolveModule(uintptr_t ip)
    {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(ip), &module))
            return std::nullopt;
        struct ReleaseModule
        {
            HMODULE module;
            ~ReleaseModule() { FreeLibrary(module); }
        } release{module};
        const auto base = reinterpret_cast<uintptr_t>(module);
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return std::nullopt;
        const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || ip < base || ip - base >= nt->OptionalHeader.SizeOfImage)
            return std::nullopt;
        wchar_t path[32768];
        const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (length == 0 || length >= std::size(path))
            return std::nullopt;
        return ModuleSample{std::wstring(path, length), nt->FileHeader.TimeDateStamp,
                            nt->OptionalHeader.SizeOfImage, static_cast<uint32_t>(ip - base)};
    }

    struct ExternalSamples
    {
        static constexpr size_t maxModules = 64, maxAddresses = 4096;
        std::vector<ModuleSample> modules;
        std::unordered_map<uint64_t, uint32_t> hits;
        uint32_t unresolved = 0, dropped = 0;

        void add(const std::optional<ModuleSample> &sample)
        {
            if (!sample)
            {
                ++unresolved;
                return;
            }
            size_t index = 0;
            for (; index < modules.size(); ++index)
            {
                const auto &known = modules[index];
                if (known.path == sample->path && known.timestamp == sample->timestamp && known.size == sample->size)
                    break;
            }
            if (index == modules.size())
            {
                if (modules.size() == maxModules || hits.size() == maxAddresses)
                {
                    ++dropped;
                    return;
                }
                modules.push_back(*sample);
            }
            const auto key = (static_cast<uint64_t>(index) << 32u) | sample->rva;
            const auto existing = hits.find(key);
            if (existing != hits.end())
                ++existing->second;
            else if (hits.size() < maxAddresses)
                hits.emplace(key, 1u);
            else
                ++dropped;
        }

        void print() const
        {
            std::printf("[vu-sampler:external-summary] modules=%zu unique=%zu unresolved=%u dropped=%u\n",
                        modules.size(), hits.size(), unresolved, dropped);
            for (size_t index = 0; index < modules.size(); ++index)
            {
                const auto &module = modules[index];
                const int length = WideCharToMultiByte(CP_UTF8, 0, module.path.data(),
                    static_cast<int>(module.path.size()), nullptr, 0, nullptr, nullptr);
                std::string path(length, '\0');
                WideCharToMultiByte(CP_UTF8, 0, module.path.data(), static_cast<int>(module.path.size()),
                                    path.data(), length, nullptr, nullptr);
                std::printf("[vu-sampler:module] id=%zu timestamp=0x%08x size=0x%x path=\"%s\"\n",
                            index, module.timestamp, module.size, path.c_str());
            }
            for (const auto &[key, count] : hits)
                std::printf("[vu-sampler:external-ip] module=%u rva=0x%x hits=%u\n",
                            static_cast<uint32_t>(key >> 32u), static_cast<uint32_t>(key), count);
        }
    };
}

struct ReplaySampler::Impl
{
    struct CloseThread
    {
        void operator()(void *handle) const { CloseHandle(handle); }
    };
    std::unique_ptr<void, CloseThread> target;
    std::jthread worker;
    std::unordered_map<uintptr_t, uint32_t> hits;
    ExternalSamples externalSamples;
    std::atomic_bool executing{false};
    uint32_t samples = 0, outside = 0, external = 0, dropped = 0, failures = 0;
    static_assert(std::atomic_bool::is_always_lock_free);

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
            while (!stop.stop_requested() && samples + outside < 8192u)
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
                // Read while suspended so the phase and instruction address describe the same instant.
                const bool inExecution = executing.load(std::memory_order_relaxed);
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
                if (!inExecution)
                {
                    ++outside;
                    continue;
                }
                ++samples;
                const auto ip = static_cast<uintptr_t>(context.Rip);
                if (ip < base || ip >= end)
                {
                    ++external;
                    // The owner is running again before any loader lock or allocation is possible.
                    externalSamples.add(resolveModule(ip));
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
        std::printf("[vu-sampler:summary] samples=%u external=%u unique=%zu dropped=%u failures=%u timings-instrumented=1 execution-only=1 outside=%u\n",
                    samples, external, hits.size(), dropped, failures, outside);
        for (const auto &[rva, count] : hits)
            std::printf("[vu-sampler:ip] rva=0x%llx hits=%u\n", static_cast<unsigned long long>(rva), count);
        externalSamples.print();
    }
};

ReplaySampler::ReplaySampler(bool enabled)
{
    if (enabled)
        m_impl = std::make_unique<Impl>();
}

std::atomic_bool *ReplaySampler::executionFlag()
{
    return m_impl ? &m_impl->executing : nullptr;
}
#else
struct ReplaySampler::Impl {};

ReplaySampler::ReplaySampler(bool enabled)
{
    if (enabled)
        std::fputs("[vu-sampler:unsupported] requires Windows x64\n", stderr);
}

std::atomic_bool *ReplaySampler::executionFlag() { return nullptr; }
#endif

ReplaySampler::~ReplaySampler() = default;

void register_replay_sampler_tests()
{
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    MiniTest::Case("VUReplaySampler", [](TestCase &tc)
    {
        tc.Run("VU sampler resolves loaded code and rejects non-module addresses", [](TestCase &t)
        {
            const auto address = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetCurrentThreadId"));
            const auto sample = resolveModule(address);
            t.IsTrue(sample.has_value(), "A live Windows export should resolve to its owning image");
            if (sample)
            {
                t.IsTrue(!sample->path.empty(), "The image path must be available for binary inspection");
                t.IsTrue(sample->rva < sample->size, "The sampled instruction must belong to the image");
            }
            t.IsFalse(resolveModule(0u).has_value(), "Null must not be attributed to the executable");
            void *allocation = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            t.IsNotNull(allocation, "The non-module fixture must allocate");
            if (allocation)
            {
                const auto unknown = resolveModule(reinterpret_cast<uintptr_t>(allocation));
                VirtualFree(allocation, 0, MEM_RELEASE);
                t.IsFalse(unknown.has_value(), "Private memory is not a loaded PE module");
            }
        });
        tc.Run("VU sampler preserves module identities and bounded external counts", [](TestCase &t)
        {
            ExternalSamples samples;
            samples.add(std::nullopt);
            ModuleSample sample{L"fixture.dll", 1u, 0x10000u, 0x10u};
            samples.add(sample);
            samples.add(sample);
            ++sample.timestamp;
            samples.add(sample);
            ++sample.size;
            samples.add(sample);
            sample.path = L"other.dll";
            samples.add(sample);
            t.Equals(samples.unresolved, 1u, "Unknown addresses remain explicit");
            t.Equals(samples.hits.at(0x10u), 2u, "Repeated addresses retain every hit");
            t.Equals(samples.modules.size(), size_t{4}, "Path, timestamp and size distinguish module identities");
            for (size_t i = samples.modules.size(); i <= ExternalSamples::maxModules; ++i)
            {
                sample.timestamp = static_cast<uint32_t>(i + 10);
                samples.add(sample);
            }
            t.Equals(samples.modules.size(), ExternalSamples::maxModules, "Module metadata storage is bounded");
            t.Equals(samples.dropped, 1u, "Module overflow is accounted for");
            ExternalSamples addresses;
            for (size_t i = 0; i <= ExternalSamples::maxAddresses; ++i)
            {
                sample.rva = static_cast<uint32_t>(i);
                addresses.add(sample);
            }
            sample.rva = 0;
            addresses.add(sample);
            t.Equals(addresses.hits.size(), ExternalSamples::maxAddresses, "Address storage is bounded");
            t.Equals(addresses.dropped, 1u, "New addresses beyond capacity are counted as dropped");
            t.Equals(addresses.hits.at(0u), 2u, "Known addresses still accumulate after capacity is reached");
        });
    });
#endif
}
