// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "config/formats.hpp"
#include "timing/rtpclock.hpp"

using namespace mxlgw;
using namespace mxlgw::timing;

namespace
{
    // 2026-10-01T00:00:00 TAI (TAI = UTC + 37 s), seconds since 1970-01-01 TAI.
    constexpr TaiNs epoch2026 = (1790812800LL + 37) * 1'000'000'000LL;
}

TEST_CASE("index <-> timestamp matches MXL rounding")
{
    for (auto const rate : {Rational{25, 1}, Rational{50, 1}, Rational{30000, 1001}, Rational{60000, 1001}, Rational{48000, 1}})
    {
        auto const i = timestampToIndex(rate, epoch2026);
        auto const t = indexToTimestamp(rate, i);
        CHECK(timestampToIndex(rate, t) == i);
        CHECK(std::llabs(t - epoch2026) <= config::periodNs(rate) / 2 + 1);
        // Rounding to nearest: half a period later still maps to the next index.
        CHECK(timestampToIndex(rate, t + config::periodNs(rate) / 2 + 1) == i + 1);
        CHECK(timestampToIndex(rate, t + config::periodNs(rate) / 2 - 1) == i);
    }
    CHECK(indexToTimestamp({50, 1}, 0) == 0);
    CHECK(timestampToIndex({50, 1}, 20'000'000) == 1);
    CHECK(indexToTimestamp({30000, 1001}, 3) == 100'100'000);
}

TEST_CASE("interlaced 25/1 uses the field rate")
{
    config::VideoFormat f;
    f.rate = {25, 1};
    f.interlace = config::Interlace::InterlacedTff;
    auto const rate = f.grainRate();
    CHECK(rate == Rational{50, 1});
    auto const i = timestampToIndex(rate, epoch2026);
    CHECK(indexToTimestamp(rate, i + 1) - indexToTimestamp(rate, i) == 20'000'000);
}

TEST_CASE("RTP ticks, wrap and unwrap")
{
    CHECK(ticksAt(1'000'000'000, videoClockHz) == 90'000);
    CHECK(ticksAt(-1, videoClockHz) == -1);
    CHECK(taiOfTicks(90'000, videoClockHz) == 1'000'000'000);
    CHECK(rtpAt(epoch2026, 48000) == static_cast<std::uint32_t>(ticksAt(epoch2026, 48000) & 0xFFFFFFFFu));

    auto const ref = epoch2026;
    auto const ticks = ticksAt(ref, videoClockHz);
    auto const rtp = static_cast<std::uint32_t>(ticks & 0xFFFFFFFF);
    CHECK(unwrapRtpTicks(rtp, videoClockHz, ref) == ticks);

    // Reference ahead or behind by up to ±0.5 wrap.
    constexpr std::int64_t wrap = std::int64_t{1} << 32;
    auto const wrapNs = taiOfTicks(wrap, videoClockHz);
    CHECK(unwrapRtpTicks(rtp, videoClockHz, ref + wrapNs / 2 - 1'000'000) == ticks);
    CHECK(unwrapRtpTicks(rtp, videoClockHz, ref - wrapNs / 2 + 1'000'000) == ticks);
    CHECK(unwrapRtpTicks(rtp, videoClockHz, ref + 5'000'000) == ticks);
    CHECK(unwrapRtpTicks(rtp, videoClockHz, ref - 5'000'000) == ticks);

    // Just before and after a 2^32 wrap.
    std::int64_t const boundary = (ticks / wrap + 1) * wrap;
    for (std::int64_t delta : {-2, -1, 0, 1, 2})
    {
        auto const t = boundary + delta * 1800;
        auto const r32 = static_cast<std::uint32_t>(t & 0xFFFFFFFF);
        auto const refNs = taiOfTicks(boundary, videoClockHz);
        CHECK(unwrapRtpTicks(r32, videoClockHz, refNs) == t);
        CHECK(unwrapRtpTicks(r32, videoClockHz, refNs + 3'000'000) == t);
        CHECK(unwrapRtpTicks(r32, videoClockHz, refNs - 3'000'000) == t);
    }
}

TEST_CASE("round trip index -> rtp -> index for 10^6 consecutive indices")
{
    for (auto const rate : {Rational{50, 1}, Rational{60000, 1001}})
    {
        auto const first = timestampToIndex(rate, epoch2026);
        bool ok = true;
        for (std::uint64_t i = first; i < first + 1'000'000 && ok; ++i)
        {
            auto const t = indexToTimestamp(rate, i);
            auto const rtp = rtpAt(t, videoClockHz);
            auto const back = unwrapRtp(rtp, videoClockHz, t + 2'000'000); // receive ~2 ms later
            ok = timestampToIndex(rate, back) == i;
        }
        CHECK(ok);
    }
}

TEST_CASE("48 kHz audio samples")
{
    auto const s = timestampToIndex({48000, 1}, epoch2026);
    auto const rtp = rtpAt(indexToTimestamp({48000, 1}, s), 48000);
    CHECK(unwrapRtpTicks(rtp, 48000, epoch2026 + 1'000'000) == static_cast<std::int64_t>(s));
}

TEST_CASE("egress transmit-time RTP (owner decision Q1)")
{
    Rational const rate{50, 1};
    auto const i = timestampToIndex(rate, epoch2026);
    auto const e = egressTiming(rate, i, 40'000'000, videoClockHz);
    CHECK(e.origin == indexToTimestamp(rate, i));
    CHECK(e.transmit == e.origin + 40'000'000);
    CHECK(e.rtp == rtpAt(e.transmit, videoClockHz));
    // Two essences of a group share the delay, so their RTP times stay aligned.
    auto const audio = rtpAt(e.transmit, 48000);
    CHECK(unwrapRtp(audio, 48000, e.transmit) - unwrapRtp(e.rtp, videoClockHz, e.transmit) < 25'000);
    // Ingest of the egress stream lands two grains later.
    CHECK(timestampToIndex(rate, unwrapRtp(e.rtp, videoClockHz, e.transmit)) == i + 2);
}
