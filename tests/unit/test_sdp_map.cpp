// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "config/config.hpp"
#include "helpers.hpp"
#include "mtl/sdp_map.hpp"

using namespace mxlgw;

namespace
{
    config::Config sample()
    {
        auto r = config::parseAndValidate(testutil::sampleConfig());
        REQUIRE(r.ok());
        return *r.config;
    }

    sdpmap::SdpMedia video1080p50()
    {
        sdpmap::SdpMedia m;
        m.mediaType = "video/raw";
        m.width = 1920;
        m.height = 1080;
        m.rate = {50, 1};
        m.sampling = "YCbCr-4:2:2";
        m.depth = 10;
        m.legs = {{"239.1.1.1", "10.1.1.50", 20000, true}, {"239.2.1.1", "10.2.1.50", 20000, true}};
        return m;
    }
}

TEST_CASE("video format checks")
{
    auto const c = sample();
    auto const& v = c.groups[0].video[0];
    CHECK(sdpmap::checkVideo(video1080p50(), v).empty());
    auto m = video1080p50();
    m.rate = {25, 1};
    m.interlace = true;
    auto const errs = sdpmap::checkVideo(m, v);
    CHECK(errs.size() == 2);
    m = video1080p50();
    m.width = 3840;
    m.height = 2160;
    CHECK(sdpmap::checkVideo(m, v).size() == 1);
    m = video1080p50();
    m.sampling = "RGB";
    m.depth = 12;
    CHECK(sdpmap::checkVideo(m, v).size() == 2);
    m.mediaType = "audio/L24";
    CHECK(sdpmap::checkVideo(m, v).size() == 1);
    m = video1080p50();
    m.rate = {100, 2}; // equivalent rational
    CHECK(sdpmap::checkVideo(m, v).empty());
}

TEST_CASE("audio and ANC format checks")
{
    auto const c = sample();
    auto const& a = c.groups[0].audio[0];
    sdpmap::SdpMedia m;
    m.mediaType = "audio/L24";
    m.channels = 8;
    m.sampleRate = 48000;
    m.ptimeMs = 1.0;
    CHECK(sdpmap::checkAudio(m, a).empty());
    m.ptimeMs = 0.125;
    m.channels = 16;
    m.mediaType = "audio/L16";
    CHECK(sdpmap::checkAudio(m, a).size() == 3);
    m.sampleRate = 96000;
    CHECK(sdpmap::checkAudio(m, a).size() == 4);

    auto const& n = c.groups[0].anc[0];
    sdpmap::SdpMedia anc;
    anc.mediaType = "video/smpte291";
    CHECK(sdpmap::checkAnc(anc, n).empty());
    anc.exactFrameRate = util::Rational{60000, 1001};
    CHECK(sdpmap::checkAnc(anc, n).size() == 1);
    anc.mediaType = "video/raw";
    CHECK(sdpmap::checkAnc(anc, n).size() == 2);
}

TEST_CASE("receiver legs from SDP and transport params")
{
    auto const sdp = video1080p50();
    auto legs = sdpmap::receiverLegs(sdp, {}, true);
    REQUIRE(legs.size() == 2);
    CHECK(legs[0].destination == "239.1.1.1");
    CHECK(legs[1].enabled);

    // One-leg SDP on a redundant group: leg 2 disabled (§7.5).
    auto one = sdp;
    one.legs.resize(1);
    legs = sdpmap::receiverLegs(one, {}, true);
    CHECK(legs[0].enabled);
    CHECK_FALSE(legs[1].enabled);

    // Transport params only.
    sdpmap::RtpReceiverParams p0;
    p0.multicastIp = "239.9.9.9";
    p0.destinationPort = 5004;
    p0.sourceIp = "10.0.0.1";
    sdpmap::RtpReceiverParams p1;
    p1.rtpEnabled = false;
    legs = sdpmap::receiverLegs(std::nullopt, {p0, p1}, true);
    CHECK(legs[0].enabled);
    CHECK(legs[0].destination == "239.9.9.9");
    CHECK(legs[0].source == "10.0.0.1");
    CHECK(legs[0].port == 5004);
    CHECK_FALSE(legs[1].enabled);

    // Params override the SDP address; rtp_enabled=false disables.
    p0.rtpEnabled = false;
    legs = sdpmap::receiverLegs(sdp, {p0}, false);
    REQUIRE(legs.size() == 1);
    CHECK(legs[0].destination == "239.9.9.9");
    CHECK_FALSE(legs[0].enabled);
}

TEST_CASE("sender legs and fps names")
{
    auto const legs = sdpmap::senderLegs({{"239.10.0.1", 20000, "10.1.1.21", true}, {"", 0, "", true}});
    REQUIRE(legs.size() == 2);
    CHECK(legs[0].enabled);
    CHECK_FALSE(legs[1].enabled);
    CHECK(sdpmap::fpsName({50, 1}, false) == "p50");
    CHECK(sdpmap::fpsName({25, 1}, true) == "p50");
    CHECK(sdpmap::fpsName({30000, 1001}, false) == "p29_97");
    CHECK(sdpmap::fpsName({30000, 1001}, true) == "p59_94");
    CHECK(sdpmap::fpsName({24000, 1001}, false) == "p23_98");
}
