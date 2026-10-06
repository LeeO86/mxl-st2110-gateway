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
        j["node"]["registry"] = {{"mode", "static"}, {"address", "10.0.0.2"}, {"dns_sd", true}};
        CHECK(hasError(parse(j), "/node/registry/dns_sd"));
        j["node"]["registry"] = {{"address", "10.0.0.2"}, {"port", 65535}};
        CHECK(hasError(parse(j), "/node/registry/query_port"));
        j["node"]["registry"]["query_port"] = 3211;
        CHECK(parse(j).ok());
        j = testutil::sampleConfig();
        j["node"]["tls"] = {{"enabled", true}};
        CHECK(hasError(parse(j), "/node/tls"));
        j = testutil::sampleConfig();
        j["ptp"]["warn_offset_ns"] = 2'000'000;
        CHECK(hasError(parse(j), "/ptp/warn_offset_ns"));
    }
}

TEST_CASE("registry: DNS-SD off by default, deprecated mode mapped (G4)")
{
    auto j = testutil::sampleConfig();
    auto r = parse(j);
    REQUIRE(r.ok());
    CHECK_FALSE(r.config->node.registry.dnsSd);
    CHECK_FALSE(r.config->node.registry.configured());
    CHECK(r.config->node.registry.port == config::defaultRegistrationPort);
    CHECK(r.config->node.registry.effectiveQueryPort() == 3211);

    j["node"]["registry"] = {{"mode", "dns-sd"}};
    r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->node.registry.dnsSd);
    CHECK(r.config->node.registry.configured());

    j["node"]["registry"] = {{"mode", "dns-sd"}, {"dns_sd", false}, {"address", "10.0.0.2"}};
    r = parse(j);
    REQUIRE(r.ok());
    CHECK_FALSE(r.config->node.registry.dnsSd);
    CHECK(r.config->node.registry.configured());

    j["node"]["registry"] = {{"mode", "static"}, {"address", "10.0.0.2"}, {"port", 8235}, {"query_address", "10.0.0.3"}, {"query_port", 8236}};
    r = parse(j);
    REQUIRE(r.ok());
    CHECK_FALSE(r.config->node.registry.dnsSd);
    CHECK(r.config->node.registry.port == 8235);
    CHECK(r.config->node.registry.effectiveQueryAddress() == "10.0.0.3");
    CHECK(r.config->node.registry.effectiveQueryPort() == 8236);
}

TEST_CASE("host address: IPv4 literals only (G5)")
{
    for (auto const* bad : {"0.0.0.0", "127.0.0.1", "169.254.10.1", "239.1.1.1", "255.255.255.255"})
    {
        auto j = testutil::sampleConfig();
        j["node"]["host_address"] = bad;
        CHECK_MESSAGE(hasError(parse(j), "/node/host_address"), bad);
    }
    auto j = testutil::sampleConfig();
    j["node"]["host_address"] = "gateway.example.net";
    CHECK(hasError(parse(j), "/node/host_address"));
    j["node"]["host_address"] = "10.0.0.5";
    REQUIRE(parse(j).ok());
    CHECK(parse(j).config->node.hostAddress == std::string("10.0.0.5"));

    // Deprecated public_address is an alias; a hostname is no longer accepted.
    j = testutil::sampleConfig();
    j["node"]["public_address"] = "10.0.0.6";
    REQUIRE(parse(j).ok());
    CHECK(parse(j).config->node.hostAddress == std::string("10.0.0.6"));
    j["node"]["host_address"] = "10.0.0.7";
    CHECK_FALSE(parse(j).ok());
    j["node"]["host_address"] = "10.0.0.6";
    CHECK(parse(j).ok());
    j = testutil::sampleConfig();
    j["node"]["public_address"] = "proxy.example.net";
    CHECK(hasError(parse(j), "/node/public_address"));

    j = testutil::sampleConfig();
    j["node"]["st2110"] = {{"host_address", "127.0.0.2"}};
    CHECK(hasError(parse(j), "/node/st2110/host_address"));
    j = testutil::sampleConfig();
    j["node"]["management_addresses"] = {"10.0.0.1", "127.0.0.1"};
    CHECK(hasError(parse(j), "/node/management_addresses/1"));

    CHECK(config::isAnnounceableIpv4("192.168.1.10"));
    CHECK_FALSE(config::isAnnounceableIpv4("localhost"));
    CHECK_FALSE(config::isAnnounceableIpv4(""));
}

TEST_CASE("ports: web port, ST 2110 node port (G6)")
{
    auto j = testutil::sampleConfig();
    auto r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->node.effectiveWebPort() == 18080);
    CHECK(r.config->node.st2110HttpPort() == 18081);
    CHECK(r.config->node.st2110Label() == "GW ST 2110");

    j["node"]["web_port"] = 18081; // collides with the ST 2110 default http_port + 1
    CHECK(hasError(parse(j), "/node/http_port"));
    j["node"]["st2110"] = {{"http_port", 18082}};
    CHECK(parse(j).ok());
    j["node"]["st2110"] = {{"http_port", 18080}};
    CHECK(hasError(parse(j), "/node/st2110/http_port"));
    j["node"]["st2110"] = {{"enabled", false}};
    CHECK(parse(j).ok());

    j = testutil::sampleConfig();
    j["node"]["http_port"] = 65535;
    CHECK(hasError(parse(j), "/node/http_port"));
    j["node"]["st2110"] = {{"http_port", 65534}, {"label", "GW 2110"}};
    REQUIRE(parse(j).ok());
    CHECK(parse(j).config->node.st2110Label() == "GW 2110");
}

TEST_CASE("node.seed derives every id (G3)")
{
    auto j = testutil::sampleConfig();
    auto const plain = parse(j);
    REQUIRE(plain.ok());
    CHECK(plain.config->mxlNodeId() == *plain.config->node.id);
    CHECK(plain.config->st2110NodeId() == util::uuidV5(*plain.config->node.id, "st2110-node"));
    CHECK(plain.config->groups[0].video[0].idNamespace == plain.config->groups[0].video[0].uid);
    CHECK_FALSE(plain.config->seedDomainId("main"));

    j["node"]["seed"] = "prod1-gw";
    auto const a = parse(j);
    REQUIRE(a.ok());
    auto const ns = config::seedNamespaceOf("prod1-gw");
    CHECK(a.config->seedNamespace() == ns);
    CHECK(a.config->mxlNodeId() == util::uuidV5(ns, "node"));
    CHECK(a.config->mxlNodeId() != *a.config->node.id); // the seed wins over node.id
    CHECK(a.config->st2110NodeId() == util::uuidV5(ns, "st2110-node"));
    CHECK(a.config->seedDomainId("main") == util::uuidV5(ns, "mxl-domain:main"));
    auto const& v = a.config->groups[0].video[0];
    CHECK(v.idNamespace == util::uuidV5(ns, v.uid.toString()));

    // Same seed and config: same ids, also without node.id; another seed: other ids.
    j["node"].erase("id");
    auto const b = parse(j);
    REQUIRE(b.ok());
    CHECK(b.config->mxlNodeId() == a.config->mxlNodeId());
    CHECK(b.config->groups[0].video[0].idNamespace == v.idNamespace);
    j["node"]["seed"] = "prod2-gw";
    auto const c = parse(j);
    REQUIRE(c.ok());
    CHECK(c.config->mxlNodeId() != a.config->mxlNodeId());
    CHECK(c.config->groups[0].video[0].idNamespace != v.idNamespace);

    j["node"]["seed"] = "";
    CHECK(hasError(parse(j), "/node/seed"));
}

TEST_CASE("node tags")
{
    auto j = testutil::sampleConfig();
    j["node"]["tags"] = {{"urn:x-platform:production", {"prod1"}}, {"empty", json::array()}};
    auto const r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->node.tags.at("urn:x-platform:production") == std::vector<std::string>{"prod1"});
    CHECK(r.config->node.tags.at("empty").empty());
    j["node"]["tags"] = {{"bad", {1}}};
    CHECK(hasError(parse(j), "/node/tags/bad/0"));
}

TEST_CASE("CPU placement from the affinity (Kubernetes cpuset)")
{
    config::Nic nic;
    nic.backend = config::Backend::Dpdk;
    nic.lcoreCount = 2;
    auto p = config::resolveCpuPlacement(nic, {4, 5, 6, 7, 8});
    CHECK(p.lcores == "4-5");
    CHECK(p.appCpus == "6-8");
    CHECK(p.lcoresDerived);
    CHECK(p.appCpusDerived);

    p = config::resolveCpuPlacement(nic, {3});
    CHECK(p.lcores == "3");
    CHECK(p.appCpus.empty());

    nic.lcoreCount = 8;
    p = config::resolveCpuPlacement(nic, {0, 1, 2});
    CHECK(p.lcores == "0-1"); // one CPU stays for the gateway's own threads
    CHECK(p.appCpus == "2");

    nic.lcores = "10-11";
    p = config::resolveCpuPlacement(nic, {8, 9, 10, 11});
    CHECK(p.lcores == "10-11");
    CHECK_FALSE(p.lcoresDerived);
    CHECK(p.appCpus == "8-9");

    nic.appCpus = "12";
    p = config::resolveCpuPlacement(nic, {8, 9, 10, 11});
    CHECK(p.appCpus == "12");
    CHECK_FALSE(p.appCpusDerived);

    config::Nic kernel;
    kernel.backend = config::Backend::Kernel;
    p = config::resolveCpuPlacement(kernel, {0, 1, 2, 3});
    CHECK(p.lcores.empty());
    CHECK(p.appCpus.empty());
}

TEST_CASE("shutdown and cleanup settings (G8)")
{
    auto j = testutil::sampleConfig();
    auto r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->node.shutdownTimeoutS == 10);
    CHECK_FALSE(r.config->mxl.cleanupOnExit);
    j["node"]["shutdown_timeout_s"] = 0;
    CHECK(hasError(parse(j), "/node/shutdown_timeout_s"));
    j["node"]["shutdown_timeout_s"] = 30;
    j["mxl"]["cleanup_on_exit"] = true;
    r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->node.shutdownTimeoutS == 30);
    CHECK(r.config->mxl.cleanupOnExit);
}

TEST_CASE("nic.tx_pacing: auto by default, tsc and rl selectable")
{
    auto j = testutil::sampleConfig();
    auto r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->nic.txPacing == config::TxPacing::Auto);
    j["nic"]["tx_pacing"] = "tsc";
    r = parse(j);
    REQUIRE(r.ok());
    CHECK(r.config->nic.txPacing == config::TxPacing::Tsc);
    CHECK(config::toJson(*r.config)["nic"]["tx_pacing"] == "tsc");
    j["nic"]["tx_pacing"] = "rl";
    CHECK(parse(j).config->nic.txPacing == config::TxPacing::Rl);
    j["nic"]["tx_pacing"] = "fast";
    CHECK(hasError(parse(j), "/nic/tx_pacing"));
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
