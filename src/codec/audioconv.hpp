// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace mxlgw::codec
{
    /// Layout of de-interleaved float32 channel buffers in an MXL continuous flow
    /// (mirrors mxlMutableWrappedMultiBufferSlice: two fragments for ring wrap-around,
    /// `stride` bytes between the same position of consecutive channels).
    struct ChannelSlices
    {
        struct Fragment
        {
            void* pointer = nullptr;
            std::size_t size = 0; // bytes
        };
        Fragment fragments[2];
        std::size_t stride = 0;
        std::size_t count = 0; // channels

        std::size_t samples() const { return (fragments[0].size + fragments[1].size) / sizeof(float); }
    };

    struct ConstChannelSlices
    {
        struct Fragment
        {
            void const* pointer = nullptr;
            std::size_t size = 0;
        };
        Fragment fragments[2];
        std::size_t stride = 0;
        std::size_t count = 0;

        std::size_t samples() const { return (fragments[0].size + fragments[1].size) / sizeof(float); }
    };

    /// L16/L24 big-endian interleaved -> float32 per channel, x / 2^(bits-1), no clamping (§6.2).
    /// `channels` interleaved channels are written to the first `channels` buffers of `dst`.
    void pcmToFloat(std::uint8_t const* pcm, std::size_t samples, int channels, int bitDepth, ChannelSlices const& dst);

    /// float32 per channel -> L16/L24 big-endian interleaved, round-to-nearest and clamped to [-1.0, +1.0).
    void floatToPcm(ConstChannelSlices const& src, std::size_t samples, int channels, int bitDepth, std::uint8_t* pcm);

    /// Single-sample helpers (exposed for tests).
    float decodeSample(std::uint8_t const* p, int bitDepth);
    void encodeSample(float value, std::uint8_t* p, int bitDepth);

    /// Writes silence (all zero) for `samples` samples at `channels` channels, `bitDepth` bits.
    void pcmSilence(std::uint8_t* pcm, std::size_t samples, int channels, int bitDepth);
}
