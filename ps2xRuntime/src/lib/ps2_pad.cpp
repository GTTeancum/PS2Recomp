#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;

    uint8_t axisToByte(float axis)
    {
        if (axis < -1.0f)
            axis = -1.0f;
        else if (axis > 1.0f)
            axis = 1.0f;
        return static_cast<uint8_t>((axis + 1.0f) * 127.5f + 0.5f);
    }

    int findFirstGamepad()
    {
        for (int gamepad = 0; gamepad < 4; ++gamepad)
        {
            if (IsGamepadAvailable(gamepad))
                return gamepad;
        }
        return -1;
    }
}

PSPadBackend::PSPadBackend()
{
    m_state[0] = 0x01;
    m_state[1] = kPadAnalogMarker;
    m_state[2] = 0xFF;
    m_state[3] = 0xFF;
    m_state[4] = m_state[5] = m_state[6] = m_state[7] = kPadStickCenter;
}

void PSPadBackend::pollHostState()
{
    std::array<uint8_t, 32> state{};
    state[0] = 0x01;
    state[1] = kPadAnalogMarker;
    state[2] = 0xFF;
    state[3] = 0xFF;
    state[4] = state[5] = state[6] = state[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    const int gamepad = findFirstGamepad();
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (gamepad >= 0)
    {
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(gamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(gamepad, GAMEPAD_AXIS_RIGHT_Y);
        state[6] = axisToByte(lx);
        state[7] = axisToByte(ly);
        state[4] = axisToByte(rx);
        state[5] = axisToByte(ry);
    }
    if (IsKeyDown(KEY_UP))
        clearBit(PAD_UP);
    if (IsKeyDown(KEY_DOWN))
        clearBit(PAD_DOWN);
    if (IsKeyDown(KEY_LEFT))
        clearBit(PAD_LEFT);
    if (IsKeyDown(KEY_RIGHT))
        clearBit(PAD_RIGHT);

    const int leftX = (IsKeyDown(KEY_D) ? 1 : 0) - (IsKeyDown(KEY_A) ? 1 : 0);
    const int leftY = (IsKeyDown(KEY_S) ? 1 : 0) - (IsKeyDown(KEY_W) ? 1 : 0);
    const int rightX = (IsKeyDown(KEY_L) ? 1 : 0) - (IsKeyDown(KEY_J) ? 1 : 0);
    const int rightY = (IsKeyDown(KEY_K) ? 1 : 0) - (IsKeyDown(KEY_I) ? 1 : 0);
    if (leftX != 0)
        state[6] = leftX < 0 ? 0x00u : 0xFFu;
    if (leftY != 0)
        state[7] = leftY < 0 ? 0x00u : 0xFFu;
    if (rightX != 0)
        state[4] = rightX < 0 ? 0x00u : 0xFFu;
    if (rightY != 0)
        state[5] = rightY < 0 ? 0x00u : 0xFFu;

    if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
        clearBit(PAD_CROSS);
    if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
        clearBit(PAD_CIRCLE);
    if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
        clearBit(PAD_SQUARE);
    if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
        clearBit(PAD_TRIANGLE);
    if (IsKeyDown(KEY_Q))
        clearBit(PAD_L1);
    if (IsKeyDown(KEY_E))
        clearBit(PAD_R1);
    if (IsKeyDown(KEY_ONE))
        clearBit(PAD_L2);
    if (IsKeyDown(KEY_THREE))
        clearBit(PAD_R2);
    if (IsKeyDown(KEY_ENTER))
        clearBit(PAD_START);
    if (IsKeyDown(KEY_RIGHT_SHIFT) || IsKeyDown(KEY_TAB))
        clearBit(PAD_SELECT);
    if (IsKeyDown(KEY_LEFT_CONTROL))
        clearBit(PAD_L3);
    if (IsKeyDown(KEY_RIGHT_CONTROL))
        clearBit(PAD_R3);

    state[2] = static_cast<uint8_t>(btns & 0xFF);
    state[3] = static_cast<uint8_t>(btns >> 8);

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_state = state;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < m_state.size())
        return false;

    std::lock_guard<std::mutex> lock(m_stateMutex);
    std::memcpy(data, m_state.data(), m_state.size());
    return true;
}
