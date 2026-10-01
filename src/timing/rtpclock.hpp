// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "util/strings.hpp"

namespace mxlgw::timing
{
    using util::Rational;

    /// All times are TAI nanoseconds since the SMPTE ST 2059-1 epoch (§5).
    using TaiNs = std::int64_t;

    inline constexpr std::int64_t videoClockHz = 90'000;

    /// Media-clock ticks at TAI time `t` (floor), 128-bit intermediate.
    std::int64_t ticksAt(TaiNs t, std::int64_t clockHz);

    /// TAI time of a tick count (rounded to nearest ns).
    TaiNs taiOfTicks(std::int64_t ticks, std::int64_t clockHz);

    /// RTP timestamp (mod 2^32) of TAI time `t`.
    std::uint32_t rtpAt(TaiNs t, std::int64_t clockHz);

    /// Unwraps a 32-bit RTP timestamp to the 64-bit tick value closest to `refTaiNs` and returns its TAI time.
    TaiNs unwrapRtp(std::uint32_t rtp32, std::int64_t clockHz, TaiNs refTaiNs);

    /// Same as unwrapRtp but returns ticks (samples for audio).
    std::int64_t unwrapRtpTicks(std::uint32_t rtp32, std::int64_t clockHz, TaiNs refTaiNs);

    /// MXL index math (identical formulas to MXL v1.1.0 IndexConversion.hpp: round to nearest).
    std::uint64_t timestampToIndex(Rational rate, TaiNs t);
    TaiNs indexToTimestamp(Rational rate, std::uint64_t index);

    /// Egress (owner decision Q1): transmit time and RTP timestamp of grain `index`.
    struct EgressTiming
    {
        TaiNs origin;      // T(i)
        TaiNs transmit;    // T(i) + output_delay
        std::uint32_t rtp; // ticks of `transmit` mod 2^32
    };
    EgressTiming egressTiming(Rational rate, std::uint64_t index, std::int64_t outputDelayNs, std::int64_t clockHz);
}
