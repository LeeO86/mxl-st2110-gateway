// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <vector>

#include "codec/v210.hpp"
#include "helpers.hpp"
#include "mtl/mock_backend.hpp"
#include "timing/rtpclock.hpp"

using namespace mxlgw;
using namespace std::chrono_literals;

namespace
{
    struct Sink : media::VideoRxHandler
    {
        explicit Sink(std::size_t bytes)
            : buffer(bytes)
        {}
        std::uint8_t* acquire(media::FrameMeta const& meta) noexcept override
        {
            last = meta;
            return drop ? nullptr : buffer.data();
        }
        std::vector<std::uint8_t> buffer;
        media::FrameMeta last;
        bool drop = false;
    };

    config::Config cfg()
    {
        auto r = config::parseAndValidate(testutil::sampleConfig());
        REQUIRE(r.ok());
        return *r.config;
    }
}

TEST_CASE("mock video loops TX to RX at the transmit time, 2022-7 merge")
{
    auto backend = media::createMockBackend(cfg());
    CHECK(backend->name() == "mock");
    CHECK(backend->status().testBackend);
    CHECK(backend->status().ports.size() == 2);
    config::VideoFormat f;
    Sink sink(f.grainBytes());
    media::VideoRxParams rp;
    rp.format = f;
    rp.legs = {{"239.1.1.1", "", 20000, true}, {"239.2.1.1", "", 20000, true}};
    auto rx = backend->createVideoRx(rp, sink);
    media::VideoTxParams tp;
    tp.format = f;
    tp.legs = rp.legs;
    auto tx = backend->createVideoTx(tp);

    std::vector<std::uint8_t> frame(f.grainBytes());
    codec::v210ColourBars(frame.data(), f.width, f.height, f.v210Stride(), 1234);
    auto const when = media::hostTaiNs() + 20'000'000;
    CHECK(tx->send(frame.data(), when, false, 10ms));
    auto const meta = rx->next(1s);
    REQUIRE(meta);
    CHECK(media::hostTaiNs() >= when);
    CHECK(meta->rtpTimestamp == timing::rtpAt(when, timing::videoClockHz));
    CHECK(codec::v210ReadCounter(sink.buffer.data(), f.width, f.v210Stride()) == 1234u);
    rx->release();
    CHECK_FALSE(rx->next(50ms)); // the duplicate from leg R is merged
    auto const s = rx->stats();
    CHECK(s.legs[0].packets == 1);
    CHECK(s.legs[1].packets == 1);
    CHECK(s.packets == 1);
    CHECK(tx->stats().packets == 1);

    // Only leg R carries traffic: still received.
    tx->updateDestination({{"239.1.1.1", "", 20000, false}, {"239.2.1.1", "", 20000, true}});
    CHECK(tx->send(frame.data(), media::hostTaiNs(), false, 10ms));
    CHECK(rx->next(1s));

    // Moving the receiver away stops delivery.
    rx->updateSource({{"239.99.1.1", "", 20000, true}});
    CHECK(tx->send(frame.data(), media::hostTaiNs(), false, 10ms));
    CHECK_FALSE(rx->next(100ms));

    // Handler drops the frame.
    rx->updateSource(rp.legs);
    sink.drop = true;
    CHECK(tx->send(frame.data(), media::hostTaiNs(), false, 10ms));
    CHECK_FALSE(rx->next(100ms));
    CHECK(rx->stats().framesDropped >= 1);
}

TEST_CASE("mock audio and ANC")
{
    auto backend = media::createMockBackend(cfg());
    media::AudioParams ap;
    ap.format.channels = 2;
    ap.legs = {{"239.1.1.2", "", 20000, true}};
    auto arx = backend->createAudioRx(ap);
    auto atx = backend->createAudioTx(ap);
    auto* buf = atx->acquire(10ms);
    REQUIRE(buf);
    buf[0] = 0x7F;
    auto const when = media::hostTaiNs();
    atx->send(when);
    auto const block = arx->next(1s);
    REQUIRE(block);
    CHECK(block->samples == 48);
    CHECK(block->pcm[0] == 0x7F);
    CHECK(block->meta.rtpTimestamp == timing::rtpAt(when, 48000));
    arx->release();
    CHECK(atx->updateDestination(ap.legs));
    CHECK(arx->updateSource(ap.legs));
    CHECK(arx->stats().packets == 1);
    CHECK(atx->stats().packets == 1);

    media::AncParams np;
    np.legs = {{"239.1.1.3", "", 20000, true}};
    auto nrx = backend->createAncRx(np);
    auto ntx = backend->createAncTx(np);
    codec::AncFrame frame;
    for (int i = 0; i < 25; ++i)
    {
        codec::AncPacket p;
        p.did = 0x60;
        p.sdid = 0x60;
        p.line = static_cast<std::uint16_t>(i);
        frame.packets.push_back(p);
    }
    CHECK(ntx->send(frame, media::hostTaiNs(), true, 10ms) == 5); // MTL-like limit of 20
    auto const got = nrx->next(1s);
    REQUIRE(got);
    CHECK(got->frame.packets.size() == 20);
    CHECK(got->meta.secondField);
    CHECK(ntx->updateDestination(np.legs));
    CHECK(nrx->updateSource(np.legs));
    CHECK(nrx->stats().packets == 1);
    CHECK(ntx->stats().packets == 1);
    CHECK(backend->ptpTimeNs() > 0);
    CHECK(media::legsFromConfig({{"239.0.0.1", "", 1}, {"239.0.0.2", "", 1}}, false).size() == 1);
}
