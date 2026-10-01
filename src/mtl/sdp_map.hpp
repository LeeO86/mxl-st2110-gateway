// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "mtl/backend.hpp"

namespace mxlgw::sdpmap
{
    /// MTL-independent view of a parsed ST 2110 SDP (filled from nmos-cpp sdp_parameters, §7.5).
    struct SdpMedia
    {
        std::string mediaType; // "video/raw", "audio/L24", "audio/L16", "video/smpte291"
        int payloadType = 0;
        // video/raw
        int width = 0;
        int height = 0;
        util::Rational rate{0, 1};
        bool interlace = false;
        bool topFieldFirst = true;
        std::string sampling;
        int depth = 0;
        std::string colorimetry;
        std::string tcs;
        // audio
        int channels = 0;
        int sampleRate = 0;
        double ptimeMs = 0.0;
        // smpte291
        std::optional<util::Rational> exactFrameRate;
        // one entry per media section (2 with a=group:DUP)
        std::vector<media::LegAddress> legs;
    };

    /// Checks the SDP against the essence's fixed format (§6.4). Empty = match.
    std::vector<std::string> checkVideo(SdpMedia const& sdp, config::VideoEssence const& essence);
    std::vector<std::string> checkAudio(SdpMedia const& sdp, config::AudioEssence const& essence);
    std::vector<std::string> checkAnc(SdpMedia const& sdp, config::AncEssence const& essence);

    /// IS-05 receiver transport parameters for one leg.
    struct RtpReceiverParams
    {
        std::optional<std::string> multicastIp;
        std::optional<std::string> sourceIp;
        std::optional<int> destinationPort;
        bool rtpEnabled = true;
    };

    /// Merges SDP legs (when an SDP was given) with staged transport params; a one-leg SDP on a
    /// redundant receiver leaves leg 2 disabled (§7.5).
    std::vector<media::LegAddress> receiverLegs(std::optional<SdpMedia> const& sdp, std::vector<RtpReceiverParams> const& params, bool redundant);

    /// IS-05 sender transport parameters for one leg (resolved: no "auto").
    struct RtpSenderParams
    {
        std::string destinationIp;
        int destinationPort = 0;
        std::string sourceIp;
        bool rtpEnabled = true;
    };

    std::vector<media::LegAddress> senderLegs(std::vector<RtpSenderParams> const& params);

    /// MTL fps value for a rate (frames per second as written in MTL's st_fps table), for logging/tests.
    std::string fpsName(util::Rational rate, bool interlaced);
}
