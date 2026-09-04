#include "runtime/ps2_vu1_native_module.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

VUNativeModule::VUNativeModule(const std::filesystem::path &path)
{
    if (!path.is_absolute())
        return;
    m_module = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (m_module)
        m_initialize = reinterpret_cast<VUNative::Initialize>(
            GetProcAddress(static_cast<HMODULE>(m_module), "ps2x_vu_native_initialize"));
}

VUNativeModule::~VUNativeModule()
{
    if (m_module)
        FreeLibrary(static_cast<HMODULE>(m_module));
}

VU1Interpreter::UpperLookup VUNativeModule::initialize(const VUNative::Host &host, size_t bytes) const
{
    return m_initialize ? m_initialize(&host, bytes) : nullptr;
}
