// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>

namespace mxlgw::util
{
    /// Exponential backoff with jitter (§5.8: 500 ms doubling to 5 s, ±10 %).
    class Backoff
    {
    public:
        using Duration = std::chrono::nanoseconds;

        Backoff(Duration initial, Duration maximum, double jitter = 0.1, std::uint64_t seed = 0);

        /// Delay before the next attempt; grows until `maximum`.
        Duration next();
        void reset();
        std::uint32_t attempts() const { return _attempts; }

        /// Delay without jitter for attempt n (0-based), for tests.
        Duration nominal(std::uint32_t attempt) const;

    private:
        Duration _initial;
        Duration _maximum;
        double _jitter;
        std::uint32_t _attempts = 0;
        std::uint64_t _state;
    };
}
