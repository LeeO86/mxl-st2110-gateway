// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cmath>

#include "codec/anc8331.hpp"
#include "codec/testpattern.hpp"

using namespace mxlgw;

TEST_CASE("testpattern: tone is locked to the absolute sample index")
{
    std::int64_t const s = 86'000'000'000'000LL; // a 2026 sample index at 48 kHz
    CHECK(codec::toneSample(s, 0, 1000.0, 0.5f, 48000) == doctest::Approx(codec::toneSample(s + 48, 0, 1000.0, 0.5f, 48000)));
    CHECK(codec::toneSample(0, 0, 1000.0, 0.5f, 48000) == doctest::Approx(0.0));
    CHECK(codec::toneSample(12, 0, 1000.0, 0.5f, 48000) == doctest::Approx(0.5)); // quarter period
    // 997 Hz is not periodic within one 48-sample block: alignment is unambiguous.
    CHECK(std::fabs(codec::toneSample(s, 0, 997.0, 0.5f, 48000) - codec::toneSample(s + 48, 0, 997.0, 0.5f, 48000)) > 1e-3);
    float peak = 0.0f;
    for (int i = 0; i < 4800; ++i)
    {
        peak = std::max(peak, std::fabs(codec::toneSample(s + i, 3, 997.0, 0.25f, 48000)));
    }
    CHECK(peak == doctest::Approx(0.25).epsilon(0.01));
}

TEST_CASE("testpattern: SMPTE 12M time code carries the counter in the binary groups")
{
    auto const p = codec::timecodePacket(0xDEADBEEF, 25);
    CHECK(p.did == 0x60);
    CHECK(p.sdid == 0x60);
    CHECK(p.udw.size() == 16);
    CHECK(codec::timecodeCounter(p) == 0xDEADBEEFu);

    // 01:02:03:04 at 25 fps
    auto const counter = static_cast<std::uint32_t>(((1 * 60 + 2) * 60 + 3) * 25 + 4);
    auto const tc = codec::timecodePacket(counter, 25);
    CHECK((tc.udw[0] >> 4) == 4);  // frame units
    CHECK((tc.udw[2] >> 4) == 0);  // frame tens
    CHECK((tc.udw[4] >> 4) == 3);  // second units
    CHECK((tc.udw[8] >> 4) == 2);  // minute units
    CHECK((tc.udw[12] >> 4) == 1); // hour units

    // Survives the RFC 8331 grain round trip.
    codec::AncFrame frame;
    frame.packets.push_back(codec::timecodePacket(42, 50));
    auto const body = codec::serialiseGrain(frame);
    REQUIRE(body);
    auto const parsed = codec::parseGrain(body->data(), body->size());
    REQUIRE(parsed.frame);
    CHECK(codec::timecodeCounter(*parsed.frame) == 42u);

    codec::AncPacket other;
    other.did = 0x41;
    other.sdid = 0x05;
    CHECK_FALSE(codec::timecodeCounter(other));
    CHECK_FALSE(codec::timecodeCounter(codec::AncFrame{}));
}
