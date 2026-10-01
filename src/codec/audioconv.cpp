// SPDX-License-Identifier: MIT
#include "codec/audioconv.hpp"

#include <cmath>
#include <cstring>

namespace mxlgw::codec
{
    namespace
    {
        constexpr float scale16 = 32768.0f;
        constexpr float scale24 = 8388608.0f;

        inline std::int32_t readBe(std::uint8_t const* p, int bytes)
        {
            if (bytes == 3)
            {
                std::int32_t v = (static_cast<std::int32_t>(p[0]) << 16) | (static_cast<std::int32_t>(p[1]) << 8) | p[2];
                return (v ^ 0x800000) - 0x800000; // sign-extend 24 bit
            }
            return static_cast<std::int16_t>((p[0] << 8) | p[1]);
        }

        inline void writeBe(std::int32_t v, std::uint8_t* p, int bytes)
        {
            if (bytes == 3)
            {
                p[0] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
                p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                p[2] = static_cast<std::uint8_t>(v & 0xFF);
                return;
            }
            p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            p[1] = static_cast<std::uint8_t>(v & 0xFF);
        }

        inline std::int32_t quantise(float value, int bitDepth)
        {
            float const scale = bitDepth == 24 ? scale24 : scale16;
            std::int32_t const maxValue = bitDepth == 24 ? 0x7FFFFF : 0x7FFF;
            std::int32_t const minValue = -maxValue - 1;
            if (!(value == value)) // NaN
            {
                return 0;
            }
            float const scaled = std::nearbyint(value * scale);
            if (scaled >= static_cast<float>(maxValue))
            {
                return maxValue;
            }
            if (scaled <= static_cast<float>(minValue))
            {
                return minValue;
            }
            return static_cast<std::int32_t>(scaled);
        }
    }

    float decodeSample(std::uint8_t const* p, int bitDepth)
    {
        int const bytes = bitDepth / 8;
        return static_cast<float>(readBe(p, bytes)) / (bitDepth == 24 ? scale24 : scale16);
    }

    void encodeSample(float value, std::uint8_t* p, int bitDepth)
    {
        writeBe(quantise(value, bitDepth), p, bitDepth / 8);
    }

    void pcmToFloat(std::uint8_t const* pcm, std::size_t samples, int channels, int bitDepth, ChannelSlices const& dst)
    {
        int const bytes = bitDepth / 8;
        std::size_t const frameBytes = static_cast<std::size_t>(channels) * static_cast<std::size_t>(bytes);
        float const inv = 1.0f / (bitDepth == 24 ? scale24 : scale16);
        std::size_t const usedChannels = static_cast<std::size_t>(channels) < dst.count ? static_cast<std::size_t>(channels) : dst.count;
        std::size_t sampleOffset = 0;
        for (auto const& fragment : dst.fragments)
        {
            std::size_t const n = fragment.size / sizeof(float);
            std::size_t const take = n < samples - sampleOffset ? n : samples - sampleOffset;
            if (take == 0 || fragment.pointer == nullptr)
            {
                continue;
            }
            for (std::size_t ch = 0; ch < usedChannels; ++ch)
            {
                auto* out = reinterpret_cast<float*>(static_cast<std::uint8_t*>(fragment.pointer) + ch * dst.stride);
                std::uint8_t const* in = pcm + sampleOffset * frameBytes + ch * static_cast<std::size_t>(bytes);
                if (bytes == 3)
                {
                    for (std::size_t s = 0; s < take; ++s, in += frameBytes)
                    {
                        std::int32_t v = (static_cast<std::int32_t>(in[0]) << 16) | (static_cast<std::int32_t>(in[1]) << 8) | in[2];
                        v = (v ^ 0x800000) - 0x800000;
                        out[s] = static_cast<float>(v) * inv;
                    }
                }
                else
                {
                    for (std::size_t s = 0; s < take; ++s, in += frameBytes)
                    {
                        out[s] = static_cast<float>(static_cast<std::int16_t>((in[0] << 8) | in[1])) * inv;
                    }
                }
            }
            sampleOffset += take;
        }
    }

    void floatToPcm(ConstChannelSlices const& src, std::size_t samples, int channels, int bitDepth, std::uint8_t* pcm)
    {
        int const bytes = bitDepth / 8;
        std::size_t const frameBytes = static_cast<std::size_t>(channels) * static_cast<std::size_t>(bytes);
        std::size_t const usedChannels = static_cast<std::size_t>(channels) < src.count ? static_cast<std::size_t>(channels) : src.count;
        if (usedChannels < static_cast<std::size_t>(channels))
        {
            std::memset(pcm, 0, samples * frameBytes);
        }
        std::size_t sampleOffset = 0;
        for (auto const& fragment : src.fragments)
        {
            std::size_t const n = fragment.size / sizeof(float);
            std::size_t const take = n < samples - sampleOffset ? n : samples - sampleOffset;
            if (take == 0 || fragment.pointer == nullptr)
            {
                continue;
            }
            for (std::size_t ch = 0; ch < usedChannels; ++ch)
            {
                auto const* in = reinterpret_cast<float const*>(static_cast<std::uint8_t const*>(fragment.pointer) + ch * src.stride);
                std::uint8_t* out = pcm + sampleOffset * frameBytes + ch * static_cast<std::size_t>(bytes);
                for (std::size_t s = 0; s < take; ++s, out += frameBytes)
                {
                    writeBe(quantise(in[s], bitDepth), out, bytes);
                }
            }
            sampleOffset += take;
        }
    }

    void pcmSilence(std::uint8_t* pcm, std::size_t samples, int channels, int bitDepth)
    {
        std::memset(pcm, 0, samples * static_cast<std::size_t>(channels) * static_cast<std::size_t>(bitDepth / 8));
    }
}
