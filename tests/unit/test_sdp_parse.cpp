// SPDX-License-Identifier: MIT
// SDP fixtures (§17.1) through nmos-cpp's parser and the §6.4 format checks.
#include <doctest/doctest.h>

#include <string>

#include "config/config.hpp"
#include "helpers.hpp"
#include "mtl/sdp_map.hpp"
#include "nmos/sdp_parse.hpp"
#include "util/fs.hpp"

using namespace mxlgw;

namespace
{
    std::string fixture(std::string const& name)
    {
        auto const text = util::readFile(std::string(MXLGW_SOURCE_DIR) + "/tests/fixtures/sdp/" + name);
        REQUIRE_MESSAGE(text.has_value(), name);
        return *text;
    }

    config::Config const& essences()
    {
        static config::Config const cfg = []
        {
            auto j = testutil::sampleConfig();
            auto& g = j["groups"][0];
            g["video"].push_back(
                {{"uid", testutil::uid(901)}, {"label", "i50"}, {"width", 1920}, {"height", 1080}, {"rate", "25/1"}, {"interlace", "interlaced_tff"}});
            g["video"].push_back({{"uid", testutil::uid(902)}, {"label", "uhd"}, {"width", 3840}, {"height", 2160}, {"rate", "50/1"}});
            g["audio"].push_back({{"uid", testutil::uid(903)}, {"label", "madi"}, {"channels", 16}, {"ptime_us", 125}, {"block_us", 1000}});
            auto parsed = config::parseAndValidate(j);
            INFO(config::formatErrors(parsed.errors));
            REQUIRE(parsed.ok());
            return *parsed.config;
        }();
        return cfg;
    }
}

TEST_CASE("video SDPs: 1080p50 with DUP, 1080i50, 2160p50")
{
    auto const& g = essences().groups[0];
    auto const p50 = nmosnode::parseSdpMedia(fixture("video-1080p50-dup.sdp"));
    CHECK(p50.mediaType == "video/raw");
    CHECK(p50.payloadType == 96);
    CHECK(p50.width == 1920);
    CHECK(p50.height == 1080);
    CHECK(p50.rate == util::Rational{50, 1});
    CHECK_FALSE(p50.interlace);
    CHECK(p50.sampling == "YCbCr-4:2:2");
    CHECK(p50.depth == 10);
    CHECK(p50.colorimetry == "BT709");
    CHECK(p50.tcs == "SDR");
    CHECK(sdpmap::checkVideo(p50, g.video[0]).empty());
    CHECK(sdpmap::checkVideo(p50, g.video[1]).size() == 2); // rate + scan mode
    CHECK_FALSE(sdpmap::checkAudio(p50, g.audio[0]).empty());

    auto const i50 = nmosnode::parseSdpMedia(fixture("video-1080i50.sdp"));
    CHECK(i50.interlace);
    CHECK(i50.rate == util::Rational{25, 1});
    CHECK(sdpmap::checkVideo(i50, g.video[1]).empty());
    auto const mismatch = sdpmap::checkVideo(i50, g.video[0]);
    REQUIRE(mismatch.size() == 2);
    CHECK(mismatch[0].find("exactframerate") == 0);
    CHECK(mismatch[1].find("scan mode interlaced") == 0);

    auto const uhd = nmosnode::parseSdpMedia(fixture("video-2160p50.sdp"));
    CHECK(uhd.width == 3840);
    CHECK(uhd.height == 2160);
    CHECK(uhd.payloadType == 98);
    CHECK(sdpmap::checkVideo(uhd, g.video[2]).empty());
    CHECK(sdpmap::checkVideo(uhd, g.video[0]).front().find("size 3840x2160") == 0);
}

TEST_CASE("audio SDPs: 8 ch L24 1 ms with DUP, 16 ch L24 125 us")
{
    auto const& g = essences().groups[0];
    auto const a8 = nmosnode::parseSdpMedia(fixture("audio-8ch-l24-1ms-dup.sdp"));
    CHECK(a8.mediaType == "audio/L24");
    CHECK(a8.channels == 8);
    CHECK(a8.sampleRate == 48000);
    CHECK(a8.ptimeMs == doctest::Approx(1.0));
    CHECK(sdpmap::checkAudio(a8, g.audio[0]).empty());
    CHECK(sdpmap::checkAudio(a8, g.audio[1]).size() == 2); // channels + ptime

    auto const a16 = nmosnode::parseSdpMedia(fixture("audio-16ch-l24-125us.sdp"));
    CHECK(a16.channels == 16);
    CHECK(a16.ptimeMs == doctest::Approx(0.125));
    CHECK(sdpmap::checkAudio(a16, g.audio[1]).empty());
    CHECK_FALSE(sdpmap::checkVideo(a16, g.video[0]).empty());
}

TEST_CASE("ANC SDP")
{
    auto const& g = essences().groups[0];
    auto const anc = nmosnode::parseSdpMedia(fixture("anc-50.sdp"));
    CHECK(anc.mediaType == "video/smpte291");
    CHECK(anc.payloadType == 100);
    REQUIRE(anc.exactFrameRate);
    CHECK(*anc.exactFrameRate == util::Rational{50, 1});
    CHECK(sdpmap::checkAnc(anc, g.anc[0]).empty());
    CHECK_FALSE(sdpmap::checkVideo(anc, g.video[0]).empty());
}

TEST_CASE("malformed SDPs throw instead of crashing")
{
    for (auto const* name : {"malformed-no-rtpmap.sdp", "malformed-fmtp.sdp", "malformed-garbage.sdp"})
    {
        CHECK_THROWS_AS_MESSAGE(nmosnode::parseSdpMedia(fixture(name)), std::exception, name);
    }
    CHECK_THROWS_AS(nmosnode::parseSdpMedia(""), std::exception);
}
