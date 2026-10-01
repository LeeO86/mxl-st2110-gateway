// SPDX-License-Identifier: MIT
// Tests against the real MXL v1.1.0 library (Phase 2, VERIFY items R2/R3).
#include <doctest/doctest.h>

#include <cstring>
#include <thread>

#include <mxl/flow.h>
#include <mxl/time.h>

#include "codec/v210.hpp"
#include "helpers.hpp"
#include "mxlbridge/bootstrap.hpp"
#include "mxlbridge/domainscan.hpp"
#include "mxlbridge/flowdef.hpp"
#include "mxlbridge/flowsync.hpp"
#include "mxlbridge/instance.hpp"
#include "mxlbridge/pattern.hpp"
#include "mxlbridge/reader.hpp"
#include "mxlbridge/writer.hpp"
#include "timing/rtpclock.hpp"

using namespace mxlgw;

namespace
{
    /// A tmpfs domain with a short history so 1080p flows fit the build's tmpfs.
    struct Domain
    {
        testutil::TempDir dir;
        std::shared_ptr<mxlbridge::Instance> instance;

        explicit Domain(std::int64_t historyNs = 200'000'000)
        {
            REQUIRE(dir.tmpfs());
            config::Domain d;
            d.name = "test";
            d.path = dir.file("domain");
            d.historyDurationNs = historyNs;
            mxlbridge::bootstrapDomain(d);
            instance = std::make_shared<mxlbridge::Instance>(d.path);
        }
        std::string path() const { return dir.file("domain"); }
    };

    mxlbridge::FlowIdentity ident(util::Uuid const& id)
    {
        mxlbridge::FlowIdentity f;
        f.flowId = id;
        f.sourceId = util::uuidV4();
        f.deviceId = util::uuidV4();
        f.label = "test";
        f.groupHint = "test:Video 1";
        f.version = "1:0";
        return f;
    }

    config::VideoFormat p25()
    {
        config::VideoFormat f;
        f.rate = {25, 1};
        return f;
    }
}

TEST_CASE("mxl: library version is the pinned v1.1 line")
{
    CHECK(mxlbridge::mxlVersionString().rfind("1.1", 0) == 0);
}

TEST_CASE("mxl: video grain round trip; existing flows are compared, not trusted (§8.4)")
{
    Domain dom;
    auto const id = util::uuidV4();
    auto const def = mxlbridge::videoFlowDef(ident(id), p25());
    mxlbridge::GrainWriter writer(dom.instance, def, mxlbridge::writerOptions(1080, 1080));
    CHECK(writer.created());
    CHECK(writer.sliceSize() == 5120); // v210 stride for 1920 px
    CHECK(writer.grainCount() >= 2);

    std::vector<std::uint8_t> frame(p25().grainBytes());
    codec::v210ColourBars(frame.data(), 1920, 1080, 5120, 1234);
    auto const index = mxlGetCurrentIndex(&writer.configInfo().common.grainRate);
    REQUIRE(writer.write(index, frame) == MXL_STATUS_OK);

    mxlStatus status = MXL_ERR_UNKNOWN;
    auto reader = mxlbridge::GrainReader::open(dom.instance, id.toString(), status);
    REQUIRE(reader);
    CHECK(reader->flowDef()["id"] == id.toString());
    mxlGrainInfo info{};
    std::uint8_t const* payload = nullptr;
    REQUIRE(reader->get(index, 0, info, payload) == MXL_STATUS_OK);
    CHECK(info.validSlices == info.totalSlices);
    CHECK(codec::v210ReadCounter(payload, 1920, 5120) == 1234u);
    CHECK(codec::v210CheckBars(payload, 1920, 1080, 5120));

    // A second writer object opens the existing flow: created=false and the definition is available.
    auto other = def;
    other["frame_width"] = 3840;
    mxlbridge::GrainWriter second(dom.instance, other, {});
    CHECK_FALSE(second.created());
    CHECK(mxlbridge::compareVideo(second.existingFlowDef(), p25()).empty());
    config::VideoFormat uhd = p25();
    uhd.width = 3840;
    uhd.height = 2160;
    CHECK_FALSE(mxlbridge::compareVideo(second.existingFlowDef(), uhd).empty());
}

TEST_CASE("mxl: discrete writer rejects index <= last committed and invalidates skipped grains (R2)")
{
    Domain dom;
    auto const id = util::uuidV4();
    mxlbridge::GrainWriter writer(dom.instance, mxlbridge::ancFlowDef(ident(id), config::AncFormat{}), {});
    std::vector<std::uint8_t> body(16, 0);
    auto const base = mxlGetCurrentIndex(&writer.configInfo().common.grainRate);
    REQUIRE(writer.write(base, body) == MXL_STATUS_OK);
    // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/internal/src/PosixDiscreteFlowWriter.cpp:91-94 — index <= last committed is MXL_ERR_INVALID_ARG.
    CHECK(writer.write(base, body) == MXL_ERR_INVALID_ARG);
    CHECK(writer.write(base - 1, body) == MXL_ERR_INVALID_ARG);
    REQUIRE(writer.write(base + 3, body) == MXL_STATUS_OK);

    mxlStatus status = MXL_ERR_UNKNOWN;
    auto reader = mxlbridge::GrainReader::open(dom.instance, id.toString(), status);
    REQUIRE(reader);
    mxlGrainInfo info{};
    std::uint8_t const* payload = nullptr;
    // VERIFIED: PosixDiscreteFlowWriter.cpp:96-113 — a forward jump marks the skipped grains invalid.
    REQUIRE(reader->getNonBlocking(base + 1, info, payload) == MXL_STATUS_OK);
    CHECK((info.flags & MXL_GRAIN_FLAG_INVALID) != 0);
    REQUIRE(reader->getNonBlocking(base + 3, info, payload) == MXL_STATUS_OK);
    CHECK((info.flags & MXL_GRAIN_FLAG_INVALID) == 0);
    CHECK(info.totalSlices == 4096); // data grains: 4096 one-byte slices
}

TEST_CASE("mxl: audio samples are addressed by their END index")
{
    Domain dom;
    auto const id = util::uuidV4();
    config::AudioFormat af;
    af.channels = 4;
    mxlbridge::SampleWriter writer(dom.instance, mxlbridge::audioFlowDef(ident(id), af), mxlbridge::writerOptions(48, 48));
    CHECK(writer.channelCount() == 4);
    CHECK(writer.maxWriteLength() == writer.bufferLength() / 2);

    auto const now = static_cast<std::uint64_t>(timing::ticksAt(static_cast<timing::TaiNs>(mxlGetTime()), 48000));
    auto const end = (now / 48) * 48;
    mxlMutableWrappedMultiBufferSlice slices{};
    REQUIRE(writer.open(end, 48, slices) == MXL_STATUS_OK);
    CHECK(slices.count == 4);
    for (std::size_t c = 0; c < slices.count; ++c)
    {
        std::size_t k = 0;
        for (auto const& frag : slices.base.fragments)
        {
            auto* p = reinterpret_cast<float*>(static_cast<std::uint8_t*>(frag.pointer) + c * slices.stride);
            for (std::size_t j = 0; j < frag.size / sizeof(float); ++j, ++k)
            {
                p[j] = static_cast<float>(c) + static_cast<float>(k) / 100.0f;
            }
        }
    }
    REQUIRE(writer.commit() == MXL_STATUS_OK);
    // Overlapping ranges are rejected (continuous writer).
    mxlMutableWrappedMultiBufferSlice again{};
    CHECK(writer.open(end - 10, 48, again) != MXL_STATUS_OK);

    mxlStatus status = MXL_ERR_UNKNOWN;
    auto reader = mxlbridge::SampleReader::open(dom.instance, id.toString(), status);
    REQUIRE(reader);
    mxlWrappedMultiBufferSlice read{};
    REQUIRE(reader->getNonBlocking(end, 48, read) == MXL_STATUS_OK);
    auto const* ch2 = reinterpret_cast<float const*>(static_cast<std::uint8_t const*>(read.base.fragments[0].pointer) + 2 * read.stride);
    CHECK(ch2[0] == doctest::Approx(2.0));
    CHECK(ch2[5] == doctest::Approx(2.05));
    // A grain reader cannot open an audio flow (§5.8 format check).
    CHECK_FALSE(mxlbridge::GrainReader::open(dom.instance, id.toString(), status));
    CHECK(status == MXL_ERR_INVALID_FLOW_READER);
}

TEST_CASE("mxl: reader status codes used by the egress state machine (§5.8)")
{
    Domain dom;
    auto const id = util::uuidV4();
    mxlStatus status = MXL_STATUS_OK;
    CHECK_FALSE(mxlbridge::GrainReader::open(dom.instance, id.toString(), status));
    CHECK(status == MXL_ERR_FLOW_NOT_FOUND);

    auto const def = mxlbridge::ancFlowDef(ident(id), config::AncFormat{});
    auto writer = std::make_unique<mxlbridge::GrainWriter>(dom.instance, def, std::string{});
    auto reader = mxlbridge::GrainReader::open(dom.instance, id.toString(), status);
    REQUIRE(reader);
    auto const rate = writer->grainRate();
    auto const idx = mxlGetCurrentIndex(&rate);
    mxlGrainInfo info{};
    std::uint8_t const* payload = nullptr;
    CHECK(reader->get(idx + 100, 10'000'000, info, payload) == MXL_ERR_OUT_OF_RANGE_TOO_EARLY); // timeout
    std::vector<std::uint8_t> body(8, 0);
    REQUIRE(writer->write(idx, body) == MXL_STATUS_OK);
    REQUIRE(writer->write(idx + writer->grainCount() + 5, body) == MXL_STATUS_OK);
    CHECK(reader->getNonBlocking(idx, info, payload) == MXL_ERR_OUT_OF_RANGE_TOO_LATE);

    // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/internal/src/Instance.cpp:135 + SharedMemory.cpp:56 — readers open flows
    // read-only without a lock, so releasing the last writer deletes the flow even while readers exist.
    writer.reset();
    CHECK_FALSE(std::filesystem::exists(mxlbridge::flowDirPath(dom.path(), id)));
    CHECK(reader->get(idx + 1000, 0, info, payload) == MXL_ERR_FLOW_INVALID);

    // Re-created by a new writer: the old reader stays invalid (new inode), a new reader works.
    auto other = std::make_shared<mxlbridge::Instance>(dom.path());
    mxlbridge::GrainWriter recreated(other, def, {});
    CHECK(recreated.created());
    CHECK(reader->get(idx + 1000, 0, info, payload) == MXL_ERR_FLOW_INVALID);
    auto fresh = mxlbridge::GrainReader::open(dom.instance, id.toString(), status);
    CHECK(fresh);
}

TEST_CASE("mxl: one sync group spans readers of different domains (R3)")
{
    Domain a;
    Domain b;
    auto const va = util::uuidV4();
    auto const ab = util::uuidV4();
    mxlbridge::GrainWriter video(a.instance, mxlbridge::ancFlowDef(ident(va), config::AncFormat{}), {});
    config::AudioFormat af;
    mxlbridge::SampleWriter audio(b.instance, mxlbridge::audioFlowDef(ident(ab), af), mxlbridge::writerOptions(48, 48));

    mxlStatus status = MXL_ERR_UNKNOWN;
    auto rv = mxlbridge::GrainReader::open(a.instance, va.toString(), status);
    auto ra = mxlbridge::SampleReader::open(b.instance, ab.toString(), status);
    REQUIRE(rv);
    REQUIRE(ra);
    mxlbridge::SyncGroup sync(a.instance);
    REQUIRE(sync.add(*rv) == MXL_STATUS_OK);
    REQUIRE(sync.add(*ra) == MXL_STATUS_OK);

    util::Rational const rate{50, 1};
    auto const idx = timing::timestampToIndex(rate, static_cast<timing::TaiNs>(mxlGetTime())) + 2;
    auto const t = static_cast<std::uint64_t>(timing::indexToTimestamp(rate, idx));
    CHECK(sync.waitForDataAt(t, 5'000'000) == MXL_ERR_OUT_OF_RANGE_TOO_EARLY);

    std::vector<std::uint8_t> body(8, 0);
    REQUIRE(video.write(idx, body) == MXL_STATUS_OK);
    auto const sampleAtT = timing::timestampToIndex({48000, 1}, static_cast<timing::TaiNs>(t));
    mxlMutableWrappedMultiBufferSlice slices{};
    REQUIRE(audio.open(sampleAtT + 48, 48, slices) == MXL_STATUS_OK);
    REQUIRE(audio.commit() == MXL_STATUS_OK);
    CHECK(sync.waitForDataAt(t, 50'000'000) == MXL_STATUS_OK);
    CHECK(sync.remove(*ra) == MXL_STATUS_OK);
    CHECK(sync.remove(*rv) == MXL_STATUS_OK);
}

TEST_CASE("mxl: own-flow GC keeps flows with a live writer (Q9)")
{
    Domain dom;
    auto const live = util::uuidV4();
    auto const stale = util::uuidV4();
    mxlbridge::GrainWriter writer(dom.instance, mxlbridge::ancFlowDef(ident(live), config::AncFormat{}), {});
    // A crashed writer leaves its flow directory behind without any lock.
    testutil::writeFile(mxlbridge::flowDirPath(dom.path(), stale) + "/data", "stale");
    CHECK(mxlbridge::flowInUse(dom.path(), live));
    CHECK_FALSE(mxlbridge::flowInUse(dom.path(), stale));
    auto const removed = mxlbridge::removeStaleFlows(dom.path(), {live, stale});
    REQUIRE(removed.size() == 1);
    CHECK(removed[0] == stale);
    CHECK(std::filesystem::exists(mxlbridge::flowDirPath(dom.path(), live)));
}

TEST_CASE("mxl: pattern writer -> verifier round trip (colour bars, tone, time code)")
{
    Domain dom;
    mxlbridge::PatternConfig pc;
    pc.videoFlow = util::uuidV4();
    pc.video = p25();
    pc.audioFlow = util::uuidV4();
    pc.audio.channels = 2;
    pc.ancFlow = util::uuidV4();
    pc.anc.rate = {25, 1};
    mxlbridge::PatternWriter writer(dom.instance, pc);
    writer.start();

    mxlbridge::VerifyConfig vc;
    vc.videoFlow = pc.videoFlow;
    vc.video = pc.video;
    vc.audioFlow = pc.audioFlow;
    vc.audio = pc.audio;
    vc.ancFlow = pc.ancFlow;
    vc.anc = pc.anc;
    vc.duration = std::chrono::milliseconds(1500);
    auto const report = mxlbridge::verifyFlows(dom.instance, vc);
    writer.stop();
    INFO(report.toJson().dump(2));
    CHECK(report.ok());
    CHECK(report.videoGrains >= 20);
    CHECK(report.videoOffsetGrains == 0);
    CHECK(report.audioOffsetSamples == 0);
    CHECK(report.ancOffsetGrains == 0);
    CHECK(report.avMisalignmentSamples == 0);
    CHECK(writer.errors() == 0);
}
