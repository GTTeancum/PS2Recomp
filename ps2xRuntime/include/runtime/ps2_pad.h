#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

class PSPadBackend
{
public:
    PSPadBackend();
    ~PSPadBackend() = default;

    void pollHostState();
    bool readState(int port, int slot, uint8_t *data, size_t size);

private:
    std::mutex m_stateMutex;
    std::array<uint8_t, 32> m_state{};
};

#endif
