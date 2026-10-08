// SPDX-License-Identifier: MIT
#include "timing/rtpclock.hpp"

#include <algorithm>

namespace mxlgw::timing
{
    namespace
    {
        constexpr __int128 nsPerSecond = 1'000'000'000;
        constexpr std::int64_t wrap = std::int64_t{1} << 32;

        __int128 floorDiv(__int128 a, __int128 b)
        {
            __int128 q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0)))
            {
                --q;
            }
            return q;
        }
    }

    std::int64_t ticksAt(TaiNs t, std::int64_t clockHz)
    {
        return static_cast<std::int64_t>(floorDiv(static_cast<__int128>(t) * clockHz, nsPerSecond));
    }

    TaiNs taiOfTicks(std::int64_t ticks, std::int64_t clockHz)
    {
        __int128 const num = static_cast<__int128>(ticks) * nsPerSecond;
        return static_cast<TaiNs>(floorDiv(num + clockHz / 2, clockHz));
    }

    std::uint32_t rtpAt(TaiNs t, std::int64_t clockHz)
    {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(ticksAt(t, clockHz)) & 0xFFFFFFFFu);
    }

    std::int64_t unwrapRtpTicks(std::uint32_t rtp32, std::int64_t clockHz, TaiNs refTaiNs)
    {
        std::int64_t const refTicks = ticksAt(refTaiNs, clockHz);
        std::int64_t const base = refTicks - (refTicks & (wrap - 1)); // floor to multiple of 2^32 (refTicks >= 0)
        std::int64_t candidate = base + static_cast<std::int64_t>(rtp32);
        // Choose the candidate congruent to rtp32 closest to refTicks.
        if (candidate - refTicks > wrap / 2)
        {
            candidate -= wrap;
        }
        else if (refTicks - candidate > wrap / 2)
        {
            candidate += wrap;
        }
        return candidate;
    }

    TaiNs unwrapRtp(std::uint32_t rtp32, std::int64_t clockHz, TaiNs refTaiNs)
    {
        return taiOfTicks(unwrapRtpTicks(rtp32, clockHz, refTaiNs), clockHz);
    }

    std::uint64_t timestampToIndex(Rational rate, TaiNs t)
    {
        // MXL: (timestamp * num + 500'000'000 * den) / (1'000'000'000 * den)
        __int128 const num = static_cast<__int128>(t) * rate.num + static_cast<__int128>(500'000'000) * rate.den;
        return static_cast<std::uint64_t>(num / (nsPerSecond * rate.den));
    }

    TaiNs indexToTimestamp(Rational rate, std::uint64_t index)
    {
        // MXL: (index * den * 1'000'000'000 + num / 2) / num
        __int128 const num = static_cast<__int128>(index) * rate.den * nsPerSecond + rate.num / 2;
        return static_cast<TaiNs>(num / rate.num);
    }

    EgressTiming egressTiming(Rational rate, std::uint64_t index, std::int64_t outputDelayNs, std::int64_t clockHz)
    {
        EgressTiming t{};
        t.origin = indexToTimestamp(rate, index);
        t.transmit = t.origin + outputDelayNs;
        t.rtp = rtpAt(t.transmit, clockHz);
        return t;
    }

    AudioBlockSchedule::Times AudioBlockSchedule::times(std::int64_t block) const
    {
        Times t{};
        t.start = taiOfTicks(block * samplesPerBlock, sampleRate);
        t.due = taiOfTicks((block + 1) * samplesPerBlock, sampleRate) + readOffsetNs;
        t.transmit = t.start + outputDelayNs;
        t.giveUp = std::max(t.due, t.transmit - marginNs);
        return t;
    }

    std::int64_t AudioBlockSchedule::lastDue(TaiNs now) const
    {
        return static_cast<std::int64_t>(floorDiv(ticksAt(now - readOffsetNs, sampleRate), samplesPerBlock)) - 1;
    }

    std::int64_t AudioBlockSchedule::firstUnsent(TaiNs now) const
    {
        return static_cast<std::int64_t>(floorDiv(ticksAt(now - outputDelayNs, sampleRate), samplesPerBlock)) + 1;
    }
}
