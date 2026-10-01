// SPDX-License-Identifier: MIT
#include "config/formats.hpp"

namespace mxlgw::config
{
    char const* toName(Interlace i)
    {
        switch (i)
        {
            case Interlace::Progressive: return "progressive";
            case Interlace::InterlacedTff: return "interlaced_tff";
            case Interlace::InterlacedBff: return "interlaced_bff";
        }
        return "progressive";
    }

    std::optional<Interlace> parseInterlace(std::string const& text)
    {
        if (text == "progressive")
        {
            return Interlace::Progressive;
        }
        if (text == "interlaced_tff")
        {
            return Interlace::InterlacedTff;
        }
        if (text == "interlaced_bff")
        {
            return Interlace::InterlacedBff;
        }
        return std::nullopt;
    }

    std::vector<Rational> const& supportedVideoRates()
    {
        static std::vector<Rational> const rates = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {50, 1}, {60000, 1001}, {60, 1}};
        return rates;
    }

    bool isSupportedVideoRate(Rational rate)
    {
        for (auto const& r : supportedVideoRates())
        {
            if (r == rate)
            {
                return true;
            }
        }
        return false;
    }

    bool interlaceAllowed(Rational rate)
    {
        return rate == Rational{25, 1} || rate == Rational{30000, 1001};
    }

    std::int64_t periodNs(Rational rate)
    {
        if (rate.num <= 0 || rate.den <= 0)
        {
            return 0;
        }
        auto const num = static_cast<__int128>(rate.den) * 1'000'000'000;
        return static_cast<std::int64_t>((num + rate.num / 2) / rate.num);
    }

    Rational VideoFormat::grainRate() const
    {
        return interlaced() ? Rational{rate.num * 2, rate.den} : rate;
    }

    std::size_t VideoFormat::v210Stride() const
    {
        return static_cast<std::size_t>((width + 47) / 48) * 128;
    }

    std::string VideoFormat::canonical() const
    {
        return "video/v210;" + std::to_string(width) + "x" + std::to_string(height) + ";" + rate.toString() + ";" + toName(interlace) + ";" + colorimetry +
               ";" + tcs;
    }

    bool VideoFormat::operator==(VideoFormat const& o) const
    {
        return width == o.width && height == o.height && rate == o.rate && interlace == o.interlace && colorimetry == o.colorimetry && tcs == o.tcs;
    }

    int AudioFormat::maxChannels(int ptimeUs)
    {
        return ptimeUs <= 125 ? 64 : 8;
    }

    std::string AudioFormat::canonical() const
    {
        return "audio/float32;" + std::to_string(sampleRate) + ";" + std::to_string(channels);
    }

    bool AudioFormat::operator==(AudioFormat const& o) const
    {
        return channels == o.channels && bitDepth == o.bitDepth && sampleRate == o.sampleRate && ptimeUs == o.ptimeUs && blockUs == o.blockUs;
    }

    Rational AncFormat::grainRate() const
    {
        return interlace != Interlace::Progressive ? Rational{rate.num * 2, rate.den} : rate;
    }

    std::string AncFormat::canonical() const
    {
        return "video/smpte291;" + grainRate().toString();
    }

    bool AncFormat::operator==(AncFormat const& o) const
    {
        return rate == o.rate && interlace == o.interlace;
    }
}
