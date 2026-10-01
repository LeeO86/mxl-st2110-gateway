// SPDX-License-Identifier: MIT
#include "util/backoff.hpp"

#include <algorithm>

namespace mxlgw::util
{
    Backoff::Backoff(Duration initial, Duration maximum, double jitter, std::uint64_t seed)
        : _initial(initial)
        , _maximum(maximum)
        , _jitter(jitter)
        , _state(seed != 0 ? seed : static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u)
    {}

    Backoff::Duration Backoff::nominal(std::uint32_t attempt) const
    {
        auto delay = _initial;
        for (std::uint32_t i = 0; i < attempt && delay < _maximum; ++i)
        {
            delay *= 2;
        }
        return std::min(delay, _maximum);
    }

    Backoff::Duration Backoff::next()
    {
        auto const base = nominal(_attempts);
        if (_attempts < 64)
        {
            ++_attempts;
        }
        // xorshift64* for jitter; deterministic per seed.
        _state ^= _state >> 12;
        _state ^= _state << 25;
        _state ^= _state >> 27;
        auto const r = static_cast<double>((_state * 2685821657736338717ULL) >> 11) / static_cast<double>(1ULL << 53); // [0,1)
        auto const factor = 1.0 + _jitter * (2.0 * r - 1.0);
        return Duration{static_cast<Duration::rep>(static_cast<double>(base.count()) * factor)};
    }

    void Backoff::reset()
    {
        _attempts = 0;
    }
}
