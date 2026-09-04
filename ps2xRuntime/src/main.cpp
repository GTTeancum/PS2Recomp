#include "ps2_runtime.h"
#include "games_database.h"
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
#include "runtime/ps2_vu1_native_module.h"
#include <memory>
#endif
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
#include "ps2_debug_panel.h"
#endif

#ifdef _DEBUG
#include "ps2_log.h"
#endif

#include <iostream>
#include <string>
#include <filesystem>
#include <exception>
#include <algorithm>
#include <cstdlib>
#include <cstdio>

#if defined(__ANDROID__)
#include <android/log.h>
#include <unistd.h>
#include <thread>
#include <cstdio>
#include <cstring>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" void ps2xDumpDispatchHistoryToStderr();

namespace
{
#if defined(_WIN32)
    PS2Runtime *g_activeRuntime = nullptr;

    LONG WINAPI logUnhandledSehException(PEXCEPTION_POINTERS info)
    {
        const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0u;
        if (code == 0xe06d7363u)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const void *address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
        uint32_t guestPc = 0u;
        uint32_t guestRa = 0u;
        uint32_t guestSp = 0u;
        uint32_t guestGp = 0u;
        if (g_activeRuntime)
        {
            guestPc = g_activeRuntime->m_debugPc.load(std::memory_order_relaxed);
            guestRa = g_activeRuntime->m_debugRa.load(std::memory_order_relaxed);
            guestSp = g_activeRuntime->m_debugSp.load(std::memory_order_relaxed);
            guestGp = g_activeRuntime->m_debugGp.load(std::memory_order_relaxed);
        }

        const void *moduleBase = GetModuleHandleW(nullptr);
        std::fprintf(stderr,
                     "[seh] code=0x%08lx address=%p module_base=%p guest_pc=0x%08x guest_ra=0x%08x guest_sp=0x%08x guest_gp=0x%08x\n",
                     static_cast<unsigned long>(code),
                     address,
                     moduleBase,
                     guestPc,
                     guestRa,
                     guestSp,
                     guestGp);
        ps2xDumpDispatchHistoryToStderr();
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

#if defined(__ANDROID__)
    int g_logcatPipeFds[2]{-1, -1};
    std::thread g_logcatThread;

    void stopLogcatRedirect()
    {
        std::fflush(stdout);
        std::fflush(stderr);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        if (g_logcatPipeFds[1] >= 0)
        {
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
        }
        if (g_logcatThread.joinable())
        {
            g_logcatThread.join();
        }
    }

    void redirectStdioToLogcat()
    {
        if (pipe(g_logcatPipeFds) != 0)
        {
            return;
        }

        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(g_logcatPipeFds[1], STDOUT_FILENO);
        dup2(g_logcatPipeFds[1], STDERR_FILENO);

        g_logcatThread = std::thread([]()
                                     {
                                         FILE *reader = fdopen(g_logcatPipeFds[0], "r");
                                         if (!reader)
                                         {
                                             return;
                                         }
                                         char line[1024];
                                         while (fgets(line, sizeof(line), reader))
                                         {
                                             size_t len = std::strlen(line);
                                             if (len > 0 && line[len - 1] == '\n')
                                             {
                                                 line[len - 1] = '\0';
                                             }
                                             __android_log_write(ANDROID_LOG_INFO, "ps2x", line);
                                         }
                                         fclose(reader);
                                         g_logcatPipeFds[0] = -1;
                                     });
        if (std::atexit(stopLogcatRedirect) != 0)
        {
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
            close(g_logcatPipeFds[1]);
            g_logcatPipeFds[1] = -1;
            if (g_logcatThread.joinable())
            {
                g_logcatThread.join();
            }
        }
    }
#endif

    void setupTerminateLogger() // to help on release build crashs
    {
        std::set_terminate([]()
                           {
                               std::cerr << "[terminate] unhandled exception" << std::endl;
                               const std::exception_ptr ep = std::current_exception();
                               if (ep)
                               {
                                   try
                                   {
                                       std::rethrow_exception(ep);
                                   }
                                   catch (const std::system_error &e)
                                   {
                                       std::cerr << "[terminate] std::system_error code=" << e.code().value()
                                                 << " category=" << e.code().category().name()
                                                 << " message=" << e.what() << std::endl;
                                   }
                                   catch (const std::exception &e)
                                   {
                                       std::cerr << "[terminate] std::exception: " << e.what() << std::endl;
                                   }
                                   catch (...)
                                   {
                                       std::cerr << "[terminate] non-std exception" << std::endl;
                                   }
                               }
                               std::abort(); });
    }

    std::string normalizeGameId(const std::string &folderName)
    {
        std::string result = folderName;

        size_t underscore = result.find('_');
        if (underscore != std::string::npos)
            result[underscore] = '-';

        size_t dot = result.find('.');
        if (dot != std::string::npos)
            result.erase(dot, 1);

        std::ranges::transform(result, result.begin(), [](unsigned char character)
                               { return static_cast<char>(std::toupper(character)); });

        return result;
    }

    std::filesystem::path getExecutablePath(int argc, char *argv[])
    {
        if (argc >= 2 && argv[1] && argv[1][0] != '\0')
        {
            std::cout << "Using argv boot path" << std::endl;
            return std::filesystem::path(argv[1]);
        }
#if defined(PS2X_DEFAULT_BOOT_ELF)
        std::cout << "Using default boot file" << std::endl;
        const std::filesystem::path configuredPath = std::filesystem::path(PS2X_DEFAULT_BOOT_ELF);
#if defined(PLATFORM_VITA)
        return configuredPath;
#endif
        if (configuredPath.is_absolute())
        {
            return configuredPath;
        }
        return (std::filesystem::current_path() / configuredPath).lexically_normal();
#else
        throw std::runtime_error("Unable to determine executable path. Pass the guest ELF as argv[1] or define PS2X_DEFAULT_BOOT_ELF.");
#endif
    }
}

int main(int argc, char *argv[])
{
#if defined(__ANDROID__)
    redirectStdioToLogcat();
#endif
#if defined(_WIN32)
    AddVectoredExceptionHandler(1, logUnhandledSehException);
#endif
    setupTerminateLogger();

    try
    {
        std::filesystem::path pathObj = getExecutablePath(argc, argv);

        std::string filePathStr = pathObj.string();
        std::string elfName = pathObj.filename().string();
        std::string normalizedId = normalizeGameId(elfName);

        std::string windowTitle = "PS2-Recomp | ";
        const char *gameName = getGameName(normalizedId);

#if !defined(PLATFORM_VITA)
        if (gameName)
        {
            windowTitle += std::string(gameName) + " | " + elfName;
        }
        else
#endif
        {
            windowTitle += elfName;
        }

        const char *nativeModulePath = std::getenv("PS2X_VU_NATIVE_MODULE");
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
        // Declared before runtime so early-return unwinding cannot unload live kernels.
        std::unique_ptr<VUNativeModule> nativeModule;
        VU1Interpreter::UpperLookup nativeLookup = nullptr;
        if (nativeModulePath && *nativeModulePath)
        {
            nativeModule = std::make_unique<VUNativeModule>(nativeModulePath);
            nativeLookup = nativeModule->initialize(VUNative::makeHost());
            if (!nativeLookup)
                throw std::runtime_error("Unable to load matching native VU module; use an absolute path to the DLL from this build");
        }
#else
        if (nativeModulePath && *nativeModulePath)
            throw std::runtime_error("This runner was built without native VU upper support");
#endif
        PS2Runtime runtime;
#if defined(_WIN32)
        g_activeRuntime = &runtime;
#endif
#if defined(PS2X_ENABLE_DEBUG_UI) && !defined(PLATFORM_VITA)
        // This hook is to prevent leak rlimgui deps to recompiler etc
        PS2DebugPanel debugPanel;
        runtime.setDebugUiCallbacks(
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->initialize();
            },
            [](PS2Runtime &rt, void *userData)
            {
                static_cast<PS2DebugPanel *>(userData)->draw(rt);
            },
            [](PS2Runtime &rt, void *userData)
            {
                (void)rt;
                static_cast<PS2DebugPanel *>(userData)->shutdown();
            },
            &debugPanel);
#endif
        if (!runtime.initialize(windowTitle.c_str()))
        {
            std::cerr << "Failed to initialize PS2 runtime" << std::endl;
            return 1;
        }

        if (!runtime.loadELF(filePathStr))
        {
            std::cerr << "Failed to load ELF file: " << filePathStr << std::endl;
            return 1;
        }

#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
        runtime.vu0().setUpperLookup(nativeLookup);
        runtime.vu1().setUpperLookup(nativeLookup);
        std::fprintf(stderr, "[vu:native] mode=%s fingerprint=%s\n",
                     nativeLookup ? "native-with-fallback" : "interpreter",
                     PS2X_VU_NATIVE_FINGERPRINT);
#endif
#if defined(PS2X_ENABLE_VU_NATIVE_PAIRS)
        const bool nativePairs = std::getenv("PS2X_VU_NATIVE_PAIRS") != nullptr;
        runtime.vu1().setNativePairsEnabled(nativePairs);
        std::fprintf(stderr, "[vu:pairs] mode=%s\n", nativePairs ? "native-with-fallback" : "interpreter");
#endif
        runtime.run();
#if defined(PS2X_ENABLE_VU_NATIVE_PAIRS)
        const auto pairCounters = runtime.vu1().pairCounters();
        std::fprintf(stderr, "[vu:pairs] stopped vu1=%llu/%llu (native/interpreted)\n",
                     static_cast<unsigned long long>(pairCounters.native),
                     static_cast<unsigned long long>(pairCounters.interpreted));
#endif
#if defined(PS2X_ENABLE_VU_NATIVE_UPPER)
        // run() joins the game thread before these non-atomic counters are read.
        const auto vu0Counters = runtime.vu0().upperCounters();
        const auto vu1Counters = runtime.vu1().upperCounters();
        std::fprintf(stderr, "[vu:native] stopped vu0=%llu/%llu vu1=%llu/%llu (native/interpreted)\n",
                     static_cast<unsigned long long>(vu0Counters.native),
                     static_cast<unsigned long long>(vu0Counters.interpreted),
                     static_cast<unsigned long long>(vu1Counters.native),
                     static_cast<unsigned long long>(vu1Counters.interpreted));
        runtime.vu0().setUpperLookup(nullptr);
        runtime.vu1().setUpperLookup(nullptr);
        nativeModule.reset();
#endif

#ifdef _DEBUG
        ps2_log::print_saved_location();
#endif
        std::cout.flush();
        std::cerr.flush();
        std::_Exit(0);
    }
    catch (const std::exception &e)
    {
        std::cerr << "[main] fatal exception: " << e.what() << std::endl;
    }
    catch (...)
    {
        std::cerr << "[main] fatal exception: unknown" << std::endl;
    }

    std::cout.flush();
    std::cerr.flush();
    std::_Exit(1);
}
