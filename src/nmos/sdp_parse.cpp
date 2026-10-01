// SPDX-License-Identifier: MIT
#include "nmos/sdp_parse.hpp"

#include "nmos/sdp_utils.h"
#include "sdp/sdp.h"

namespace mxlgw::nmosnode
{
    sdpmap::SdpMedia parseSdpMedia(std::string const& sdpText)
    {
        auto const session = sdp::parse_session_description(utility::s2us(sdpText));
        auto const params = nmos::get_session_description_sdp_parameters(session);
        sdpmap::SdpMedia m;
        m.mediaType = utility::us2s(params.media_type.name) + "/" + utility::us2s(params.rtpmap.encoding_name);
        m.payloadType = static_cast<int>(params.rtpmap.payload_type);
        if (params.rtpmap.encoding_name == U("raw"))
        {
            auto const v = nmos::get_video_raw_parameters(params);
            m.width = static_cast<int>(v.width);
            m.height = static_cast<int>(v.height);
            m.rate = {static_cast<std::int64_t>(v.exactframerate.numerator()), static_cast<std::int64_t>(v.exactframerate.denominator())};
            m.interlace = v.interlace;
            m.sampling = utility::us2s(v.sampling.name);
            m.depth = static_cast<int>(v.depth);
            m.colorimetry = utility::us2s(v.colorimetry.name);
            m.tcs = utility::us2s(v.tcs.name);
        }
        else if (params.rtpmap.encoding_name == U("L24") || params.rtpmap.encoding_name == U("L16"))
        {
            auto const a = nmos::get_audio_L_parameters(params);
            m.channels = static_cast<int>(a.channel_count);
            m.sampleRate = static_cast<int>(a.sample_rate);
            m.ptimeMs = a.packet_time;
        }
        else if (params.rtpmap.encoding_name == U("smpte291"))
        {
            auto const d = nmos::get_video_smpte291_parameters(params);
            if (d.exactframerate.numerator() != 0)
            {
                m.exactFrameRate =
                    util::Rational{static_cast<std::int64_t>(d.exactframerate.numerator()), static_cast<std::int64_t>(d.exactframerate.denominator())};
            }
        }
        return m;
    }
}
