// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "mxlbridge/flowdef.hpp"

using namespace mxlgw;
using namespace mxlgw::mxlbridge;

namespace
{
    FlowIdentity identity()
    {
        FlowIdentity id;
        id.flowId = *util::parseUuid("fb75bb21-36a1-5045-8a3d-a8ee69e913d7");
        id.sourceId = *util::parseUuid("1100764e-6ce9-5568-8333-c8a66547f1dc");
        id.deviceId = *util::parseUuid("6c370020-79bf-5cf6-8f3b-c12e3cb28b5d");
        id.label = "CAM 1 V";
        id.description = "desc";
        id.groupHint = "CAM 1:Video 1";
        id.version = nmosVersion(1'773'758'086'505'760'400LL);
        return id;
    }
}

TEST_CASE("video flow definition (BCP-007-03 example shape)")
{
    config::VideoFormat f;
    auto const j = videoFlowDef(identity(), f);
    CHECK(j["media_type"] == "video/v210");
    CHECK(j["format"] == "urn:x-nmos:format:video");
    CHECK(j["frame_width"] == 1920);
    CHECK(j["grain_rate"]["numerator"] == 50);
    CHECK(j["components"].size() == 3);
    CHECK(j["components"][1]["width"] == 960);
    CHECK(j["tags"]["urn:x-nmos:tag:grouphint/v1.0"][0] == "CAM 1:Video 1");
    CHECK(j["version"] == "1773758086:505760400");
    CHECK(compareVideo(j, f).empty());
    auto g = f;
    g.rate = {25, 1};
    g.interlace = config::Interlace::InterlacedTff;
    CHECK(compareVideo(j, g).size() == 2);
    auto wrong = j;
    wrong["media_type"] = "video/v210a";
    CHECK(compareVideo(wrong, f).size() == 1);
    wrong = j;
    wrong["frame_width"] = 3840;
    CHECK(compareVideo(wrong, f).size() == 1);
}

TEST_CASE("audio and data flow definitions")
{
    config::AudioFormat a;
    a.channels = 8;
    auto const ja = audioFlowDef(identity(), a);
    CHECK(ja["media_type"] == "audio/float32");
    CHECK(ja["bit_depth"] == 32);
    CHECK(ja["channel_count"] == 8);
    CHECK(compareAudio(ja, a).empty());
    auto b = a;
    b.channels = 2;
    CHECK(compareAudio(ja, b).size() == 1);
    auto noCount = ja;
    noCount.erase("channel_count");
    CHECK(compareAudio(noCount, b).size() == 1); // MXL default 1

    config::AncFormat n;
    n.rate = {25, 1};
    n.interlace = config::Interlace::InterlacedTff;
    auto const jd = ancFlowDef(identity(), n);
    CHECK(jd["media_type"] == "video/smpte291");
    CHECK(jd["grain_rate"]["numerator"] == 50); // field rate (Q10)
    CHECK(compareAnc(jd, n).empty());
    config::AncFormat p;
    CHECK(compareAnc(jd, p).empty()); // 50/1 progressive has the same grain rate
    p.rate = {30000, 1001};
    CHECK(compareAnc(jd, p).size() == 1);
    auto wrong = jd;
    wrong["media_type"] = "x";
    wrong.erase("grain_rate");
    CHECK(compareAnc(wrong, n).size() == 2);
}

TEST_CASE("writer options and versions")
{
    auto const opts = nlohmann::json::parse(writerOptions(1, 48));
    CHECK(opts["maxCommitBatchSizeHint"] == 1);
    CHECK(opts["maxSyncBatchSizeHint"] == 48);
    CHECK(nmosVersion(5'000'000'007LL) == "5:7");
}
