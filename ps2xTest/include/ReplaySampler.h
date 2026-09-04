#pragma once

#include <memory>

// Optional sampling of the calling replay-test thread, never another process.
class ReplaySampler
{
public:
    explicit ReplaySampler(bool enabled);
    ~ReplaySampler();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
