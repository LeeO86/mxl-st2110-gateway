// SPDX-License-Identifier: MIT
// libFuzzer target: SDP text -> nmos-cpp parser -> SdpMedia -> format checks and leg merge
// (§7.5, §6.4, §18 robustness). Malformed SDPs must throw std::exception (IS-05 error), never crash.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

#include "config/config.hpp"
#include "mtl/sdp_map.hpp"
#include "nmos/sdp_parse.hpp"

namespace
{
    mxlgw::config::Config const& sampleConfig()
    {
        static mxlgw::config::Config const config = []
        {
            auto const j = nlohmann::json::parse(R"({
              "schema_version": 1,
              "node": {"id": "11111111-1111-4111-8111-111111111111", "label": "FUZZ"},
              "nic": {"backend": "mock", "port_pairs": [{"name": "media",
                      "primary": {"name": "media-p", "ip": "10.1.1.21", "netmask": "255.255.255.0"}}]},
              "ptp": {"mode": "external", "require_lock": false},
              "mxl": {"scan_path": null, "domains": [{"name": "main", "path": "/Volumes/mxl/main"}]},
              "groups": [{"uid": "00000000-0000-4000-8000-000000000100", "label": "CAM", "direction": "ingest", "domain": "main",
                "video": [{"uid": "00000000-0000-4000-8000-000000000101", "label": "V", "width": 1920, "height": 1080, "rate": "50/1"}],
                "audio": [{"uid": "00000000-0000-4000-8000-000000000102", "label": "A", "channels": 8}],
                "anc": [{"uid": "00000000-0000-4000-8000-000000000103", "label": "ANC"}]}]
            })");
            auto parsed = mxlgw::config::parseAndValidate(j);
            if (!parsed.ok())
            {
                std::abort();
            }
            return *parsed.config;
        }();
        return config;
    }
}

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using namespace mxlgw;
    std::string const text(reinterpret_cast<char const*>(data), size);
    sdpmap::SdpMedia media;
    try
    {
        media = nmosnode::parseSdpMedia(text);
    }
    catch (std::exception const&)
    {
        return 0;
    }
    auto const& group = sampleConfig().groups.front();
    (void)sdpmap::checkVideo(media, group.video.front());
    (void)sdpmap::checkAudio(media, group.audio.front());
    (void)sdpmap::checkAnc(media, group.anc.front());
    std::vector<sdpmap::RtpReceiverParams> params(2);
    params[0].multicastIp = "239.1.1.1";
    params[0].destinationPort = 20000;
    (void)sdpmap::receiverLegs(media, params, true);
    (void)sdpmap::receiverLegs(media, params, false);
    return 0;
}
