// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "codec/audioconv.hpp"

using namespace mxlgw::codec;

namespace
{
    struct Ring
    {
        // `length` samples per channel, `channels` channels, contiguous (stride = length * 4).
        Ring(std::size_t length, std::size_t channels)
            : data(length * channels)
            , length(length)
            , channels(channels)
        {}
        std::vector<float> data;
        std::size_t length;
        std::size_t channels;

        ChannelSlices slices(std::size_t start, std::size_t count)
        {
            ChannelSlices s;
            auto const first = std::min(count, length - start);
            s.fragments[0] = {data.data() + start, first * sizeof(float)};
            s.fragments[1] = {data.data(), (count - first) * sizeof(float)};
            s.stride = length * sizeof(float);
            s.count = channels;
            return s;
        }
        ConstChannelSlices constSlices(std::size_t start, std::size_t count)
        {
            auto const m = slices(start, count);
            ConstChannelSlices s;
            s.fragments[0] = {m.fragments[0].pointer, m.fragments[0].size};
            s.fragments[1] = {m.fragments[1].pointer, m.fragments[1].size};
            s.stride = m.stride;
            s.count = m.count;
            return s;
        }
        float& at(std::size_t ch, std::size_t i) { return data[ch * length + (i % length)]; }
    };
}

TEST_CASE("single samples: full scale, zero, clamping, rounding")
{
    std::uint8_t b[3];
    for (int depth : {16, 24})
    {
        encodeSample(0.0f, b, depth);
        CHECK(decodeSample(b, depth) == 0.0f);
        encodeSample(1.0f, b, depth); // +1.0 is outside [-1, +1): clamps to max
        CHECK(decodeSample(b, depth) < 1.0f);
        CHECK(decodeSample(b, depth) > 0.9998f);
        encodeSample(5.0f, b, depth);
        CHECK(decodeSample(b, depth) < 1.0f);
        encodeSample(-1.0f, b, depth);
        CHECK(decodeSample(b, depth) == -1.0f);
        encodeSample(-7.0f, b, depth);
        CHECK(decodeSample(b, depth) == -1.0f);
        encodeSample(std::nanf(""), b, depth);
        CHECK(decodeSample(b, depth) == 0.0f);
    }
    encodeSample(0.5f, b, 24);
    CHECK(b[0] == 0x40);
    CHECK(b[1] == 0x00);
    CHECK(b[2] == 0x00);
    encodeSample(1.4f / 8388608.0f, b, 24); // rounds to 1
    CHECK(b[2] == 0x01);
    encodeSample(1.6f / 8388608.0f, b, 24); // rounds to 2
    CHECK(b[2] == 0x02);
    b[0] = 0x80;
    b[1] = 0x00;
    CHECK(decodeSample(b, 16) == -1.0f);
}

TEST_CASE("interleaved <-> per-channel for 1/2/8/16/64 channels with wrap")
{
    for (int depth : {16, 24})
    {
        for (int channels : {1, 2, 8, 16, 64})
        {
            std::size_t const samples = 48;
            int const bytes = depth / 8;
            std::vector<std::uint8_t> pcm(samples * static_cast<std::size_t>(channels) * static_cast<std::size_t>(bytes));
            for (std::size_t s = 0; s < samples; ++s)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    float const v = std::sin(0.1f * static_cast<float>(s) + static_cast<float>(ch)) * 0.9f;
                    encodeSample(v, pcm.data() + (s * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)) * static_cast<std::size_t>(bytes),
                                 depth);
                }
            }
            Ring ring(100, static_cast<std::size_t>(channels));
            std::size_t const start = 80; // wraps after 20 samples
            pcmToFloat(pcm.data(), samples, channels, depth, ring.slices(start, samples));
            for (int ch = 0; ch < channels; ++ch)
            {
                for (std::size_t s = 0; s < samples; s += 7)
                {
                    auto const* p = pcm.data() + (s * static_cast<std::size_t>(channels) + static_cast<std::size_t>(ch)) * static_cast<std::size_t>(bytes);
                    CHECK(ring.at(static_cast<std::size_t>(ch), start + s) == decodeSample(p, depth));
                }
            }
            std::vector<std::uint8_t> back(pcm.size());
            floatToPcm(ring.constSlices(start, samples), samples, channels, depth, back.data());
            CHECK(back == pcm);
        }
    }
}

TEST_CASE("fewer flow channels than wire channels: silence")
{
    Ring ring(10, 1);
    for (std::size_t i = 0; i < 10; ++i)
    {
        ring.at(0, i) = 0.25f;
    }
    std::vector<std::uint8_t> pcm(4 * 2 * 3, 0xFF);
    floatToPcm(ring.constSlices(0, 4), 4, 2, 24, pcm.data());
    CHECK(decodeSample(pcm.data(), 24) == 0.25f);
    CHECK(decodeSample(pcm.data() + 3, 24) == 0.0f);
    pcmSilence(pcm.data(), 4, 2, 24);
    CHECK(decodeSample(pcm.data(), 24) == 0.0f);
    CHECK(ring.slices(0, 4).samples() == 4);
    CHECK(ring.constSlices(8, 4).samples() == 4);
}
