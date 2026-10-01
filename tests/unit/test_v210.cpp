// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <vector>

#include "codec/v210.hpp"
#include "group/replacement.hpp"

using namespace mxlgw;
using namespace mxlgw::codec;

TEST_CASE("v210 stride matches MXL")
{
    CHECK(v210Stride(1920) == 5120);
    CHECK(v210Stride(3840) == 10240);
    CHECK(v210Stride(1280) == 3456);
}

TEST_CASE("fill black and read back")
{
    int const w = 1920;
    int const lines = 4;
    std::vector<std::uint8_t> buf(v210Stride(w) * lines, 0xAA);
    v210FillBlack(buf.data(), w, lines, v210Stride(w));
    for (int x : {0, 1, 2, 3, 4, 5, 6, 1919})
    {
        auto const p = v210Pixel(buf.data(), v210Stride(w), x, 3);
        CHECK(p.y == 64);
        CHECK(p.cb == 512);
        CHECK(p.cr == 512);
    }
}

TEST_CASE("colour bars with frame counter")
{
    int const w = 1920;
    int const lines = 64;
    std::vector<std::uint8_t> buf(v210Stride(w) * lines);
    for (std::uint32_t counter : {0u, 1u, 0xDEADBEEFu, 0xFFFFFFFFu})
    {
        v210ColourBars(buf.data(), w, lines, v210Stride(w), counter);
        CHECK(v210ReadCounter(buf.data(), w, v210Stride(w)) == counter);
        CHECK(v210CheckBars(buf.data(), w, lines, v210Stride(w)));
    }
    auto const p = v210Pixel(buf.data(), v210Stride(w), 1919, 40);
    CHECK(p.y == colourBar(7).y);
    // A black frame has no counter pattern problem but no bars.
    v210FillBlack(buf.data(), w, lines, v210Stride(w));
    CHECK(v210ReadCounter(buf.data(), w, v210Stride(w)) == 0u);
    CHECK_FALSE(v210CheckBars(buf.data(), w, lines, v210Stride(w)));
    // Grey is neither black nor white: no counter.
    v210Fill(buf.data(), w, lines, v210Stride(w), {500, 512, 512});
    CHECK_FALSE(v210ReadCounter(buf.data(), w, v210Stride(w)));
}

TEST_CASE("replacement frames: black and repeat (§5.7)")
{
    config::VideoFormat f;
    group::VideoReplacement black(f, config::MissingData::Black);
    CHECK(v210Pixel(black.frame(), f.v210Stride(), 100, 100).y == 64);
    std::vector<std::uint8_t> grain(f.grainBytes());
    v210ColourBars(grain.data(), f.width, f.linesPerGrain(), f.v210Stride(), 77);
    black.remember(grain.data());
    CHECK(v210Pixel(black.frame(), f.v210Stride(), 100, 100).y == 64);

    group::VideoReplacement repeat(f, config::MissingData::Repeat);
    CHECK_FALSE(repeat.repeating());
    CHECK(repeat.frame() == repeat.black());
    repeat.remember(grain.data());
    CHECK(repeat.repeating());
    CHECK(v210ReadCounter(repeat.frame(), f.width, f.v210Stride()) == 77u);
    CHECK(group::emptyAnc().packets.empty());
}
