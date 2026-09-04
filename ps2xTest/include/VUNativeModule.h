#pragma once

#include "runtime/ps2_vu1_native.h"
#include <filesystem>

class VUNativeModule
{
public:
    explicit VUNativeModule(const std::filesystem::path &path);
    ~VUNativeModule();
    VUNativeModule(const VUNativeModule &) = delete;
    VUNativeModule &operator=(const VUNativeModule &) = delete;
    VU1Interpreter::UpperLookup initialize(const VUNative::Host &host, size_t bytes = sizeof(VUNative::Host)) const;

private:
    void *m_module = nullptr;
    VUNative::Initialize m_initialize = nullptr;
};
