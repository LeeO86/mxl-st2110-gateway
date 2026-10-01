// SPDX-License-Identifier: MIT
#include "mtl/sdp_map.hpp"

#include <cmath>

namespace mxlgw::sdpmap
{
    namespace
    {
        std::string rateText(util::Rational r)
        {
            return r.toString();
        }

        bool sameRate(util::Rational a, util::Rational b)
        {
            return static_cast<__int128>(a.num) * b.den == static_cast<__int128>(b.num) * a.den;
        }
    }

    std::vector<std::string> checkVideo(SdpMedia const& sdp, config::VideoEssence const& essence)
    {
        std::vector<std::string> out;
        auto const& f = essence.format;
        if (sdp.mediaType != "video/raw")
        {
            out.push_back("media type " + sdp.mediaType + " is not video/raw");
            return out;
        }
        if (sdp.width != f.width || sdp.height != f.height)
        {
            out.push_back("size " + std::to_string(sdp.width) + "x" + std::to_string(sdp.height) + " does not match " + std::to_string(f.width) + "x" +
                          std::to_string(f.height));
        }
        if (!sameRate(sdp.rate, f.rate))
        {
            out.push_back("exactframerate " + rateText(sdp.rate) + " does not match " + rateText(f.rate));
        }
        if (sdp.interlace != f.interlaced())
        {
            out.push_back(std::string("scan mode ") + (sdp.interlace ? "interlaced" : "progressive") + " does not match " + config::toName(f.interlace));
        }
        if (!sdp.sampling.empty() && sdp.sampling != "YCbCr-4:2:2")
        {
            out.push_back("sampling " + sdp.sampling + " is not YCbCr-4:2:2");
        }
        if (sdp.depth != 0 && sdp.depth != 10)
        {
            out.push_back("depth " + std::to_string(sdp.depth) + " is not 10");
        }
        return out;
    }

    std::vector<std::string> checkAudio(SdpMedia const& sdp, config::AudioEssence const& essence)
    {
        std::vector<std::string> out;
        auto const& f = essence.format;
        std::string const expected = f.bitDepth == 24 ? "audio/L24" : "audio/L16";
        if (sdp.mediaType != expected)
        {
            out.push_back("media type " + sdp.mediaType + " is not " + expected);
        }
        if (sdp.channels != f.channels)
        {
            out.push_back("channel count " + std::to_string(sdp.channels) + " does not match " + std::to_string(f.channels));
        }
        if (sdp.sampleRate != f.sampleRate)
        {
            out.push_back("sample rate " + std::to_string(sdp.sampleRate) + " does not match " + std::to_string(f.sampleRate));
        }
        if (sdp.ptimeMs > 0.0 && std::fabs(sdp.ptimeMs * 1000.0 - f.ptimeUs) > 0.5)
        {
            out.push_back("ptime " + std::to_string(sdp.ptimeMs) + " ms does not match " + std::to_string(f.ptimeUs) + " us");
        }
        return out;
    }

    std::vector<std::string> checkAnc(SdpMedia const& sdp, config::AncEssence const& essence)
    {
        std::vector<std::string> out;
        if (sdp.mediaType != "video/smpte291")
        {
            out.push_back("media type " + sdp.mediaType + " is not video/smpte291");
        }
        if (sdp.exactFrameRate && !sameRate(*sdp.exactFrameRate, essence.format.rate))
        {
            out.push_back("exactframerate " + rateText(*sdp.exactFrameRate) + " does not match " + rateText(essence.format.rate));
        }
        return out;
    }

    std::vector<media::LegAddress> receiverLegs(std::optional<SdpMedia> const& sdp, std::vector<RtpReceiverParams> const& params, bool redundant)
    {
        std::size_t const count = redundant ? 2 : 1;
        std::vector<media::LegAddress> legs(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            auto& leg = legs[i];
            leg.enabled = false;
            if (sdp && i < sdp->legs.size())
            {
                leg = sdp->legs[i];
                leg.enabled = true;
            }
            if (i < params.size())
            {
                auto const& p = params[i];
                if (p.multicastIp && !p.multicastIp->empty())
                {
                    leg.destination = *p.multicastIp;
                }
                if (p.sourceIp)
                {
                    leg.source = *p.sourceIp;
                }
                if (p.destinationPort && *p.destinationPort > 0)
                {
                    leg.port = *p.destinationPort;
                }
                bool const fromSdp = sdp && i < sdp->legs.size();
                leg.enabled = p.rtpEnabled && (fromSdp || (!leg.destination.empty() && leg.port > 0));
            }
            if (leg.destination.empty() || leg.port <= 0)
            {
                leg.enabled = false;
            }
        }
        return legs;
    }

    std::vector<media::LegAddress> senderLegs(std::vector<RtpSenderParams> const& params)
    {
        std::vector<media::LegAddress> legs;
        for (auto const& p : params)
        {
            media::LegAddress leg;
            leg.destination = p.destinationIp;
            leg.port = p.destinationPort;
            leg.source = p.sourceIp;
            leg.enabled = p.rtpEnabled && !p.destinationIp.empty() && p.destinationPort > 0;
            legs.push_back(leg);
        }
        return legs;
    }

    std::string fpsName(util::Rational rate, bool interlaced)
    {
        auto r = rate;
        if (interlaced)
        {
            r.num *= 2;
        }
        if (r == util::Rational{24000, 1001})
        {
            return "p23_98";
        }
        if (r == util::Rational{30000, 1001})
        {
            return "p29_97";
        }
        if (r == util::Rational{60000, 1001})
        {
            return "p59_94";
        }
        if (r == util::Rational{120000, 1001})
        {
            return "p119_88";
        }
        return "p" + std::to_string(r.num / (r.den == 0 ? 1 : r.den));
    }
}
