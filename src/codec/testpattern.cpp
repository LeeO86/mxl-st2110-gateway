// SPDX-License-Identifier: MIT
#include "codec/testpattern.hpp"

#include <algorithm>
#include <cmath>

namespace mxlgw::codec
{
    float toneSample(std::int64_t sampleIndex, int channel, double hz, float level, int sampleRate)
    {
        // Non-harmonic per-channel frequencies: an offset that matches one channel does not match the others.
        auto const k = channel % 4;
        auto const f = hz * (1.0 + 0.5 * k) + 7.0 * k;
        auto const period = static_cast<double>(sampleRate) / f;
        auto const whole = std::llround(period);
        double phase = 0.0;
        // Reduce the index modulo the period first so the phase stays exact for 2026 epoch indices.
        if (whole > 0 && std::fabs(period - static_cast<double>(whole)) < 1e-9)
        {
            phase = static_cast<double>(((sampleIndex % whole) + whole) % whole) / static_cast<double>(whole);
        }
        else
        {
            phase = std::fmod(static_cast<double>(sampleIndex), period) / period;
        }
        return level * static_cast<float>(std::sin(2.0 * M_PI * phase));
    }

    AncPacket timecodePacket(std::uint32_t counter, int framesPerSecond, std::uint16_t line)
    {
        AncPacket p;
        p.line = line;
        p.did = 0x60;
        p.sdid = 0x60;
        auto const fps = framesPerSecond > 30 ? framesPerSecond / 2 : std::max(1, framesPerSecond);
        auto const tcFrames = framesPerSecond > 30 ? counter / 2 : counter;
        auto const ff = tcFrames % static_cast<std::uint32_t>(fps);
        auto const totalSeconds = tcFrames / static_cast<std::uint32_t>(fps);
        auto const ss = totalSeconds % 60;
        auto const mm = (totalSeconds / 60) % 60;
        auto const hh = (totalSeconds / 3600) % 24;
        std::uint8_t const digits[8] = {
            static_cast<std::uint8_t>(ff % 10), static_cast<std::uint8_t>(ff / 10), static_cast<std::uint8_t>(ss % 10), static_cast<std::uint8_t>(ss / 10),
            static_cast<std::uint8_t>(mm % 10), static_cast<std::uint8_t>(mm / 10), static_cast<std::uint8_t>(hh % 10), static_cast<std::uint8_t>(hh / 10),
        };
        p.udw.resize(16);
        for (int i = 0; i < 8; ++i)
        {
            // RP 188: UDW 2i carries a time-address nibble in b4-b7, UDW 2i+1 a binary-group nibble.
            p.udw[static_cast<std::size_t>(2 * i)] = static_cast<std::uint8_t>(digits[i] << 4);
            auto const nibble = static_cast<std::uint8_t>((counter >> (4 * i)) & 0xF);
            p.udw[static_cast<std::size_t>(2 * i + 1)] = static_cast<std::uint8_t>(nibble << 4);
        }
        return p;
    }

    std::optional<std::uint32_t> timecodeCounter(AncPacket const& packet)
    {
        if (packet.did != 0x60 || packet.sdid != 0x60 || packet.udw.size() < 16)
        {
            return std::nullopt;
        }
        std::uint32_t counter = 0;
        for (int i = 0; i < 8; ++i)
        {
            auto const nibble = static_cast<std::uint32_t>((packet.udw[static_cast<std::size_t>(2 * i + 1)] >> 4) & 0xF);
            counter |= nibble << (4 * i);
        }
        return counter;
    }

    std::optional<std::uint32_t> timecodeCounter(AncFrame const& frame)
    {
        for (auto const& p : frame.packets)
        {
            if (auto const c = timecodeCounter(p))
            {
                return c;
            }
        }
        return std::nullopt;
    }
}
