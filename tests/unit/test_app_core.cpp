// SPDX-License-Identifier: MIT
// Pure application helpers: group factory, IS-05 target mapping, connection persistence, clock
// supervision and the /metrics export (§9.3, §7.5, §7.6, §5.3, §12.1).
#include <doctest/doctest.h>

#include <regex>
#include <set>
#include <sstream>

#include "app/connection_state.hpp"
#include "app/targets.hpp"
#include "config/group_factory.hpp"
#include "helpers.hpp"
#include "ops/clock_supervisor.hpp"
#include "ops/metrics_export.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

namespace
{
    std::function<util::Uuid()> counterUids()
    {
        auto n = std::make_shared<int>(1000);
        return [n] { return *util::parseUuid(testutil::uid((*n)++)); };
    }

    config::Config parsed(json const& j)
    {
        auto r = config::parseAndValidate(j);
        INFO(config::formatErrors(r.errors));
        REQUIRE(r.ok());
        return *r.config;
    }
}

TEST_CASE("group request parsing reports per-field errors")
{
    config::ValidationErrors errors;
    auto r = config::parseGroupRequest(json{{"label", "CAM 1"}, {"domain", "main"}, {"counts", {{"video", 2}, {"audio", 3}}}}, errors);
    CHECK(errors.empty());
    CHECK(r.direction == "ingest");
    CHECK(r.video == 2);
    CHECK(r.audio == 3);
    CHECK(r.anc == 0); // missing count = 0

    errors.clear();
    config::parseGroupRequest(json{{"direction", "sideways"}, {"redundancy", "yes"}, {"counts", {{"video", 33}, {"audio", -1}, {"anc", "1"}}}}, errors);
    std::set<std::string> pointers;
    for (auto const& e : errors)
    {
        pointers.insert(e.pointer);
    }
    CHECK(pointers == std::set<std::string>{"/label", "/domain", "/direction", "/redundancy", "/counts/video", "/counts/audio", "/counts/anc"});

    errors.clear();
    config::parseGroupRequest(json::array(), errors);
    CHECK(errors.size() == 1);
}

TEST_CASE("makeGroup creates essences with the default profile and passes validation")
{
    config::GroupRequest req;
    req.label = "PGM 2";
    req.direction = "egress";
    req.domain = "main";
    req.video = 2;
    req.audio = 2;
    req.anc = 1;
    auto const g = config::makeGroup(req, json::object(), counterUids());
    CHECK(g["direction"] == "egress");
    CHECK(g["missing_data"] == "black");
    CHECK(g["output_delay_ns"].is_null());
    REQUIRE(g["video"].size() == 2);
    CHECK(g["video"][0]["label"] == "PGM 2 V1");
    CHECK(g["video"][1]["label"] == "PGM 2 V2");
    CHECK(g["video"][0]["rate"] == "50/1");
    CHECK(g["video"][0]["pacing"] == "narrow");
    CHECK(g["audio"][0]["label"] == "PGM 2 A1-8");
    CHECK(g["audio"][1]["label"] == "PGM 2 A9-16");
    CHECK(g["audio"][0]["channels"] == 8);
    CHECK(g["anc"][0]["label"] == "PGM 2 ANC");
    std::set<std::string> uids{g["uid"].get<std::string>()};
    for (auto const* key : {"video", "audio", "anc"})
    {
        for (auto const& e : g[key])
        {
            uids.insert(e["uid"].get<std::string>());
        }
    }
    CHECK(uids.size() == 6);

    auto cfg = testutil::sampleConfig();
    cfg["groups"].push_back(json(g));
    parsed(cfg);

    SUBCASE("explicit essence lists win over counts")
    {
        auto const body = json{{"video", json::array({{{"label", "custom"}, {"width", 3840}, {"height", 2160}, {"rate", "50/1"}}})}};
        auto const c = config::makeGroup(req, body, counterUids());
        REQUIRE(c["video"].size() == 1);
        CHECK(c["video"][0]["label"] == "custom");
        CHECK(c["video"][0].contains("uid"));
    }
}

TEST_CASE("duplicateGroup assigns new uids and keeps the result valid")
{
    auto cfg = testutil::sampleConfig();
    auto const uids = counterUids();
    auto const egress = ojson(cfg["groups"][1]);
    auto const copy = config::duplicateGroup(egress, uids);
    CHECK(copy["label"] == "PGM copy");
    CHECK(copy["uid"] != egress["uid"]);
    CHECK(copy["video"][0]["uid"] != egress["video"][0]["uid"]);
    CHECK_FALSE(copy["video"][0].contains("defaults"));
    cfg["groups"].push_back(json(copy));

    auto const ingest = config::duplicateGroup(ojson(cfg["groups"][0]), uids);
    CHECK(ingest["video"][0]["defaults"] == cfg["groups"][0]["video"][0]["defaults"]);
    cfg["groups"].push_back(json(ingest));
    parsed(cfg);
}

TEST_CASE("IS-05 /active maps to pipeline targets")
{
    auto const rx = app::rtpReceiverTarget(json::parse(R"({
        "master_enable": true,
        "transport_params": [
            {"multicast_ip": "239.1.1.1", "source_ip": "10.1.1.50", "destination_port": 20000, "interface_ip": "10.1.1.21", "rtp_enabled": true},
            {"multicast_ip": null, "interface_ip": "10.2.1.21", "destination_port": 20000, "rtp_enabled": false}
        ]})"));
    CHECK(rx.masterEnable);
    REQUIRE(rx.legs.size() == 2);
    CHECK(rx.legs[0].destination == "239.1.1.1");
    CHECK(rx.legs[0].source == "10.1.1.50");
    CHECK(rx.legs[0].port == 20000);
    CHECK(rx.legs[1].destination == "10.2.1.21"); // unicast reception on the interface
    CHECK_FALSE(rx.legs[1].enabled);

    auto const tx = app::rtpSenderTarget(json::parse(R"({"master_enable": false, "transport_params": [{"destination_ip": "239.10.0.1"}]})"));
    CHECK_FALSE(tx.masterEnable);
    CHECK(tx.legs[0].port == 5004);
    CHECK(tx.legs[0].enabled);

    CHECK(app::mxlSenderTarget(json{{"master_enable", true}}).masterEnable);
    auto const mx = app::mxlReceiverTarget(json::parse(R"({"master_enable": true, "transport_params": [
        {"mxl_domain_id": "7e3a8c52-1d2f-4f0a-9b8e-5c6d7e8f9a01", "mxl_flow_id": "not-a-uuid"}]})"));
    CHECK(mx.masterEnable);
    REQUIRE(mx.domainId);
    CHECK(mx.domainId->toString() == "7e3a8c52-1d2f-4f0a-9b8e-5c6d7e8f9a01");
    CHECK_FALSE(mx.flowId);
    CHECK_FALSE(app::mxlReceiverTarget(json::object()).masterEnable);
}

TEST_CASE("default IS-05 transport parameters from the configuration (§7.6)")
{
    auto const cfg = parsed(testutil::sampleConfig());
    auto const& camVideo = cfg.groups[0].video[0];
    auto const rx = app::defaultRtpReceiverParams(camVideo, true);
    REQUIRE(rx.size() == 2);
    CHECK(rx[0]["multicast_ip"] == "239.1.1.1");
    CHECK(rx[0]["source_ip"].is_null());
    CHECK(rx[1]["multicast_ip"] == "239.2.1.1");
    CHECK(rx[1]["rtp_enabled"] == true);

    auto const& camAudio = cfg.groups[0].audio[0]; // one default leg only
    auto const rxA = app::defaultRtpReceiverParams(camAudio, true);
    CHECK(rxA[1]["rtp_enabled"] == false);
    CHECK(rxA[1]["destination_port"] == "auto");

    auto const& pgm = cfg.groups[1].video[0];
    auto const tx = app::defaultRtpSenderParams(pgm, true, cfg.nic.portPairs[0]);
    REQUIRE(tx.size() == 2);
    CHECK(tx[0]["source_ip"] == "10.1.1.21");
    CHECK(tx[0]["destination_ip"] == "239.10.0.1");
    CHECK(tx[1]["source_ip"] == "10.2.1.21");
    CHECK(tx[1]["destination_ip"] == "auto");
    CHECK(tx[1]["rtp_enabled"] == false);
    CHECK(app::defaultRtpSenderParams(pgm, false, cfg.nic.portPairs[0]).size() == 1);
}

TEST_CASE("connection state persists /active atomically (§7.6)")
{
    testutil::TempDir dir(false);
    auto const path = dir.file("state/connections.json");
    auto const id = *util::parseUuid(testutil::uid(7));
    {
        app::ConnectionState s(path);
        s.load(); // missing file: empty
        CHECK(s.size() == 0);
        CHECK(s.save(id, "receiver", json{{"master_enable", true}}));
        CHECK(s.active(id)->at("master_enable") == true);
    }
    auto const onDisk = json::parse(*util::readFile(path));
    CHECK(onDisk["version"] == 1);
    CHECK(onDisk["resources"][id.toString()]["type"] == "receiver");
    {
        app::ConnectionState s(path);
        s.load();
        REQUIRE(s.active(id));
        CHECK(s.active(id)->at("master_enable") == true);
        s.erase(id);
        CHECK_FALSE(s.active(id));
    }
    app::ConnectionState reloaded(path);
    reloaded.load();
    CHECK(reloaded.size() == 0);

    testutil::writeFile(path, "{ not json");
    app::ConnectionState broken(path);
    broken.load(); // ignored with a warning
    CHECK(broken.size() == 0);
}

TEST_CASE("clock supervisor: min-bracket measurement and 60 s window (§5.3)")
{
    // True offset 1 ms. Reads 0 and 2 have wide, asymmetric host brackets; read 1 is tight.
    std::vector<std::int64_t> const host{0, 100'000, 200'000, 201'000, 300'000, 350'000};
    std::vector<std::int64_t> const ptp{1'090'000, 1'200'500, 1'310'000};
    std::size_t h = 0;
    std::size_t p = 0;
    auto const d = ops::ClockSupervisor::measure([&] { return ptp.at(p++); }, [&] { return host.at(h++); }, 3);
    CHECK(d == 1'000'000);

    ops::ClockSupervisor sup([] { return 0; }, [] { return 0; }, std::chrono::hours(1));
    CHECK_FALSE(sup.latest().valid);
    auto const t0 = std::chrono::steady_clock::now();
    sup.addSample(500, t0);
    sup.addSample(-200, t0 + std::chrono::seconds(10));
    sup.addSample(100, t0 + std::chrono::seconds(20));
    auto s = sup.latest();
    CHECK(s.valid);
    CHECK(s.offsetNs == 100);
    CHECK(s.min60Ns == -200);
    CHECK(s.max60Ns == 500);
    sup.addSample(50, t0 + std::chrono::seconds(75)); // drops samples older than 60 s
    s = sup.latest();
    CHECK(s.min60Ns == 50);
    CHECK(s.max60Ns == 100);
}

namespace
{
    ops::MetricsInput fullMetricsInput()
    {
        ops::MetricsInput in;
        in.ready = true;
        in.restartRequired = false;
        media::BackendStatus b;
        b.backend = "dpdk";
        b.mtlVersion = "26.09";
        b.ptpAvailable = true;
        b.ptpSelectionChanges = 3;
        for (int i = 0; i < 2; ++i)
        {
            media::PortStatus p;
            p.name = i == 0 ? "media-p" : "media-r";
            p.pci = i == 0 ? "0000:31:00.0" : "0000:31:00.1";
            p.mac = "aa:bb:cc:dd:ee:0" + std::to_string(i);
            p.linkUp = true;
            p.linkSpeedMbps = 25000;
            p.bindMode = "pf";
            p.driver = "net_ice";
            p.ddpPackage = "ICE OS Default Package 1.3.36.0";
            p.rxPackets = 10;
            b.ports.push_back(p);
            auto& ptp = b.ptp[i];
            ptp.active = true;
            ptp.locked = true;
            ptp.selected = i == 0;
            ptp.parent = timing::PortIdentity{};
            ptp.announce = timing::AnnounceInfo{};
            ptp.domain = 127;
            ptp.syncCount = 100;
            ptp.gmChanges = 1;
        }
        in.backend = b;
        in.clock = ops::ClockSample{true, 120, -50, 300};

        group::GroupSnapshot ingest;
        ingest.uid = *util::parseUuid(testutil::uid(1));
        ingest.label = "CAM \"1\"";
        ingest.direction = config::Direction::Ingest;
        group::EssenceSnapshot iv;
        iv.groupUid = ingest.uid;
        iv.groupLabel = ingest.label;
        iv.uid = *util::parseUuid(testutil::uid(2));
        iv.label = "CAM 1 V";
        iv.direction = config::Direction::Ingest;
        iv.state.state = group::EssenceState::Running;
        iv.rx.framesComplete = 5;
        iv.rx.legs[0].packets = 100;
        iv.originAgeNs = 1500;
        ingest.essences.push_back(iv);
        in.groups.push_back(ingest);

        group::GroupSnapshot egress;
        egress.uid = *util::parseUuid(testutil::uid(3));
        egress.label = "PGM";
        egress.direction = config::Direction::Egress;
        group::EssenceSnapshot ev;
        ev.groupUid = egress.uid;
        ev.groupLabel = egress.label;
        ev.uid = *util::parseUuid(testutil::uid(4));
        ev.label = "PGM A";
        ev.type = config::EssenceType::Audio;
        ev.direction = config::Direction::Egress;
        ev.state.state = group::EssenceState::WaitingForFlow;
        ev.readLagGrains = 2.0;
        ev.leadNs = 1'000'000;
        ev.reader = group::ReaderInfo{*util::parseUuid(testutil::uid(5)), "/Volumes/mxl/mirror-x", "mirror", *util::parseUuid(testutil::uid(6))};
        egress.essences.push_back(ev);
        in.groups.push_back(egress);

        mxlbridge::ScanResult scan;
        mxlbridge::DomainEntry mirror;
        mirror.kind = mxlbridge::DomainKind::Mirror;
        scan.domains.push_back(mirror);
        in.scan = scan;
        in.domains.push_back({"main", 1024, 4096, 3});
        in.nmosRegistered = {{"mxl", true}, {"st2110", false}};
        in.activations[{"receiver", "urn:x-nmos:transport:mxl", "ok"}] = 2;
        return in;
    }
}

TEST_CASE("metrics export covers every §12.1 family in valid exposition format")
{
    ops::MetricsWriter w;
    ops::exportMetrics(w, fullMetricsInput());
    auto const text = w.render();

    std::set<std::string> families;
    std::set<std::string> typed;
    std::regex const sample(R"re(^([a-zA-Z_:][a-zA-Z0-9_:]*)(\{([a-zA-Z_][a-zA-Z0-9_]*="([^"\\]|\\.)*",?)*\})? -?[0-9.e+]+$)re");
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.rfind("# TYPE ", 0) == 0)
        {
            auto const name = line.substr(7, line.find(' ', 7) - 7);
            CHECK_MESSAGE(typed.insert(name).second, "duplicate TYPE for " << name);
            continue;
        }
        if (line.rfind("# HELP ", 0) == 0 || line.empty())
        {
            continue;
        }
        INFO(line);
        CHECK(std::regex_match(line, sample));
        families.insert(line.substr(0, line.find_first_of("{ ")));
    }

    for (auto const* name : {"mxl_st2110_gateway_build_info",
                             "mxl_st2110_gateway_ready",
                             "mxl_st2110_gateway_restart_required",
                             "mxl_st2110_gateway_ptp_locked",
                             "mxl_st2110_gateway_ptp_selected",
                             "mxl_st2110_gateway_ptp_selection_changes_total",
                             "mxl_st2110_gateway_ptp_info",
                             "mxl_st2110_gateway_ptp_offset_ns",
                             "mxl_st2110_gateway_ptp_path_delay_ns",
                             "mxl_st2110_gateway_ptp_utc_offset_seconds",
                             "mxl_st2110_gateway_ptp_gm_changes_total",
                             "mxl_st2110_gateway_ptp_sync_total",
                             "mxl_st2110_gateway_ptp_errors_total",
                             "mxl_st2110_gateway_clock_mtl_minus_host_tai_ns",
                             "mxl_st2110_gateway_nic_link_up",
                             "mxl_st2110_gateway_nic_link_speed_mbps",
                             "mxl_st2110_gateway_nic_info",
                             "mxl_st2110_gateway_nic_rx_packets_total",
                             "mxl_st2110_gateway_nic_tx_packets_total",
                             "mxl_st2110_gateway_nic_rx_bytes_total",
                             "mxl_st2110_gateway_nic_tx_bytes_total",
                             "mxl_st2110_gateway_nic_rx_errors_total",
                             "mxl_st2110_gateway_nic_rx_missed_total",
                             "mxl_st2110_gateway_essence_state",
                             "mxl_st2110_gateway_rx_frames_total",
                             "mxl_st2110_gateway_rx_leg_packets_total",
                             "mxl_st2110_gateway_rx_packets_total",
                             "mxl_st2110_gateway_rx_leg_seq_lost_total",
                             "mxl_st2110_gateway_ingest_origin_age_ns",
                             "mxl_st2110_gateway_mxl_grains_written_total",
                             "mxl_st2110_gateway_mxl_samples_written_total",
                             "mxl_st2110_gateway_mxl_write_errors_total",
                             "mxl_st2110_gateway_mxl_grains_read_total",
                             "mxl_st2110_gateway_mxl_read_timeouts_total",
                             "mxl_st2110_gateway_mxl_late_reads_total",
                             "mxl_st2110_gateway_mxl_grains_invalid_total",
                             "mxl_st2110_gateway_mxl_flow_not_found_total",
                             "mxl_st2110_gateway_mxl_read_lag_grains",
                             "mxl_st2110_gateway_mxl_reader_info",
                             "mxl_st2110_gateway_mxl_discovered_domains",
                             "mxl_st2110_gateway_tx_frames_total",
                             "mxl_st2110_gateway_tx_late_frames_total",
                             "mxl_st2110_gateway_egress_lead_ns",
                             "mxl_st2110_gateway_nmos_registered",
                             "mxl_st2110_gateway_nmos_activations_total",
                             "mxl_st2110_gateway_mxl_domain_bytes",
                             "mxl_st2110_gateway_mxl_domain_flows"})
    {
        CHECK_MESSAGE(families.count(name) == 1, "missing metric " << name);
    }

    CHECK(text.find(R"(group="CAM \"1\"")") != std::string::npos);
    CHECK(text.find(R"(domain_kind="mirror")") != std::string::npos);
    CHECK(text.find(R"(mxl_st2110_gateway_mxl_discovered_domains{kind="mirror"} 1)") != std::string::npos);

    // One-hot essence state: exactly one series per essence has value 1.
    std::regex const stateLine(R"re(^mxl_st2110_gateway_essence_state\{.*uid="([^"]+)".*\} ([01])$)re");
    std::map<std::string, int> ones;
    std::istringstream again(text);
    while (std::getline(again, line))
    {
        std::smatch m;
        if (std::regex_match(line, m, stateLine))
        {
            ones[m[1]] += m[2] == "1" ? 1 : 0;
        }
    }
    REQUIRE(ones.size() == 2);
    for (auto const& [uid, n] : ones)
    {
        CHECK_MESSAGE(n == 1, uid);
    }
}

TEST_CASE("metrics without a backend still expose the process gauges")
{
    ops::MetricsWriter w;
    ops::MetricsInput in;
    ops::exportMetrics(w, in);
    auto const text = w.render();
    CHECK(text.find("mxl_st2110_gateway_build_info{") != std::string::npos);
    CHECK(text.find("mxl_st2110_gateway_ready 0") != std::string::npos);
    CHECK(text.find("mxl_st2110_gateway_ptp_locked") == std::string::npos);

    // External PTP / kernel backend: no PTP series at all rather than "unlocked".
    auto external = fullMetricsInput();
    external.backend->ptpAvailable = false;
    ops::MetricsWriter we;
    ops::exportMetrics(we, external);
    auto const ext = we.render();
    CHECK(ext.find("mxl_st2110_gateway_ptp_") == std::string::npos);
    CHECK(ext.find("mxl_st2110_gateway_nic_link_up{") != std::string::npos);
    CHECK(std::string(ops::portLabel(0)) == "p");
    CHECK(std::string(ops::portLabel(1)) == "r");
}
