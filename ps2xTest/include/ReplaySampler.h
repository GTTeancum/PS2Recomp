#pragma once

#include <atomic>
#include <memory>

// Optional sampling of the calling replay-test thread, never another process.
class ReplaySampler
{
public:
    explicit ReplaySampler(bool enabled);
    ~ReplaySampler();
    std::atomic_bool *executionFlag();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
