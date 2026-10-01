// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "util/strings.hpp"

namespace mxlgw::config
{
    using util::Rational;

    enum class Interlace
    {
        Progressive,
        InterlacedTff,
        InterlacedBff,
    };

    char const* toName(Interlace i);
    std::optional<Interlace> parseInterlace(std::string const& text);

    /// Rates of §6.1 (as written in the configuration).
    std::vector<Rational> const& supportedVideoRates();
    bool isSupportedVideoRate(Rational rate);
    bool interlaceAllowed(Rational rate);

    /// Duration of one period of `rate` in ns (rounded to nearest).
    std::int64_t periodNs(Rational rate);

    struct VideoFormat
    {
        int width = 1920;
        int height = 1080;
        Rational rate{50, 1};
        Interlace interlace = Interlace::Progressive;
        std::string colorimetry = "BT709";
        std::string tcs = "SDR";

        bool interlaced() const { return interlace != Interlace::Progressive; }
        /// MXL index rate: the field rate for interlaced flows (MXL doubles grain_rate, FlowParser.cpp).
        Rational grainRate() const;
        std::int64_t grainDurationNs() const { return periodNs(grainRate()); }
        int linesPerGrain() const { return interlaced() ? height / 2 : height; }
        /// MXL v210 line length ((width + 47) / 48) * 128.
        std::size_t v210Stride() const;
        std::size_t grainBytes() const { return v210Stride() * static_cast<std::size_t>(linesPerGrain()); }
        /// Fixed-order description used for the Flow id (§7.3).
        std::string canonical() const;
        bool operator==(VideoFormat const& o) const;
    };

    struct AudioFormat
    {
        int channels = 2;
        int bitDepth = 24;
        int sampleRate = 48000;
        int ptimeUs = 1000;
        int blockUs = 1000;

        int samplesPerPacket() const { return static_cast<int>(static_cast<std::int64_t>(sampleRate) * ptimeUs / 1'000'000); }
        int samplesPerBlock() const { return static_cast<int>(static_cast<std::int64_t>(sampleRate) * blockUs / 1'000'000); }
        int bytesPerSample() const { return bitDepth / 8; }
        int packetPayloadBytes() const { return samplesPerPacket() * channels * bytesPerSample(); }
        std::int64_t blockDurationNs() const { return static_cast<std::int64_t>(blockUs) * 1000; }
        /// Maximum channels for the packet time per ST 2110-30 conformance levels A/B (1 ms: 8) and C (125 µs: 64).
        static int maxChannels(int ptimeUs);
        std::string canonical() const;
        bool operator==(AudioFormat const& o) const;
    };

    struct AncFormat
    {
        Rational rate{50, 1};
        Interlace interlace = Interlace::Progressive;

        /// One grain per field for interlaced formats (owner decision Q10).
        Rational grainRate() const;
        std::int64_t grainDurationNs() const { return periodNs(grainRate()); }
        std::string canonical() const;
        bool operator==(AncFormat const& o) const;
    };

    /// MXL data grain size (lib/include/mxl/dataformat.h MXL_DATA_FORMAT_GRAIN_SIZE).
    inline constexpr std::size_t ancGrainBytes = 4096;
}
