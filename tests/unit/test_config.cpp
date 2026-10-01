// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "config/config.hpp"
#include "helpers.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using nlohmann::json;

namespace
{
    config::ParseResult parse(json const& j)
    {
        return config::parseAndValidate(j);
    }

    bool hasError(config::ParseResult const& r, std::string const& pointer)
    {
        for (auto const& e : r.errors)
        {
            if (e.pointer == pointer)
            {
                return true;
            }
        }
        return false;
    }
}

TEST_CASE("sample configuration parses with defaults")
{
    auto const r = parse(testutil::sampleConfig());
    INFO(config::formatErrors(r.errors));
    REQUIRE(r.ok());
    auto const& c = *r.config;
    CHECK(c.node.httpPort == 18080);
    CHECK(c.nic.backend == config::Backend::Mock);
    CHECK(c.groups.size() == 2);
    auto const& g = c.groups[0];
    CHECK(g.direction == config::Direction::Ingest);
    CHECK(g.redundancy);
    CHECK(g.enabled);
    CHECK(g.video[0].format.rate == util::Rational{50, 1});
    CHECK(g.video[0].payloadType == 96);
    CHECK(g.audio[0].payloadType == 97);
    CHECK(g.anc[0].payloadType == 100);
    CHECK(g.anc[0].format.rate == util::Rational{50, 1}); // from the group's video
    CHECK(g.video[0].legs.size() == 2);
    CHECK(c.groups[1].effectiveOutputDelayNs() == 40'000'000); // two grains at 50p
    CHECK(c.groups[1].missingData == config::MissingData::Black);
    CHECK(c.groups[1].audio[0].format.blockUs == 1000);
    CHECK_FALSE(c.unconfigured());
    CHECK(c.findGroup(g.uid) == &c.groups[0]);
    CHECK(c.mxl.findDomain("main") != nullptr);
    CHECK(c.mxl.findDomain("other") == nullptr);
}

TEST_CASE("minimal example is setup mode")
{
    auto const text = util::readFile(std::string(MXLGW_SOURCE_DIR) + "/config/examples/gateway.minimal.json");
    REQUIRE(text);
    auto const r = parse(json::parse(*text));
    REQUIRE(r.ok());
    CHECK(r.config->unconfigured());
    CHECK(r.config->mxl.scanPath == std::string("/Volumes/mxl"));
}

TEST_CASE("every shipped example passes the semantic rules")
{
    for (auto const* name : {"gateway.example.json", "gateway.fabrics-host-a.json", "gateway.fabrics-host-b.json"})
    {
        auto const text = util::readFile(std::string(MXLGW_SOURCE_DIR) + "/config/examples/" + name);
        REQUIRE_MESSAGE(text, name);
        auto const r = parse(json::parse(*text));
        INFO(name << ": " << config::formatErrors(r.errors));
        CHECK(r.ok());
    }
}

TEST_CASE("semantic rules")
{
    SUBCASE("redundant port required")
    {
        auto j = testutil::sampleConfig();
        j["nic"]["port_pairs"][0].erase("redundant");
        CHECK(hasError(parse(j), "/nic/port_pairs/0/redundant"));
    }
    SUBCASE("second leg requires redundancy")
    {
        auto j = testutil::sampleConfig();
        j["groups"][1]["video"][0]["defaults"]["legs"].push_back({{"multicast", "239.20.0.1"}, {"port", 20000}});
        CHECK(hasError(parse(j), "/groups/1/video/0/defaults/legs"));
    }
    SUBCASE("multicast and duplicate egress destinations")
    {
        auto j = testutil::sampleConfig();
        j["groups"][1]["audio"][0]["defaults"]["legs"][0]["multicast"] = "10.0.0.1";
        CHECK(hasError(parse(j), "/groups/1/audio/0/defaults/legs/0/multicast"));
        j = testutil::sampleConfig();
        j["groups"][1]["audio"][0]["defaults"]["legs"][0] = {{"multicast", "239.10.0.1"}, {"port", 20000}};
        CHECK(hasError(parse(j), "/groups/1/audio/0/defaults/legs/0"));
    }
    SUBCASE("unique labels and uids")
    {
        auto j = testutil::sampleConfig();
        j["groups"][1]["label"] = "CAM 1";
        CHECK(hasError(parse(j), "/groups/1/label"));
        j = testutil::sampleConfig();
        j["groups"][1]["video"][0]["uid"] = j["groups"][0]["video"][0]["uid"];
        CHECK(hasError(parse(j), "/groups/1/video/0/uid"));
    }
    SUBCASE("domain reference and mirror path")
    {
        auto j = testutil::sampleConfig();
        j["groups"][0]["domain"] = "nowhere";
        CHECK(hasError(parse(j), "/groups/0/domain"));
        j = testutil::sampleConfig("/Volumes/mxl/mirror-abc");
        CHECK(hasError(parse(j), "/mxl/domains/0/path"));
        j = testutil::sampleConfig();
        j["mxl"]["domains"].push_back(j["mxl"]["domains"][0]);
        auto const r = parse(j);
        CHECK(hasError(r, "/mxl/domains/1/name"));
        CHECK(hasError(r, "/mxl/domains/1/path"));
    }
    SUBCASE("interlace rules")
    {
        auto j = testutil::sampleConfig();
        j["groups"][0]["video"][0]["interlace"] = "interlaced_tff";
        CHECK(hasError(parse(j), "/groups/0/video/0/interlace")); // 50/1 not allowed
        j["groups"][0]["video"][0]["rate"] = "25/1";
        auto const ok = parse(j);
        INFO(config::formatErrors(ok.errors));
        CHECK(ok.ok());
        CHECK(ok.config->groups[0].anc[0].format.interlace == config::Interlace::InterlacedTff);
        CHECK(ok.config->groups[0].anc[0].format.grainRate() == util::Rational{50, 1});
        j["groups"][0]["video"][0]["width"] = 3840;
        j["groups"][0]["video"][0]["height"] = 2160;
        CHECK(hasError(parse(j), "/groups/0/video/0/interlace"));
    }
    SUBCASE("size combination")
    {
        auto j = testutil::sampleConfig();
        j["groups"][0]["video"][0]["height"] = 2160;
        CHECK(hasError(parse(j), "/groups/0/video/0/width"));
    }
    SUBCASE("audio packet time, block and channel limits")
    {
        auto j = testutil::sampleConfig();
        j["groups"][0]["audio"][0]["block_us"] = 1500;
        CHECK(hasError(parse(j), "/groups/0/audio/0/block_us"));
        j = testutil::sampleConfig();
        j["groups"][0]["audio"][0]["channels"] = 16;
        CHECK(hasError(parse(j), "/groups/0/audio/0/channels"));
        j["groups"][0]["audio"][0]["ptime_us"] = 125;
        CHECK(parse(j).ok());
    }
    SUBCASE("read offsets")
    {
        auto j = testutil::sampleConfig();
        j["groups"][0]["video"][0]["read_offset_grains"] = 1;
        CHECK(hasError(parse(j), "/groups/0/video/0/read_offset_grains"));
        j = testutil::sampleConfig();
        j["groups"][1]["video"][0]["read_offset_grains"] = 1;
        j["groups"][1]["video"][0]["read_offset_ns"] = 1000;
        CHECK(hasError(parse(j), "/groups/1/video/0/read_offset_ns"));
        j = testutil::sampleConfig();
        j["mxl"]["default_read_offset_grains"] = 1;
        j["mxl"]["default_read_offset_ns"] = 1;
        CHECK(hasError(parse(j), "/mxl/default_read_offset_ns"));
    }
    SUBCASE("output delay minimum")
    {
        auto j = testutil::sampleConfig();
        j["groups"][1]["output_delay_ns"] = 21'000'000; // < 20 ms + 0 + 2 ms
        CHECK(hasError(parse(j), "/groups/1/output_delay_ns"));
        j["groups"][1]["output_delay_ns"] = 22'000'000;
        CHECK(parse(j).ok());
        j["groups"][1]["video"][0]["read_offset_grains"] = 2;
        CHECK(hasError(parse(j), "/groups/1/output_delay_ns")); // needs 62 ms
        j["groups"][1]["output_delay_ns"] = 62'000'000;
        CHECK(parse(j).ok());
        j["mxl"]["default_read_offset_grains"] = 3;
        j["groups"][1]["video"][0].erase("read_offset_grains");
        CHECK(hasError(parse(j), "/groups/1/output_delay_ns")); // default applies: 82 ms
        j = testutil::sampleConfig();
        j["groups"][0]["output_delay_ns"] = 40'000'000;
        CHECK(hasError(parse(j), "/groups/0/output_delay_ns"));
    }
    SUBCASE("ANC needs a rate without video")
    {
        auto j = testutil::sampleConfig();
        j["groups"][0].erase("video");
        CHECK(hasError(parse(j), "/groups/0/anc/0/rate"));
        j["groups"][0]["anc"][0]["rate"] = "25/1";
        CHECK(parse(j).ok());
    }
    SUBCASE("egress video and ANC share the group cadence")
    {
        auto j = testutil::sampleConfig();
        j["groups"][1]["video"].push_back({{"uid", testutil::uid(950)}, {"label", "PGM V2"}, {"width", 1920}, {"height", 1080}, {"rate", "25/1"}});
        CHECK(hasError(parse(j), "/groups/1/video/1/rate"));
        j["groups"][1]["video"][1]["rate"] = "50/1";
        j["groups"][1]["anc"] = {{{"uid", testutil::uid(951)}, {"label", "PGM ANC"}, {"rate", "25/1"}}};
        CHECK(hasError(parse(j), "/groups/1/anc/0/rate"));
        j["groups"][1]["anc"][0].erase("rate"); // inherits the video rate
        CHECK(parse(j).ok());
        // 1080i25 video and field-rate ANC share 50 grains/s
        j["groups"][1]["video"].erase(1);
        j["groups"][1]["video"][0]["rate"] = "25/1";
        j["groups"][1]["video"][0]["interlace"] = "interlaced_tff";
        INFO(config::formatErrors(parse(j).errors));
        CHECK(parse(j).ok());
        // ingest groups may mix rates
        auto k = testutil::sampleConfig();
        k["groups"][0]["video"].push_back({{"uid", testutil::uid(952)}, {"label", "CAM 1 V2"}, {"width", 1920}, {"height", 1080}, {"rate", "25/1"}});
        CHECK(parse(k).ok());
    }
    SUBCASE("empty group")
    {
        auto j = testutil::sampleConfig();
        j["groups"][1].erase("video");
        j["groups"][1].erase("audio");
        CHECK(hasError(parse(j), "/groups/1"));
    }
    SUBCASE("nic backend requirements")
    {
        auto j = testutil::sampleConfig();
        j["nic"]["backend"] = "dpdk";
        auto r = parse(j);
        CHECK(hasError(r, "/nic/port_pairs/0/primary/pci"));
        j["nic"]["port_pairs"][0]["primary"]["pci"] = "0000:31:00.0";
        j["nic"]["port_pairs"][0]["redundant"]["pci"] = "env:PCIDEVICE_INTEL_COM_E810_MEDIA_R";
        CHECK(parse(j).ok());
        j["nic"]["port_pairs"][0]["primary"]["pci"] = "31:00.0";
        CHECK(hasError(parse(j), "/nic/port_pairs/0/primary/pci"));
        j = testutil::sampleConfig();
        j["nic"]["backend"] = "kernel";
        CHECK(hasError(parse(j), "/nic/port_pairs/0/primary/ifname"));
        j["nic"]["port_pairs"][0]["primary"]["ifname"] = "veth0";
        j["nic"]["port_pairs"][0]["redundant"]["ifname"] = "veth1";
        CHECK(parse(j).ok());
    }
    SUBCASE("port names, netmask, gateway, multicast port ip")
    {
        auto j = testutil::sampleConfig();
        j["nic"]["port_pairs"][0]["redundant"]["name"] = "media-p";
        CHECK(hasError(parse(j), "/nic/port_pairs/0/redundant/name"));
        j = testutil::sampleConfig();
        j["nic"]["port_pairs"][0]["primary"]["netmask"] = "255.0.255.0";
        CHECK(hasError(parse(j), "/nic/port_pairs/0/primary/netmask"));
        j = testutil::sampleConfig();
        j["nic"]["port_pairs"][0]["primary"]["gateway"] = "10.9.9.1";
        CHECK(hasError(parse(j), "/nic/port_pairs/0/primary/gateway"));
        j = testutil::sampleConfig();
        j["nic"]["port_pairs"][0]["primary"]["ip"] = "239.0.0.1";
        CHECK(hasError(parse(j), "/nic/port_pairs/0/primary/ip"));
    }
    SUBCASE("lcores and app cpus")
    {
        auto j = testutil::sampleConfig();
        j["nic"]["lcores"] = "4-9";
        j["nic"]["app_cpus"] = "9-12";
        CHECK(hasError(parse(j), "/nic/app_cpus"));
        j["nic"]["app_cpus"] = "x";
        CHECK(hasError(parse(j), "/nic/app_cpus"));
        j["nic"]["lcores"] = "9-4";
        CHECK(hasError(parse(j), "/nic/lcores"));
    }
    SUBCASE("registry, tls, ptp thresholds")
    {
        auto j = testutil::sampleConfig();
        j["node"]["registry"] = {{"mode", "static"}};
        auto r = parse(j);
        CHECK(hasError(r, "/node/registry/address"));
        CHECK(hasError(r, "/node/registry/port"));
        j = testutil::sampleConfig();
        j["node"]["tls"] = {{"enabled", true}};
        CHECK(hasError(parse(j), "/node/tls"));
        j = testutil::sampleConfig();
        j["ptp"]["warn_offset_ns"] = 2'000'000;
        CHECK(hasError(parse(j), "/ptp/warn_offset_ns"));
    }
}

TEST_CASE("toJson round trip")
{
    auto const r = parse(testutil::sampleConfig());
    REQUIRE(r.ok());
    auto const again = parse(json::parse(config::toJson(*r.config).dump()));
    INFO(config::formatErrors(again.errors));
    REQUIRE(again.ok());
    CHECK(again.config->groups.size() == r.config->groups.size());
    CHECK(again.config->groups[0].video[0].format == r.config->groups[0].video[0].format);
    CHECK(again.config->groups[0].audio[0].format == r.config->groups[0].audio[0].format);
    CHECK(again.config->groups[1].effectiveOutputDelayNs() == r.config->groups[1].effectiveOutputDelayNs());
    auto const g = config::groupFromJson(json::parse(config::toJson(r.config->groups[1]).dump()));
    CHECK(g.uid == r.config->groups[1].uid);
    CHECK(g.direction == config::Direction::Egress);
}

TEST_CASE("formats")
{
    config::VideoFormat v;
    CHECK(v.v210Stride() == 5120);
    CHECK(v.grainBytes() == 5120u * 1080u);
    CHECK(v.grainDurationNs() == 20'000'000);
    v.width = 3840;
    v.height = 2160;
    CHECK(v.v210Stride() == 10240);
    v = config::VideoFormat{};
    v.rate = {25, 1};
    v.interlace = config::Interlace::InterlacedBff;
    CHECK(v.grainRate() == util::Rational{50, 1});
    CHECK(v.linesPerGrain() == 540);
    CHECK(v.canonical() == "video/v210;1920x1080;25/1;interlaced_bff;BT709;SDR");
    CHECK(config::periodNs({30000, 1001}) == 33'366'667);
    CHECK(config::periodNs({0, 1}) == 0);
    CHECK(config::isSupportedVideoRate({60000, 1001}));
    CHECK_FALSE(config::isSupportedVideoRate({48, 1}));

    config::AudioFormat a;
    a.channels = 8;
    CHECK(a.samplesPerPacket() == 48);
    CHECK(a.samplesPerBlock() == 48);
    CHECK(a.packetPayloadBytes() == 48 * 8 * 3);
    CHECK(a.canonical() == "audio/float32;48000;8");
    CHECK(config::AudioFormat::maxChannels(125) == 64);
    CHECK(config::AudioFormat::maxChannels(1000) == 8);

    config::AncFormat n;
    CHECK(n.canonical() == "video/smpte291;50/1");
    CHECK(config::parseInterlace("interlaced_tff") == config::Interlace::InterlacedTff);
    CHECK_FALSE(config::parseInterlace("psf"));
    CHECK(std::string(config::toName(config::Backend::Kernel)) == "kernel");
    CHECK(std::string(config::toName(config::PtpMode::BuiltinPhc2sys)) == "builtin_phc2sys");
    CHECK(std::string(config::toName(config::EssenceType::Anc)) == "anc");
    CHECK(std::string(config::toName(config::Pacing::Wide)) == "wide");
    CHECK(std::string(config::toName(config::Packing::GpmSl)) == "GPM_SL");
}
