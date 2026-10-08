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

    /// Egress audio schedule (§5.7). Block `k` holds the samples [k·n, (k+1)·n).
    struct AudioBlockSchedule
    {
        std::int64_t samplesPerBlock = 0;
        std::int64_t sampleRate = 48'000;
        std::int64_t readOffsetNs = 0;
        std::int64_t outputDelayNs = 0;
        /// How long before its transmit time a block must reach MTL at the latest.
        std::int64_t marginNs = 0;

        struct Times
        {
            TaiNs start;    // T(k), TAI of the first sample
            TaiNs due;      // the block's end plus the read offset: its data is read from here on
            TaiNs giveUp;   // transmit - margin (not before due): no data by then sends silence
            TaiNs transmit; // T(k) + output delay
        };
        Times times(std::int64_t block) const;
        /// The last block whose data is due at `now` (where a worker starting at `now` begins).
        std::int64_t lastDue(TaiNs now) const;
        /// The first block whose transmit time is after `now` (earlier blocks can no longer be sent).
        std::int64_t firstUnsent(TaiNs now) const;
    };
}
