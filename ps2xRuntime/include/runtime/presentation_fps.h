#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

class PresentationFps
{
public:
    using Clock = std::chrono::steady_clock;

    explicit PresentationFps(Clock::time_point now) : m_start(now) {}

    bool update(Clock::time_point now, bool newFrame)
    {
        if (newFrame)
            ++m_frames;
        const auto elapsed = now - m_start;
        if (elapsed < std::chrono::seconds(1))
            return false;
        m_fps = static_cast<double>(m_frames) / std::chrono::duration<double>(elapsed).count();
        m_frames = 0u;
        m_start = now;
        m_ready = true;
        return true;
    }

    double fps() const { return m_fps; }

    std::string title(const std::string &name) const
    {
        std::ostringstream text;
        text.imbue(std::locale::classic());
        text << name << " | ";
        if (m_ready)
            text << std::fixed << std::setprecision(1) << m_fps;
        else
            text << "--";
        text << " FPS";
        return text.str();
    }

private:
    Clock::time_point m_start;
    uint64_t m_frames = 0u;
    double m_fps = 0.0;
    bool m_ready = false;
};
