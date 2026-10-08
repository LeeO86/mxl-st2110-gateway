// SPDX-License-Identifier: MIT
// End-to-end pipeline tests: real MXL v1.1.0 + the in-process mock network (§17.2).
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "group/domain_resolver.hpp"
#include "group/group_manager.hpp"
#include "helpers.hpp"
#include "mtl/mock_backend.hpp"
#include "mxlbridge/bootstrap.hpp"
#include "mxlbridge/domaindef.hpp"
#include "mxlbridge/domainscan.hpp"
#include "mxlbridge/pattern.hpp"

using namespace mxlgw;
using namespace std::chrono_literals;

namespace
{
    constexpr std::int64_t historyNs = 160'000'000;

    nlohmann::json videoJson(int uid, std::string const& label, std::string const& mcast, std::string const& rate = "25/1")
    {
        return {{"uid", testutil::uid(uid)},
                {"label", label},
                {"width", 1920},
                {"height", 1080},
                {"rate", rate},
                {"defaults", {{"legs", {{{"multicast", mcast}, {"port", 20000}}}}}}};
    }
    nlohmann::json audioJson(int uid, std::string const& label, std::string const& mcast)
    {
        return {{"uid", testutil::uid(uid)},
                {"label", label},
                {"channels", 2},
                {"bit_depth", 24},
                {"ptime_us", 1000},
                {"block_us", 1000},
                {"defaults", {{"legs", {{{"multicast", mcast}, {"port", 20000}}}}}}};
    }
    nlohmann::json ancJson(int uid, std::string const& label, std::string const& mcast)
    {
        return {{"uid", testutil::uid(uid)}, {"label", label}, {"defaults", {{"legs", {{{"multicast", mcast}, {"port", 20000}}}}}}};
    }

    /// Mock backend whose video senders fail to be created while `fail` is set (like st20p_tx_create).
    struct FailingVideoTxBackend final : media::MediaBackend
    {
        std::unique_ptr<media::MediaBackend> inner;
        std::atomic<bool>& fail;

        FailingVideoTxBackend(std::unique_ptr<media::MediaBackend> backend, std::atomic<bool>& failFlag)
            : inner(std::move(backend))
            , fail(failFlag)
        {}

        std::string name() const override { return inner->name(); }
        std::int64_t ptpTimeNs() const override { return inner->ptpTimeNs(); }
        media::BackendStatus status() const override { return inner->status(); }
        std::unique_ptr<media::VideoRxSession> createVideoRx(media::VideoRxParams const& p, media::VideoRxHandler& h) override
        {
            return inner->createVideoRx(p, h);
        }
        std::unique_ptr<media::VideoTxSession> createVideoTx(media::VideoTxParams const& p) override
        {
            if (fail)
            {
                throw std::runtime_error("st20p_tx_create failed for " + p.name);
            }
            return inner->createVideoTx(p);
        }
        std::unique_ptr<media::AudioRxSession> createAudioRx(media::AudioParams const& p) override { return inner->createAudioRx(p); }
        std::unique_ptr<media::AudioTxSession> createAudioTx(media::AudioParams const& p) override { return inner->createAudioTx(p); }
        std::unique_ptr<media::AncRxSession> createAncRx(media::AncParams const& p) override { return inner->createAncRx(p); }
        std::unique_ptr<media::AncTxSession> createAncTx(media::AncParams const& p) override { return inner->createAncTx(p); }
    };

    /// Every audio block handed to a sender: its transmit time and when it was handed over (host TAI).
    struct AudioSendLog
    {
        struct Send
        {
            std::int64_t transmit = 0;
            std::int64_t handed = 0;
        };
        std::mutex mutex;
        std::map<std::string, std::vector<Send>> byName;

        std::vector<Send> of(std::string const& name)
        {
            std::lock_guard const lock{mutex};
            return byName[name];
        }
    };

    struct RecordingAudioTx final : media::AudioTxSession
    {
        std::unique_ptr<media::AudioTxSession> inner;
        AudioSendLog& log;
        std::string name;

        RecordingAudioTx(std::unique_ptr<media::AudioTxSession> session, AudioSendLog& sendLog, std::string label)
            : inner(std::move(session))
            , log(sendLog)
            , name(std::move(label))
        {}

        std::uint8_t* acquire() override { return inner->acquire(); }
        void send(std::int64_t transmitTai) override
        {
            {
                std::lock_guard const lock{log.mutex};
                log.byName[name].push_back({transmitTai, media::hostTaiNs()});
            }
            inner->send(transmitTai);
        }
        bool updateDestination(std::vector<media::LegAddress> const& legs) override { return inner->updateDestination(legs); }
        media::SessionStats stats() const override { return inner->stats(); }
    };

    /// Mock backend whose audio senders record what they are handed.
    struct RecordingAudioBackend final : media::MediaBackend
    {
        std::unique_ptr<media::MediaBackend> inner;
        AudioSendLog& log;

        RecordingAudioBackend(std::unique_ptr<media::MediaBackend> backend, AudioSendLog& sendLog)
            : inner(std::move(backend))
            , log(sendLog)
        {}

        std::string name() const override { return inner->name(); }
        std::int64_t ptpTimeNs() const override { return inner->ptpTimeNs(); }
        media::BackendStatus status() const override { return inner->status(); }
        std::unique_ptr<media::VideoRxSession> createVideoRx(media::VideoRxParams const& p, media::VideoRxHandler& h) override
        {
            return inner->createVideoRx(p, h);
        }
        std::unique_ptr<media::VideoTxSession> createVideoTx(media::VideoTxParams const& p) override { return inner->createVideoTx(p); }
        std::unique_ptr<media::AudioRxSession> createAudioRx(media::AudioParams const& p) override { return inner->createAudioRx(p); }
        std::unique_ptr<media::AudioTxSession> createAudioTx(media::AudioParams const& p) override
        {
            return std::make_unique<RecordingAudioTx>(inner->createAudioTx(p), log, p.name);
        }
        std::unique_ptr<media::AncRxSession> createAncRx(media::AncParams const& p) override { return inner->createAncRx(p); }
        std::unique_ptr<media::AncTxSession> createAncTx(media::AncParams const& p) override { return inner->createAncTx(p); }
    };

    using BackendWrap = std::function<std::unique_ptr<media::MediaBackend>(std::unique_ptr<media::MediaBackend>)>;

    /// Gateway core (domains, resolver, mock backend, groups) without NMOS.
    struct Harness
    {
        testutil::TempDir root;
        config::Config config;
        mxlbridge::InstanceRegistry instances;
        std::unique_ptr<mxlbridge::DomainDirectory> directory;
        std::unique_ptr<group::DomainResolver> resolver;
        std::unique_ptr<media::MediaBackend> backend;
        std::unique_ptr<group::GroupManager> groups;
        group::DomainRuntime main;

        explicit Harness(nlohmann::json groupsJson, BackendWrap const& wrap = {})
        {
            REQUIRE(root.tmpfs());
            auto j = testutil::sampleConfig(root.file("main"));
            j["mxl"]["scan_path"] = root.path();
            j["mxl"]["domains"][0]["history_duration_ns"] = historyNs;
            j["nic"]["port_pairs"][0].erase("redundant");
            j["groups"] = std::move(groupsJson);
            auto parsed = config::parseAndValidate(j);
            INFO(config::formatErrors(parsed.errors));
            REQUIRE(parsed.ok());
            config = *parsed.config;
            auto const boot = mxlbridge::bootstrapDomain(config.mxl.domains[0]);
            main = {"main", boot.path, boot.id, boot.label, instances.pin(boot.path)};
            directory = std::make_unique<mxlbridge::DomainDirectory>(config.mxl.scanPath, std::vector<mxlbridge::ConfiguredDomainRef>{{"main", boot.path}});
            directory->rescan();
            resolver = std::make_unique<group::DomainResolver>(*directory, instances);
            backend = media::createMockBackend(config);
            if (wrap)
            {
                backend = wrap(std::move(backend));
            }
            groups = std::make_unique<group::GroupManager>(*config.node.id, *backend, *resolver, std::vector<group::DomainRuntime>{main}, "");
            groups->apply(config);
        }

        ~Harness()
        {
            groups.reset();
            backend.reset();
            resolver.reset();
        }

        group::EssenceSnapshot essence(int uid) const
        {
            auto const id = *util::parseUuid(testutil::uid(uid));
            for (auto const& g : groups->snapshot())
            {
                for (auto const& e : g.essences)
                {
                    if (e.uid == id)
                    {
                        return e;
                    }
                }
            }
            FAIL("essence not found");
            return {};
        }

        static group::RtpTarget rtp(std::string const& mcast) { return {true, {media::LegAddress{mcast, "", 20000, true}}}; }
    };

    util::Uuid uuidOf(int n)
    {
        return *util::parseUuid(testutil::uid(n));
    }

    template <typename Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds timeout)
    {
        auto const until = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < until)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return pred();
    }
}

TEST_CASE("pipeline: MXL -> egress -> ST 2110 (mock) -> ingest -> MXL keeps content, cadence and lip-sync")
{
    nlohmann::json groups = nlohmann::json::array();
    groups.push_back({{"uid", testutil::uid(200)},
                      {"label", "PGM"},
                      {"direction", "egress"},
                      {"domain", "main"},
                      {"video", {videoJson(201, "PGM V", "239.10.0.1")}},
                      {"audio", {audioJson(202, "PGM A", "239.10.0.2")}},
                      {"anc", {ancJson(203, "PGM ANC", "239.10.0.3")}}});
    groups.push_back({{"uid", testutil::uid(300)},
                      {"label", "LOOP"},
                      {"direction", "ingest"},
                      {"domain", "main"},
                      {"video", {videoJson(301, "LOOP V", "239.10.0.1")}},
                      {"audio", {audioJson(302, "LOOP A", "239.10.0.2")}},
                      {"anc", {ancJson(303, "LOOP ANC", "239.10.0.3")}}});
    Harness h(groups);

    mxlbridge::PatternConfig pc;
    pc.videoFlow = util::uuidV4();
    pc.video.rate = {25, 1};
    pc.audioFlow = util::uuidV4();
    pc.audio.channels = 2;
    pc.ancFlow = util::uuidV4();
    pc.anc.rate = {25, 1};
    mxlbridge::PatternWriter source(h.main.instance, pc);
    source.start();

    h.groups->setMxlReceiver(uuidOf(201), {true, h.main.id, pc.videoFlow});
    h.groups->setMxlReceiver(uuidOf(202), {true, h.main.id, pc.audioFlow});
    h.groups->setMxlReceiver(uuidOf(203), {true, h.main.id, pc.ancFlow});
    h.groups->setRtpSender(uuidOf(201), Harness::rtp("239.10.0.1"));
    h.groups->setRtpSender(uuidOf(202), Harness::rtp("239.10.0.2"));
    h.groups->setRtpSender(uuidOf(203), Harness::rtp("239.10.0.3"));
    for (int uid : {301, 302, 303})
    {
        h.groups->setRtpReceiver(uuidOf(uid), Harness::rtp(uid == 301 ? "239.10.0.1" : uid == 302 ? "239.10.0.2" : "239.10.0.3"));
        h.groups->setMxlSender(uuidOf(uid), {true});
    }

    REQUIRE(
        waitFor([&] { return h.essence(201).state.state == group::EssenceState::Running && h.essence(301).state.state == group::EssenceState::Running; }, 5s));
    std::this_thread::sleep_for(500ms);

    mxlbridge::VerifyConfig vc;
    vc.videoFlow = h.essence(301).flowId;
    vc.video.rate = {25, 1};
    vc.audioFlow = h.essence(302).flowId;
    vc.audio.channels = 2;
    vc.ancFlow = h.essence(303).flowId;
    vc.anc.rate = {25, 1};
    vc.duration = 2s;
    auto const report = mxlbridge::verifyFlows(h.main.instance, vc);
    INFO(report.toJson().dump(2));
    CHECK(report.ok());
    // Q1 "transmit time" model: the ingest index is the source index + output_delay (2 grains at 25p).
    CHECK(report.videoOffsetGrains == 2);
    CHECK(report.ancOffsetGrains == 2);
    CHECK(report.audioOffsetSamples == 2 * 1920);
    CHECK(report.avMisalignmentSamples == 0);

    auto const egress = h.essence(201);
    CHECK(egress.reader);
    CHECK(egress.reader->domainKind == "configured");
    CHECK(egress.readTimeouts <= 2);
    CHECK(egress.grainsRead > 50);
    auto const ingest = h.essence(301);
    CHECK(ingest.grainsWritten > 50);
    CHECK(ingest.writeErrors == 0);
    CHECK(ingest.originAgeNs.has_value());
    CHECK(h.essence(302).samplesWritten > 48 * 2000);
    source.stop();
}

TEST_CASE("pipeline: late flow in a mirror domain — waiting_for_flow, running, no_signal, resumes (§5.8, §17.3)")
{
    nlohmann::json groups = nlohmann::json::array();
    groups.push_back({{"uid", testutil::uid(400)},
                      {"label", "LATE"},
                      {"direction", "egress"},
                      {"domain", "main"},
                      {"video", {videoJson(401, "LATE V", "239.20.0.1")}},
                      {"audio", {audioJson(402, "LATE A", "239.20.0.2")}}});
    Harness h(groups);

    auto const mirrorId = util::uuidV4();
    auto const mirrorPath = h.root.file("mirror-" + mirrorId.toString());
    testutil::writeFile(mirrorPath + "/domain_def.json",
                        nlohmann::json{{"id", mirrorId.toString()},
                                       {"label", "remote main"},
                                       {"description", ""},
                                       {"tags", nlohmann::json::object()},
                                       {"x-mxl-fabrics-agent", {{"mirror", true}, {"source_host_id", "host-a"}, {"owner_host_id", "host-b"}}}}
                            .dump());
    testutil::writeFile(mirrorPath + "/options.json", nlohmann::json{{mxlbridge::historyDurationOption, historyNs}}.dump());

    auto const videoFlow = util::uuidV4();
    auto const audioFlow = util::uuidV4();
    h.groups->setRtpSender(uuidOf(401), Harness::rtp("239.20.0.1"));
    h.groups->setRtpSender(uuidOf(402), Harness::rtp("239.20.0.2"));
    h.groups->setMxlReceiver(uuidOf(401), {true, mirrorId, videoFlow});
    h.groups->setMxlReceiver(uuidOf(402), {true, mirrorId, audioFlow});

    REQUIRE(waitFor([&] { return h.essence(401).flowNotFound >= 2; }, 3s));
    CHECK(h.essence(401).state.state == group::EssenceState::WaitingForFlow);
    CHECK(h.essence(401).state.reason == "flow_not_found");
    auto const txBefore = h.essence(401).tx.packets;

    // The "agent" creates the mirror flows: the receivers start without any further request.
    auto mirror = std::make_shared<mxlbridge::Instance>(mirrorPath);
    mxlbridge::PatternConfig pc;
    pc.videoFlow = videoFlow;
    pc.video.rate = {25, 1};
    pc.audioFlow = audioFlow;
    pc.audio.channels = 2;
    auto writer = std::make_unique<mxlbridge::PatternWriter>(mirror, pc);
    writer->start();
    REQUIRE(
        waitFor([&] { return h.essence(401).state.state == group::EssenceState::Running && h.essence(402).state.state == group::EssenceState::Running; }, 8s));
    REQUIRE(waitFor([&] { return h.essence(401).reader.has_value(); }, 1s));
    CHECK(h.essence(401).reader->domainKind == "mirror");
    CHECK(h.essence(401).reader->domainId == mirrorId);
    // Replacement data kept the ST 2110 output going while the flow was missing.
    CHECK(h.essence(401).tx.packets > txBefore);

    // Writer stops (MXL deletes the flow on a clean stop): no_signal, not error; retries continue and the
    // ST 2110 output keeps going with replacement data.
    writer->stop();
    writer.reset();
    REQUIRE(waitFor([&] { return h.essence(401).state.state == group::EssenceState::NoSignal; }, 3s));
    REQUIRE(waitFor([&] { return h.essence(402).state.state == group::EssenceState::NoSignal; }, 3s));
    CHECK(h.essence(401).state.reason == "flow_removed");
    auto const retries = h.essence(401).flowNotFound;
    auto const sent = h.essence(401).tx.packets;
    REQUIRE(waitFor([&] { return h.essence(401).flowNotFound > retries && h.essence(401).tx.packets > sent; }, 4s));

    // Writer restarts: reading resumes automatically.
    writer = std::make_unique<mxlbridge::PatternWriter>(mirror, pc);
    writer->start();
    REQUIRE(
        waitFor([&] { return h.essence(401).state.state == group::EssenceState::Running && h.essence(402).state.state == group::EssenceState::Running; }, 8s));
    writer->stop();

    // The gateway never wrote into the mirror domain (only the "agent" created flows there).
    std::set<std::string> const allowed{"domain_def.json", "options.json", videoFlow.toString() + ".mxl-flow", audioFlow.toString() + ".mxl-flow"};
    for (auto const& e : std::filesystem::directory_iterator(mirrorPath))
    {
        CHECK(allowed.count(e.path().filename().string()) == 1);
    }
    CHECK(std::filesystem::exists(mirrorPath + "/domain_def.json"));

    // master_enable=false stops the retries.
    h.groups->setMxlReceiver(uuidOf(401), {false, mirrorId, videoFlow});
    REQUIRE(waitFor([&] { return h.essence(401).state.state == group::EssenceState::Idle; }, 2s));
}

TEST_CASE("pipeline: unknown domain is accepted and waited for; format mismatch is an error but keeps retrying")
{
    nlohmann::json groups = nlohmann::json::array();
    groups.push_back(
        {{"uid", testutil::uid(500)}, {"label", "WAIT"}, {"direction", "egress"}, {"domain", "main"}, {"video", {videoJson(501, "WAIT V", "239.30.0.1")}}});
    Harness h(groups);
    auto const unknown = util::uuidV4();
    h.groups->setMxlReceiver(uuidOf(501), {true, unknown, util::uuidV4()});
    REQUIRE(waitFor([&] { return h.essence(501).state.reason == "domain_not_found"; }, 3s));
    CHECK(h.essence(501).state.state == group::EssenceState::WaitingForFlow);

    // A flow with the wrong format in the configured domain.
    mxlbridge::PatternConfig pc;
    pc.videoFlow = util::uuidV4();
    pc.video.rate = {50, 1};
    mxlbridge::PatternWriter wrong(h.main.instance, pc);
    wrong.start();
    std::this_thread::sleep_for(100ms);
    h.groups->setMxlReceiver(uuidOf(501), {true, h.main.id, pc.videoFlow});
    REQUIRE(waitFor([&] { return h.essence(501).state.state == group::EssenceState::Error; }, 3s));
    CHECK(h.essence(501).state.reason == "format_mismatch");
    auto const attempts = h.essence(501).flowNotFound;
    REQUIRE(waitFor([&] { return h.essence(501).flowNotFound > attempts; }, 3s));
    wrong.stop();
}

TEST_CASE("pipeline: a video sender that cannot be created is error/egress_sender_failed, not running, and is retried")
{
    nlohmann::json groups = nlohmann::json::array();
    groups.push_back(
        {{"uid", testutil::uid(520)}, {"label", "TXFAIL"}, {"direction", "egress"}, {"domain", "main"}, {"video", {videoJson(521, "TXFAIL V", "239.30.0.5")}}});
    std::atomic<bool> fail{true};
    Harness h(groups, [&](std::unique_ptr<media::MediaBackend> inner) { return std::make_unique<FailingVideoTxBackend>(std::move(inner), fail); });

    mxlbridge::PatternConfig pc;
    pc.videoFlow = util::uuidV4();
    pc.video.rate = {25, 1};
    mxlbridge::PatternWriter source(h.main.instance, pc);
    source.start();
    h.groups->setMxlReceiver(uuidOf(521), {true, h.main.id, pc.videoFlow});
    h.groups->setRtpSender(uuidOf(521), Harness::rtp("239.30.0.5"));

    // The MXL side reads grains, but the essence shows the failed sender.
    REQUIRE(waitFor([&] { return h.essence(521).grainsRead > 10; }, 5s));
    CHECK(h.essence(521).state.state == group::EssenceState::Error);
    CHECK(h.essence(521).state.reason == "egress_sender_failed");

    // The sender is created at a later retry and the essence runs.
    fail = false;
    REQUIRE(waitFor([&] { return h.essence(521).state.state == group::EssenceState::Running; }, 5s));
    CHECK(h.essence(521).state.reason.empty());

    // Disabling the sender clears the error as well.
    fail = true;
    h.groups->setRtpSender(uuidOf(521), {false, {}});
    std::this_thread::sleep_for(200ms);
    CHECK(h.essence(521).state.state == group::EssenceState::Running);
    source.stop();
}

TEST_CASE("pipeline: egress audio sends one block per packet time ahead of its transmit time, whatever the sources do (§5.7)")
{
    // Like the platform's TX groups: 1080p50 (output delay 40 ms) and four audio essences, ptime 1 ms.
    nlohmann::json groups = nlohmann::json::array();
    groups.push_back({{"uid", testutil::uid(600)},
                      {"label", "PACE"},
                      {"direction", "egress"},
                      {"domain", "main"},
                      {"video", {videoJson(601, "PACE V", "239.40.0.1", "50/1")}},
                      {"audio",
                       {audioJson(602, "PACE A1", "239.40.0.2"), audioJson(603, "PACE A2", "239.40.0.3"), audioJson(604, "PACE A3", "239.40.0.4"),
                        audioJson(605, "PACE A4", "239.40.0.5")}}});
    AudioSendLog sends;
    Harness h(groups, [&](std::unique_ptr<media::MediaBackend> inner) { return std::make_unique<RecordingAudioBackend>(std::move(inner), sends); });

    // A1: on time, 1 ms blocks (with the video). A2: 20 ms chunks committed 15 ms late (a frame-based writer
    // behind real time). A3: 60 ms late, always after its give-up time. A4: no source (silence).
    struct Source
    {
        mxlbridge::PatternConfig config;
        std::unique_ptr<mxlbridge::PatternWriter> writer;
    };
    auto source = [&](int blockUs, std::int64_t delayNs, bool video)
    {
        Source s;
        if (video)
        {
            s.config.videoFlow = util::uuidV4();
            s.config.video.rate = {50, 1};
        }
        s.config.audioFlow = util::uuidV4();
        s.config.audio.channels = 2;
        s.config.audio.blockUs = blockUs;
        s.config.audioDelayNs = delayNs;
        s.writer = std::make_unique<mxlbridge::PatternWriter>(h.main.instance, s.config);
        s.writer->start();
        return s;
    };
    auto const prompt = source(1000, 0, true);
    auto const chunked = source(20'000, 15'000'000, false);
    auto const late = source(1000, 60'000'000, false);
    h.groups->setMxlReceiver(uuidOf(601), {true, h.main.id, prompt.config.videoFlow});
    h.groups->setMxlReceiver(uuidOf(602), {true, h.main.id, prompt.config.audioFlow});
    h.groups->setMxlReceiver(uuidOf(603), {true, h.main.id, chunked.config.audioFlow});
    h.groups->setMxlReceiver(uuidOf(604), {true, h.main.id, late.config.audioFlow});
    for (int uid = 601; uid <= 605; ++uid)
    {
        h.groups->setRtpSender(uuidOf(uid), Harness::rtp("239.40.0." + std::to_string(uid - 600)));
    }
    REQUIRE(
        waitFor([&] { return h.essence(602).state.state == group::EssenceState::Running && h.essence(603).state.state == group::EssenceState::Running; }, 5s));
    std::map<int, group::EssenceSnapshot> const before{{602, h.essence(602)}, {603, h.essence(603)}, {604, h.essence(604)}};
    auto const from = media::hostTaiNs();
    std::this_thread::sleep_for(2s);
    auto const to = media::hostTaiNs();

    for (std::string const label : {"PACE A1", "PACE A2", "PACE A3", "PACE A4"})
    {
        INFO(label);
        std::vector<AudioSendLog::Send> window;
        for (auto const& s : sends.of(label))
        {
            if (s.handed >= from && s.handed < to)
            {
                window.push_back(s);
            }
        }
        REQUIRE(window.size() > 1500);
        // One block per packet time on the clock: consecutive transmit times, none twice, (almost) none skipped.
        std::int64_t gaps = 0;
        for (std::size_t i = 1; i < window.size(); ++i)
        {
            auto const step = window[i].transmit - window[i - 1].transmit;
            CHECK(step > 0);
            gaps += step / 1'000'000 - 1;
        }
        CHECK(gaps <= static_cast<std::int64_t>(window.size() / 100));
        CHECK(static_cast<double>(window.size()) == doctest::Approx(static_cast<double>(to - from) / 1e6).epsilon(0.03));
        // Every block reaches its sender before its transmit time (a shared CI runner may preempt the worker for
        // a moment: up to 0.5 % may miss); a late source (A3) only costs its own lead.
        std::vector<std::int64_t> leads;
        for (auto const& s : window)
        {
            leads.push_back(s.transmit - s.handed);
        }
        std::sort(leads.begin(), leads.end());
        CHECK(std::count_if(leads.begin(), leads.end(), [](std::int64_t lead) { return lead <= 0; }) <= static_cast<std::ptrdiff_t>(leads.size() / 200));
        // A block is handed over when its data is due (A1, A4: ~38 ms ahead), arrives (A2: 20 ms chunks 35 ms
        // after their start, 3.5..22.5 ms ahead) or is given up (A3: 2 ms ahead), never when another essence's is.
        auto const p1 = leads[leads.size() / 100];
        std::map<std::string, std::int64_t> const minimum{{"PACE A1", 20'000'000}, {"PACE A2", 2'000'000}, {"PACE A3", 500'000}, {"PACE A4", 20'000'000}};
        CHECK(p1 >= minimum.at(label));
    }
    auto delta = [&](int uid, std::uint64_t group::EssenceSnapshot::*field) { return h.essence(uid).*field - before.at(uid).*field; };
    // The chunked late source arrives before its give-up time and is read; the 60 ms late one never is.
    CHECK(delta(603, &group::EssenceSnapshot::grainsRead) > 1800);
    CHECK(delta(603, &group::EssenceSnapshot::readTimeouts) < 40);
    CHECK(delta(604, &group::EssenceSnapshot::readTimeouts) > 1800);
    CHECK(delta(602, &group::EssenceSnapshot::txDropped) == 0);
    for (auto const* s : {&prompt, &chunked, &late})
    {
        s->writer->stop();
    }
}
