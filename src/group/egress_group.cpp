// SPDX-License-Identifier: MIT
#include "group/egress_group.hpp"

#include <algorithm>
#include <cmath>

#include <mxl/flow.h>

#include "codec/anc8331.hpp"
#include "codec/audioconv.hpp"
#include "group/ingest_essence.hpp"
#include "group/replacement.hpp"
#include "mxlbridge/flowdef.hpp"
#include "mxlbridge/flowsync.hpp"
#include "mxlbridge/reader.hpp"
#include "timing/rtpclock.hpp"
#include "util/backoff.hpp"
#include "util/cpuset.hpp"
#include "util/threading.hpp"

namespace mxlgw::group
{
    namespace
    {
        constexpr std::int64_t noSignalAfterNs = 500'000'000;
        constexpr std::int64_t publishEveryNs = 250'000'000;

        std::int64_t steadyNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::int64_t ceilDiv(std::int64_t a, std::int64_t b)
        {
            return a >= 0 ? (a + b - 1) / b : -((-a) / b);
        }
    }

    struct EgressGroup::Mailbox
    {
        struct Cache
        {
            std::optional<ReaderInfo> reader;
            media::SessionStats tx;
            bool mxlReceiverActive = false;
            bool rtpSenderActive = false;
        };
        struct Result
        {
            MxlReceiverTarget target;
            Resolution resolution;
        };

        std::mutex mutex;
        std::condition_variable cv;
        bool stop = false;
        std::uint64_t generation = 0;
        std::map<util::Uuid, MxlReceiverTarget> rx;
        std::map<util::Uuid, RtpTarget> tx;
        std::map<util::Uuid, Result> results;
        std::map<util::Uuid, Cache> cache;
    };

    struct EgressGroup::Essence
    {
        config::EssenceType type = config::EssenceType::Video;
        std::size_t index = 0;
        util::Uuid uid;
        std::string label;
        config::VideoFormat video;
        config::AudioFormat audio;
        config::AncFormat anc;
        int payloadType = 96;
        config::Pacing pacing = config::Pacing::Narrow;
        config::Packing packing = config::Packing::Bpm;
        util::Rational rate;
        std::atomic<std::int64_t> readOffsetNs{0};
        std::unique_ptr<StateHolder> state;

        // worker-owned
        MxlReceiverTarget haveRx;
        RtpTarget haveTx;
        std::unique_ptr<mxlbridge::GrainReader> grains;
        std::unique_ptr<mxlbridge::SampleReader> samples;
        bool inSync = false;
        std::optional<ReaderInfo> readerInfo;
        util::Backoff backoff{std::chrono::milliseconds(500), std::chrono::seconds(5), 0.1};
        std::int64_t nextAttempt = 0;
        bool resolving = false;
        bool formatError = false;
        bool wasAttached = false; // since the last target change
        std::unique_ptr<media::VideoTxSession> vtx;
        std::unique_ptr<media::AudioTxSession> atx;
        std::unique_ptr<media::AncTxSession> ntx;
        std::unique_ptr<VideoReplacement> replacement;
        bool repeatFresh = false;
        std::optional<std::uint64_t> lastGoodIndex;
        std::optional<std::uint64_t> lastMissed;
        std::int64_t lastGoodSteady = 0;

        // counters
        std::atomic<std::uint64_t> grainsRead{0};
        std::atomic<std::uint64_t> readTimeouts{0};
        std::atomic<std::uint64_t> lateReads{0};
        std::atomic<std::uint64_t> grainsInvalid{0};
        std::atomic<std::uint64_t> flowNotFound{0};
        std::atomic<std::uint64_t> txDropped{0};
        std::atomic<double> readLag{0.0};
        std::atomic<bool> haveLag{false};
        std::atomic<std::int64_t> leadNs{0};
        std::atomic<bool> haveLead{false};

        bool hasReader() const { return grains || samples; }
        mxlbridge::FlowReader const* reader() const
        {
            return grains ? static_cast<mxlbridge::FlowReader const*>(grains.get()) : static_cast<mxlbridge::FlowReader const*>(samples.get());
        }
        bool hasTx() const { return vtx || atx || ntx; }
    };

    namespace
    {
        thread_local mxlbridge::SyncGroup* tlsSync = nullptr;
    }

    EgressGroup::EgressGroup(EgressSpec spec, media::MediaBackend& backend, DomainResolver& resolver)
        : _spec(std::move(spec))
        , _backend(backend)
        , _resolver(resolver)
        , _mailbox(std::make_shared<Mailbox>())
    {
        auto const& g = _spec.group;
        auto add = [&](config::EssenceType type, std::size_t index, config::EssenceCommon const& common)
        {
            auto e = std::make_unique<Essence>();
            e->type = type;
            e->index = index;
            e->uid = common.uid;
            e->label = common.label;
            e->payloadType = common.payloadType;
            auto const it = _spec.readOffsetNs.find(common.uid);
            e->readOffsetNs.store(it != _spec.readOffsetNs.end() ? it->second : 0);
            e->state = std::make_unique<StateHolder>(nlohmann::json{{"group", g.label}, {"essence", common.label}, {"direction", "egress"}});
            e->state->set(EssenceState::Idle, "receiver_inactive");
            _essences.push_back(std::move(e));
        };
        for (std::size_t i = 0; i < g.video.size(); ++i)
        {
            add(config::EssenceType::Video, i, g.video[i]);
            auto& e = *_essences.back();
            e.video = g.video[i].format;
            e.pacing = g.video[i].pacing;
            e.packing = g.video[i].packing;
            e.rate = e.video.grainRate();
            e.replacement = std::make_unique<VideoReplacement>(e.video, g.missingData);
        }
        for (std::size_t i = 0; i < g.audio.size(); ++i)
        {
            add(config::EssenceType::Audio, i, g.audio[i]);
            auto& e = *_essences.back();
            e.audio = g.audio[i].format;
            e.rate = {e.audio.sampleRate, 1};
        }
        for (std::size_t i = 0; i < g.anc.size(); ++i)
        {
            add(config::EssenceType::Anc, i, g.anc[i]);
            auto& e = *_essences.back();
            e.anc = g.anc[i].format;
            e.rate = e.anc.grainRate();
        }
        // Group cadence: first video grain rate, else ANC, else the audio block rate (§5.7).
        if (!g.video.empty())
        {
            _rate = g.video.front().format.grainRate();
        }
        else if (!g.anc.empty())
        {
            _rate = g.anc.front().format.grainRate();
        }
        else if (!g.audio.empty())
        {
            _rate = {1'000'000, g.audio.front().format.blockUs};
        }
        else
        {
            _rate = {50, 1};
        }
        _cadenceNs = config::periodNs(_rate);
        _outputDelayNs = g.effectiveOutputDelayNs();
        _thread = std::thread([this] { run(); });
    }

    EgressGroup::~EgressGroup()
    {
        {
            std::lock_guard const lock{_mailbox->mutex};
            _mailbox->stop = true;
        }
        _mailbox->cv.notify_all();
        _thread.join();
    }

    void EgressGroup::setReceiver(util::Uuid const& essenceUid, MxlReceiverTarget const& target)
    {
        {
            std::lock_guard const lock{_mailbox->mutex};
            _mailbox->rx[essenceUid] = target;
            ++_mailbox->generation;
        }
        _mailbox->cv.notify_all();
    }

    void EgressGroup::setSender(util::Uuid const& essenceUid, RtpTarget const& target)
    {
        {
            std::lock_guard const lock{_mailbox->mutex};
            _mailbox->tx[essenceUid] = target;
            ++_mailbox->generation;
        }
        _mailbox->cv.notify_all();
    }

    void EgressGroup::setReadOffset(util::Uuid const& essenceUid, std::int64_t readOffsetNs)
    {
        for (auto& e : _essences)
        {
            if (e->uid == essenceUid)
            {
                e->readOffsetNs.store(readOffsetNs);
            }
        }
    }

    std::int64_t EgressGroup::maxReadOffset() const
    {
        std::int64_t m = 0;
        for (auto const& e : _essences)
        {
            m = std::max(m, e->readOffsetNs.load());
        }
        return m;
    }

    void EgressGroup::detach(Essence& e)
    {
        if (e.hasReader())
        {
            if (e.inSync && tlsSync != nullptr)
            {
                tlsSync->remove(*e.reader());
            }
            e.grains.reset();
            e.samples.reset();
        }
        e.inSync = false;
        e.readerInfo.reset();
        e.lastMissed.reset();
        e.lastGoodIndex.reset();
        e.haveLag.store(false);
    }

    void EgressGroup::scheduleRetry(Essence& e, std::int64_t steadyNow, char const* reason)
    {
        e.flowNotFound.fetch_add(1, std::memory_order_relaxed);
        if (e.formatError)
        {
            // keep error/format_mismatch while retrying
        }
        else if (e.wasAttached && std::string(reason) == "flow_not_found")
        {
            // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/internal/src/Instance.cpp:135, SharedMemory.cpp:56 — readers hold no lock, so a
            // writer that stops cleanly deletes its flow. A flow that was read and went away is a stopped source:
            // no_signal (§5.8), not waiting_for_flow (docs/decisions.md).
            e.state->set(EssenceState::NoSignal, "flow_removed");
        }
        else
        {
            e.state->set(EssenceState::WaitingForFlow, reason);
        }
        auto const delay = e.backoff.next();
        e.nextAttempt = steadyNow + std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count();
        auto const key = e.uid.toString();
        if (_notFoundLog.allow(key))
        {
            bool const domainMissing = std::string(reason) == "domain_not_found" || std::string(reason) == "domain_conflict";
            log::warn(domainMissing ? "mxl_domain_unknown" : "mxl_flow_not_found",
                      {{"group", _spec.group.label},
                       {"essence", e.label},
                       {"reason", reason},
                       {"mxl_domain_id", e.haveRx.domainId ? e.haveRx.domainId->toString() : std::string()},
                       {"mxl_flow_id", e.haveRx.flowId ? e.haveRx.flowId->toString() : std::string()},
                       {"retry_in_ms", std::chrono::duration_cast<std::chrono::milliseconds>(delay).count()}});
        }
    }

    void EgressGroup::attach(Essence& e, Resolution const& r, std::int64_t steadyNow)
    {
        mxlStatus status = MXL_ERR_UNKNOWN;
        auto const flowId = e.haveRx.flowId->toString();
        if (e.type == config::EssenceType::Audio)
        {
            e.samples = mxlbridge::SampleReader::open(r.instance, flowId, status);
        }
        else
        {
            e.grains = mxlbridge::GrainReader::open(r.instance, flowId, status);
        }
        if (status == MXL_ERR_FLOW_NOT_FOUND || !e.hasReader())
        {
            detach(e);
            if (status != MXL_ERR_FLOW_NOT_FOUND && status != MXL_STATUS_OK)
            {
                // e.g. MXL_ERR_INVALID_FLOW_READER: the flow is of another kind (video vs audio).
                e.formatError = true;
                e.state->set(EssenceState::Error, "format_mismatch");
                log::warn("flow_def_mismatch",
                          {{"group", _spec.group.label}, {"essence", e.label}, {"mxl_flow_id", flowId}, {"status", mxlbridge::statusName(status)}});
            }
            scheduleRetry(e, steadyNow, "flow_not_found");
            return;
        }
        // §6.4 / §5.8: check the definition every time the flow appears or is re-created.
        auto const& def = e.reader()->flowDef();
        std::vector<std::string> mismatch;
        switch (e.type)
        {
            case config::EssenceType::Video: mismatch = mxlbridge::compareVideo(def, e.video); break;
            case config::EssenceType::Audio: mismatch = mxlbridge::compareAudio(def, e.audio); break;
            case config::EssenceType::Anc: mismatch = mxlbridge::compareAnc(def, e.anc); break;
        }
        if (!mismatch.empty())
        {
            detach(e);
            e.formatError = true;
            e.state->set(EssenceState::Error, "format_mismatch");
            log::warn("flow_def_mismatch", {{"group", _spec.group.label}, {"essence", e.label}, {"mxl_flow_id", flowId}, {"differences", mismatch}});
            scheduleRetry(e, steadyNow, "format_mismatch");
            return;
        }
        e.formatError = false;
        e.wasAttached = true;
        e.backoff.reset();
        e.readerInfo = ReaderInfo{r.domain.id, r.domain.path, mxlbridge::toName(r.domain.kind), *e.haveRx.flowId};
        if (tlsSync != nullptr && tlsSync->add(*e.reader()) == MXL_STATUS_OK)
        {
            e.inSync = true;
        }
        e.lastGoodSteady = steadyNow;
        log::info("mxl_reader_attached", {{"group", _spec.group.label},
                                          {"essence", e.label},
                                          {"mxl_flow_id", flowId},
                                          {"domain_id", r.domain.id.toString()},
                                          {"domain_path", r.domain.path},
                                          {"domain_kind", mxlbridge::toName(r.domain.kind)}});
    }

    void EgressGroup::reconcileTx(Essence& e)
    {
        RtpTarget want;
        {
            std::lock_guard const lock{_mailbox->mutex};
            auto const it = _mailbox->tx.find(e.uid);
            if (it == _mailbox->tx.end())
            {
                return;
            }
            want = it->second;
        }
        if (want == e.haveTx && (e.hasTx() || !want.masterEnable))
        {
            return;
        }
        bool const legsChanged = !(want.legs == e.haveTx.legs);
        e.haveTx = want;
        if (!want.masterEnable)
        {
            e.vtx.reset();
            e.atx.reset();
            e.ntx.reset();
            log::info("egress_sender_stopped", {{"group", _spec.group.label}, {"essence", e.label}});
            return;
        }
        if (e.hasTx() && legsChanged)
        {
            bool const ok = e.vtx ? e.vtx->updateDestination(want.legs) : e.atx ? e.atx->updateDestination(want.legs) : e.ntx->updateDestination(want.legs);
            log::info("egress_destination_updated", {{"group", _spec.group.label}, {"essence", e.label}, {"ok", ok}});
            if (ok)
            {
                return;
            }
            e.vtx.reset();
            e.atx.reset();
            e.ntx.reset();
        }
        if (e.hasTx())
        {
            return;
        }
        try
        {
            auto const grainsAhead = static_cast<int>(ceilDiv(_outputDelayNs + _cadenceNs, _cadenceNs)) + 2;
            switch (e.type)
            {
                case config::EssenceType::Video:
                {
                    media::VideoTxParams p{e.label, e.video, e.payloadType, e.pacing, e.packing, want.legs, grainsAhead};
                    e.vtx = _backend.createVideoTx(p);
                    break;
                }
                case config::EssenceType::Audio:
                {
                    auto const block = e.audio.blockDurationNs();
                    auto const depth = static_cast<int>(ceilDiv(_outputDelayNs + _cadenceNs, block)) + 4;
                    e.atx = _backend.createAudioTx({e.label, e.audio, e.payloadType, want.legs, depth});
                    break;
                }
                case config::EssenceType::Anc:
                {
                    e.ntx = _backend.createAncTx({e.label, e.anc, e.payloadType, want.legs, grainsAhead});
                    break;
                }
            }
            log::info("egress_sender_started", {{"group", _spec.group.label}, {"essence", e.label}});
        }
        catch (std::exception const& ex)
        {
            log::error("egress_sender_failed", {{"group", _spec.group.label}, {"essence", e.label}, {"error", ex.what()}});
        }
    }

    void EgressGroup::reconcileRx(Essence& e, std::int64_t steadyNow)
    {
        MxlReceiverTarget want;
        std::optional<Mailbox::Result> result;
        bool haveWant = false;
        {
            std::lock_guard const lock{_mailbox->mutex};
            if (auto const it = _mailbox->rx.find(e.uid); it != _mailbox->rx.end())
            {
                want = it->second;
                haveWant = true;
            }
            if (auto const it = _mailbox->results.find(e.uid); it != _mailbox->results.end())
            {
                result = std::move(it->second);
                _mailbox->results.erase(it);
            }
        }
        if (haveWant && !(want == e.haveRx))
        {
            detach(e);
            e.haveRx = want;
            e.backoff.reset();
            e.nextAttempt = steadyNow;
            e.formatError = false;
            e.wasAttached = false;
            e.resolving = false;
            result.reset();
            if (!want.masterEnable)
            {
                e.state->set(EssenceState::Idle, "receiver_inactive");
                log::info("mxl_reader_stopped", {{"group", _spec.group.label}, {"essence", e.label}});
            }
            else if (!want.flowId || !want.domainId)
            {
                e.state->set(EssenceState::Idle, "no_flow");
            }
            else
            {
                e.state->set(EssenceState::WaitingForFlow, "attaching");
            }
        }
        if (!e.haveRx.masterEnable || !e.haveRx.flowId || !e.haveRx.domainId)
        {
            return;
        }
        if (result)
        {
            e.resolving = false;
            if (result->target == e.haveRx && !e.hasReader())
            {
                if (result->resolution.found())
                {
                    attach(e, result->resolution, steadyNow);
                }
                else
                {
                    scheduleRetry(e, steadyNow, result->resolution.reason());
                }
            }
        }
        if (!e.hasReader() && !e.resolving && steadyNow >= e.nextAttempt)
        {
            e.resolving = true;
            std::weak_ptr<Mailbox> weak = _mailbox;
            auto const uid = e.uid;
            auto const target = e.haveRx;
            _resolver.request(uid.toString() + "@" + _spec.group.uid.toString(), *target.domainId, *target.flowId,
                              [weak, uid, target](Resolution r)
                              {
                                  if (auto mb = weak.lock())
                                  {
                                      {
                                          std::lock_guard const lock{mb->mutex};
                                          mb->results[uid] = Mailbox::Result{target, std::move(r)};
                                          ++mb->generation;
                                      }
                                      mb->cv.notify_all();
                                  }
                              });
        }
    }

    void EgressGroup::reconcile(std::int64_t steadyNow)
    {
        for (auto& e : _essences)
        {
            reconcileTx(*e);
            reconcileRx(*e, steadyNow);
        }
    }

    void EgressGroup::markData(Essence& e, bool good, std::int64_t steadyNow)
    {
        if (!e.hasReader())
        {
            return;
        }
        if (good)
        {
            e.lastGoodSteady = steadyNow;
            if (!e.inSync && tlsSync != nullptr && tlsSync->add(*e.reader()) == MXL_STATUS_OK)
            {
                e.inSync = true;
            }
            e.state->set(EssenceState::Running);
            return;
        }
        if (steadyNow - e.lastGoodSteady > noSignalAfterNs)
        {
            // §5.8: a silent flow is not an error. Take it out of the sync group so it does not hold up
            // the other essences; it is polled without blocking and re-added with its first grain.
            if (e.inSync && tlsSync != nullptr)
            {
                tlsSync->remove(*e.reader());
                e.inSync = false;
            }
            e.state->set(EssenceState::NoSignal, "no_grains");
        }
    }

    void EgressGroup::processGrain(Essence& e, std::int64_t periodOrigin, std::int64_t deadline)
    {
        auto const idx = timing::timestampToIndex(e.rate, periodOrigin);
        auto const txTai = timing::indexToTimestamp(e.rate, idx) + _outputDelayNs;
        bool const interlaced = e.type == config::EssenceType::Video ? e.video.interlaced() : e.anc.interlace != config::Interlace::Progressive;
        bool const secondField = interlaced && (idx % 2 == 1);
        std::uint8_t const* payload = nullptr;
        mxlGrainInfo info{};
        bool good = false;
        bool const tx = e.hasTx();

        if (e.grains)
        {
            auto const now = media::hostTaiNs();
            auto const timeout = e.inSync ? std::max<std::int64_t>(0, deadline - now) : 0;
            auto const status = e.grains->get(idx, static_cast<std::uint64_t>(timeout), info, payload);
            switch (status)
            {
                case MXL_STATUS_OK:
                    if ((info.flags & MXL_GRAIN_FLAG_INVALID) != 0)
                    {
                        e.grainsInvalid.fetch_add(1, std::memory_order_relaxed);
                        payload = nullptr;
                        // A writer that marks grains invalid is alive: not "no signal".
                        e.lastGoodSteady = steadyNs();
                    }
                    else
                    {
                        good = true;
                        e.grainsRead.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                case MXL_ERR_OUT_OF_RANGE_TOO_EARLY:
                    e.readTimeouts.fetch_add(1, std::memory_order_relaxed);
                    e.lastMissed = idx;
                    payload = nullptr;
                    break;
                case MXL_ERR_OUT_OF_RANGE_TOO_LATE:
                    e.lateReads.fetch_add(1, std::memory_order_relaxed);
                    payload = nullptr;
                    break;
                case MXL_ERR_FLOW_INVALID:
                    // §5.8: the writer re-created the flow — re-open immediately with the same flow id.
                    log::info("mxl_flow_recreated", {{"group", _spec.group.label}, {"essence", e.label}});
                    detach(e);
                    e.nextAttempt = 0;
                    payload = nullptr;
                    break;
                default:
                    e.readTimeouts.fetch_add(1, std::memory_order_relaxed);
                    payload = nullptr;
                    break;
            }
            if (e.grains)
            {
                if (e.lastMissed && *e.lastMissed < idx)
                {
                    mxlGrainInfo lateInfo{};
                    std::uint8_t const* latePayload = nullptr;
                    if (e.grains->getNonBlocking(*e.lastMissed, lateInfo, latePayload) == MXL_STATUS_OK)
                    {
                        e.lateReads.fetch_add(1, std::memory_order_relaxed); // arrived after its deadline: skipped
                    }
                    e.lastMissed.reset();
                }
                if (auto const rt = e.grains->runtime())
                {
                    e.readLag.store(static_cast<double>(static_cast<std::int64_t>(rt->headIndex) - static_cast<std::int64_t>(idx)), std::memory_order_relaxed);
                    e.haveLag.store(true, std::memory_order_relaxed);
                }
            }
        }
        if (good)
        {
            e.lastGoodIndex = idx;
            e.repeatFresh = false;
        }

        if (tx)
        {
            auto const now = media::hostTaiNs();
            if (e.type == config::EssenceType::Video)
            {
                std::uint8_t const* frame = payload;
                if (!good)
                {
                    if (_spec.group.missingData == config::MissingData::Repeat && !e.repeatFresh && e.lastGoodIndex && e.grains)
                    {
                        // Copy the last good grain once, on the first miss (it is still in the ring).
                        mxlGrainInfo li{};
                        std::uint8_t const* lp = nullptr;
                        if (e.grains->getNonBlocking(*e.lastGoodIndex, li, lp) == MXL_STATUS_OK && (li.flags & MXL_GRAIN_FLAG_INVALID) == 0)
                        {
                            e.replacement->remember(lp);
                        }
                        e.repeatFresh = true;
                    }
                    frame = e.replacement->frame();
                }
                if (!e.vtx->send(frame, txTai, secondField, std::chrono::milliseconds(2)))
                {
                    e.txDropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
            else
            {
                codec::AncFrame frame;
                if (good)
                {
                    auto parsed = codec::parseGrain(payload, std::min<std::size_t>(info.grainSize, config::ancGrainBytes));
                    if (parsed.frame)
                    {
                        frame = std::move(*parsed.frame);
                    }
                    else
                    {
                        e.grainsInvalid.fetch_add(1, std::memory_order_relaxed);
                        frame = emptyAnc();
                    }
                }
                else
                {
                    frame = emptyAnc();
                }
                if (secondField)
                {
                    frame.field = codec::AncField::Field2;
                }
                else if (interlaced)
                {
                    frame.field = codec::AncField::Field1;
                }
                auto const dropped = e.ntx->send(frame, txTai, secondField, std::chrono::milliseconds(2));
                if (dropped > 0)
                {
                    e.txDropped.fetch_add(dropped, std::memory_order_relaxed);
                }
            }
            e.leadNs.store(txTai - now, std::memory_order_relaxed);
            e.haveLead.store(true, std::memory_order_relaxed);
        }
        markData(e, good, steadyNs());
    }

    void EgressGroup::processAudio(Essence& e, std::int64_t periodOrigin, std::int64_t deadline)
    {
        auto const n = static_cast<std::int64_t>(e.audio.samplesPerBlock());
        if (n <= 0)
        {
            return;
        }
        util::Rational const sr{e.audio.sampleRate, 1};
        auto const sStart = static_cast<std::int64_t>(timing::timestampToIndex(sr, periodOrigin));
        auto const sEnd = static_cast<std::int64_t>(timing::timestampToIndex(sr, periodOrigin + _cadenceNs));
        bool anyGood = false;
        for (auto k = ceilDiv(sStart, n); k < ceilDiv(sEnd, n); ++k)
        {
            auto const end = static_cast<std::uint64_t>((k + 1) * n);
            std::uint8_t* buffer = e.atx ? e.atx->acquire(std::chrono::milliseconds(1)) : nullptr;
            if (e.atx && buffer == nullptr)
            {
                e.txDropped.fetch_add(1, std::memory_order_relaxed);
            }
            bool good = false;
            if (e.samples)
            {
                auto const now = media::hostTaiNs();
                auto const timeout = e.inSync ? std::max<std::int64_t>(0, deadline - now) : 0;
                mxlWrappedMultiBufferSlice slices{};
                auto const status = e.samples->get(end, static_cast<std::size_t>(n), static_cast<std::uint64_t>(timeout), slices);
                switch (status)
                {
                    case MXL_STATUS_OK:
                    {
                        good = true;
                        e.grainsRead.fetch_add(1, std::memory_order_relaxed);
                        if (buffer != nullptr)
                        {
                            codec::ConstChannelSlices src;
                            for (int f = 0; f < 2; ++f)
                            {
                                src.fragments[f].pointer = slices.base.fragments[f].pointer;
                                src.fragments[f].size = slices.base.fragments[f].size;
                            }
                            src.stride = slices.stride;
                            src.count = slices.count;
                            codec::floatToPcm(src, static_cast<std::size_t>(n), e.audio.channels, e.audio.bitDepth, buffer);
                        }
                        break;
                    }
                    case MXL_ERR_OUT_OF_RANGE_TOO_EARLY: e.readTimeouts.fetch_add(1, std::memory_order_relaxed); break;
                    case MXL_ERR_OUT_OF_RANGE_TOO_LATE: e.lateReads.fetch_add(1, std::memory_order_relaxed); break;
                    case MXL_ERR_FLOW_INVALID:
                        log::info("mxl_flow_recreated", {{"group", _spec.group.label}, {"essence", e.label}});
                        detach(e);
                        e.nextAttempt = 0;
                        break;
                    default: e.readTimeouts.fetch_add(1, std::memory_order_relaxed); break;
                }
                if (e.samples)
                {
                    if (auto const rt = e.samples->runtime())
                    {
                        auto const perGrain = std::max<double>(1.0, static_cast<double>(e.audio.sampleRate) * static_cast<double>(_cadenceNs) / 1e9);
                        e.readLag.store((static_cast<double>(rt->headIndex) - static_cast<double>(end)) / perGrain, std::memory_order_relaxed);
                        e.haveLag.store(true, std::memory_order_relaxed);
                    }
                }
            }
            anyGood = anyGood || good;
            if (buffer != nullptr)
            {
                if (!good)
                {
                    codec::pcmSilence(buffer, static_cast<std::size_t>(n), e.audio.channels, e.audio.bitDepth);
                }
                auto const txTai = timing::taiOfTicks(k * n, e.audio.sampleRate) + _outputDelayNs;
                e.leadNs.store(txTai - media::hostTaiNs(), std::memory_order_relaxed);
                e.haveLead.store(true, std::memory_order_relaxed);
                e.atx->send(txTai);
            }
        }
        markData(e, anyGood, steadyNs());
    }

    void EgressGroup::processPeriod(std::uint64_t i)
    {
        auto const origin = timing::indexToTimestamp(_rate, i);
        auto const deadline = origin + _outputDelayNs - _spec.txLeadNs;
        if (tlsSync != nullptr && tlsSync->size() > 0)
        {
            auto const timeout = std::max<std::int64_t>(0, deadline - media::hostTaiNs());
            // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/internal/src/FlowSynchronizationGroup.cpp:77-131 — waits until each reader's
            // head reaches timestampToIndex(rate, T); returns MXL_ERR_OUT_OF_RANGE_TOO_EARLY on timeout.
            (void)tlsSync->waitForDataAt(static_cast<std::uint64_t>(origin), static_cast<std::uint64_t>(timeout));
        }
        for (auto& e : _essences)
        {
            if (e->type == config::EssenceType::Audio)
            {
                processAudio(*e, origin, deadline);
            }
            else
            {
                processGrain(*e, origin, deadline);
            }
        }
    }

    GroupSnapshot EgressGroup::snapshot() const
    {
        GroupSnapshot g;
        g.uid = _spec.group.uid;
        g.label = _spec.group.label;
        g.direction = config::Direction::Egress;
        g.enabled = _spec.group.enabled;
        g.domain = _spec.group.domain;
        g.outputDelayNs = _outputDelayNs;
        std::map<util::Uuid, Mailbox::Cache> cache;
        std::map<util::Uuid, MxlReceiverTarget> rx;
        {
            std::lock_guard const lock{_mailbox->mutex};
            cache = _mailbox->cache;
            rx = _mailbox->rx;
        }
        for (auto const& e : _essences)
        {
            EssenceSnapshot s;
            s.groupUid = _spec.group.uid;
            s.groupLabel = _spec.group.label;
            s.uid = e->uid;
            s.label = e->label;
            s.type = e->type;
            s.direction = config::Direction::Egress;
            s.state = e->state->get();
            if (auto const it = rx.find(e->uid); it != rx.end() && it->second.flowId)
            {
                s.flowId = *it->second.flowId;
            }
            if (auto const it = cache.find(e->uid); it != cache.end())
            {
                s.reader = it->second.reader;
                s.tx = it->second.tx;
                s.mxlReceiverActive = it->second.mxlReceiverActive;
                s.rtpSenderActive = it->second.rtpSenderActive;
            }
            s.grainsRead = e->grainsRead.load(std::memory_order_relaxed);
            s.readTimeouts = e->readTimeouts.load(std::memory_order_relaxed);
            s.lateReads = e->lateReads.load(std::memory_order_relaxed);
            s.grainsInvalid = e->grainsInvalid.load(std::memory_order_relaxed);
            s.flowNotFound = e->flowNotFound.load(std::memory_order_relaxed);
            s.txDropped = e->txDropped.load(std::memory_order_relaxed);
            if (e->haveLag.load(std::memory_order_relaxed))
            {
                s.readLagGrains = e->readLag.load(std::memory_order_relaxed);
            }
            if (e->haveLead.load(std::memory_order_relaxed))
            {
                s.leadNs = e->leadNs.load(std::memory_order_relaxed);
            }
            s.readOffsetNs = e->readOffsetNs.load();
            g.essences.push_back(std::move(s));
        }
        return g;
    }

    void EgressGroup::run()
    {
        util::setThreadName("egress-" + _spec.group.label.substr(0, 8));
        if (!_spec.appCpus.empty())
        {
            if (auto const cpus = util::parseCpuList(_spec.appCpus))
            {
                util::pinToCpus(*cpus);
            }
        }
        util::tryRealtime(20);
        std::unique_ptr<mxlbridge::SyncGroup> sync;
        try
        {
            sync = std::make_unique<mxlbridge::SyncGroup>(_spec.owner);
        }
        catch (std::exception const& ex)
        {
            log::error("mxl_sync_group_failed", {{"group", _spec.group.label}, {"error", ex.what()}});
        }
        tlsSync = sync.get();

        auto publish = [&]
        {
            std::lock_guard const lock{_mailbox->mutex};
            for (auto const& e : _essences)
            {
                auto& c = _mailbox->cache[e->uid];
                c.reader = e->readerInfo;
                c.mxlReceiverActive = e->haveRx.masterEnable;
                c.rtpSenderActive = e->hasTx();
                c.tx = e->vtx ? e->vtx->stats() : e->atx ? e->atx->stats() : e->ntx ? e->ntx->stats() : media::SessionStats{};
            }
        };

        std::uint64_t seenGeneration = ~std::uint64_t{0};
        auto lastPublish = steadyNs();
        auto i = timing::timestampToIndex(_rate, media::hostTaiNs());
        while (true)
        {
            {
                std::lock_guard const lock{_mailbox->mutex};
                if (_mailbox->stop)
                {
                    break;
                }
            }
            reconcile(steadyNs());
            auto const origin = timing::indexToTimestamp(_rate, i);
            auto const readOffset = maxReadOffset();
            auto const readStart = origin + _cadenceNs + readOffset;
            // Sleep until the end of grain period i plus the read offset (§5.7); wake for target changes.
            bool stop = false;
            while (true)
            {
                auto const now = media::hostTaiNs();
                if (now >= readStart)
                {
                    break;
                }
                std::unique_lock lock{_mailbox->mutex};
                if (_mailbox->stop)
                {
                    stop = true;
                    break;
                }
                if (_mailbox->generation != seenGeneration)
                {
                    seenGeneration = _mailbox->generation;
                    lock.unlock();
                    reconcile(steadyNs());
                    continue;
                }
                _mailbox->cv.wait_for(lock, std::chrono::nanoseconds(std::min<std::int64_t>(readStart - now, 20'000'000)));
            }
            if (stop)
            {
                break;
            }
            auto const now = media::hostTaiNs();
            auto const deadline = origin + _outputDelayNs - _spec.txLeadNs;
            if (now > deadline)
            {
                // Behind schedule (start-up, stall): skip to the period whose read time is now.
                _periodsLate.fetch_add(1, std::memory_order_relaxed);
                if (_behindLog.allow("behind"))
                {
                    log::warn("egress_behind_schedule", {{"group", _spec.group.label}, {"late_ns", now - deadline}});
                }
                i = timing::timestampToIndex(_rate, now - _cadenceNs - readOffset);
                if (timing::indexToTimestamp(_rate, i) + _outputDelayNs - _spec.txLeadNs < now)
                {
                    ++i;
                }
                continue;
            }
            processPeriod(i);
            ++i;
            if (steadyNs() - lastPublish > publishEveryNs)
            {
                publish();
                lastPublish = steadyNs();
            }
        }
        for (auto& e : _essences)
        {
            detach(*e);
            e->vtx.reset();
            e->atx.reset();
            e->ntx.reset();
        }
        tlsSync = nullptr;
        sync.reset();
    }
}
