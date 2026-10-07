// SPDX-License-Identifier: MIT
#include "group/ingest_essence.hpp"

#include <algorithm>
#include <cstring>

#include <mxl/flow.h>

#include "codec/anc8331.hpp"
#include "codec/audioconv.hpp"
#include "mxlbridge/flowdef.hpp"
#include "mxlbridge/writer.hpp"
#include "timing/rtpclock.hpp"
#include "util/cpuset.hpp"
#include "util/logging.hpp"
#include "util/threading.hpp"

namespace mxlgw::group
{
    namespace
    {
        constexpr std::size_t slotCount = 8;
        constexpr auto workerPoll = std::chrono::milliseconds(20);
        constexpr std::int64_t noSignalAfterNs = 1'000'000'000;
        constexpr std::uint64_t noIndex = ~std::uint64_t{0};

        // The payload type the sender's SDP announced; the essence's configured one without an SDP.
        int payloadTypeFor(RtpTarget const& target, int configured)
        {
            return target.payloadType > 0 ? target.payloadType : configured;
        }

        std::int64_t steadyNs()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        config::EssenceCommon const& commonOf(config::Group const& g, config::EssenceType type, std::size_t index)
        {
            switch (type)
            {
                case config::EssenceType::Video: return g.video.at(index);
                case config::EssenceType::Audio: return g.audio.at(index);
                case config::EssenceType::Anc: return g.anc.at(index);
            }
            return g.video.at(index);
        }
    }

    /// One pending video frame between acquire() (lcore) and the worker.
    struct IngestEssence::Slot
    {
        std::atomic<std::uint64_t> tag{0};
        SlotKind kind = SlotKind::Empty;
        std::uint64_t index = 0;
        std::int64_t originTai = 0;
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
    };

    /// The essence's MXL writer (GrainWriter for video/ANC, SampleWriter for audio).
    class IngestEssence::Writer
    {
    public:
        std::unique_ptr<mxlbridge::GrainWriter> grains;
        std::unique_ptr<mxlbridge::SampleWriter> samples;
        std::uint32_t grainCount() const { return grains ? grains->grainCount() : 0; }
    };

    /// Parks the worker outside next() so the control thread can change sessions and writers; the
    /// worker never holds _mutex while it blocks for a frame, so the wait is at most one poll interval.
    class IngestEssence::Quiesce
    {
    public:
        Quiesce(IngestEssence& self, std::unique_lock<std::mutex>& lock)
            : _self(self)
        {
            ++_self._quiesce;
            _self._cv.wait(lock, [&] { return !_self._inNext; });
        }
        ~Quiesce()
        {
            --_self._quiesce;
            _self._cv.notify_all();
        }
        Quiesce(Quiesce const&) = delete;
        Quiesce& operator=(Quiesce const&) = delete;

    private:
        IngestEssence& _self;
    };

    IngestEssence::IngestEssence(IngestSpec spec, media::MediaBackend& backend, std::shared_ptr<mxlbridge::Instance> domain)
        : _spec(std::move(spec))
        , _backend(backend)
        , _domain(std::move(domain))
        , _state({{"group", _spec.group.label}, {"essence", commonOf(_spec.group, _spec.type, _spec.index).label}, {"direction", "ingest"}})
    {
        auto const& common = commonOf(_spec.group, _spec.type, _spec.index);
        _uid = common.uid;
        _label = common.label;
        _ids = essenceIds(_spec.group, _spec.type, _spec.index);
        _flowDef = flowDefinition(_spec.nodeId, _spec.group, _spec.type, _spec.index, mxlbridge::nmosVersion(media::hostTaiNs()));
        switch (_spec.type)
        {
            case config::EssenceType::Video:
            {
                auto const& f = _spec.group.video.at(_spec.index).format;
                _grainRate = f.grainRate();
                auto const lines = static_cast<std::uint32_t>(f.linesPerGrain());
                _writerOptions = mxlbridge::writerOptions(lines, lines);
                _slots = std::make_unique<Slot[]>(slotCount);
                _scratch.resize(f.grainBytes());
                break;
            }
            case config::EssenceType::Audio:
            {
                auto const& f = _spec.group.audio.at(_spec.index).format;
                _grainRate = {f.sampleRate, 1};
                auto const n = static_cast<std::uint32_t>(f.samplesPerBlock());
                _writerOptions = mxlbridge::writerOptions(n, n);
                break;
            }
            case config::EssenceType::Anc:
            {
                auto const& f = _spec.group.anc.at(_spec.index).format;
                _grainRate = f.grainRate();
                _writerOptions = mxlbridge::writerOptions(static_cast<std::uint32_t>(config::ancGrainBytes), static_cast<std::uint32_t>(config::ancGrainBytes));
                break;
            }
        }
        _thread = std::thread([this] { run(); });
    }

    IngestEssence::~IngestEssence()
    {
        {
            std::unique_lock lock{_mutex};
            ++_quiesce;
            _stop = true;
        }
        _cv.notify_all();
        _thread.join();
        std::unique_lock lock{_mutex};
        // Sessions first: once MTL has freed its frames no conversion can target a grain any more.
        _videoRx.reset();
        _audioRx.reset();
        _ancRx.reset();
        releaseWriterLocked();
        finishRetiredLocked(true);
    }

    std::size_t IngestEssence::grainBytes() const
    {
        return _spec.type == config::EssenceType::Video ? _spec.group.video.at(_spec.index).format.grainBytes() : 0;
    }

    void IngestEssence::setReceiver(RtpTarget const& target)
    {
        std::unique_lock lock{_mutex};
        Quiesce const quiesce(*this, lock);
        bool const wasActive = _receiver.masterEnable;
        bool const legsChanged = !(_receiver.legs == target.legs);
        _receiver = target;
        if (!target.masterEnable)
        {
            if (wasActive)
            {
                _videoRx.reset();
                _audioRx.reset();
                _ancRx.reset();
                finishRetiredLocked(true);
                log::info("ingest_receiver_stopped", {{"group", _spec.group.label}, {"essence", _label}});
            }
            updateStateLocked();
            publishLocked();
            return;
        }
        bool const haveSession = _videoRx || _audioRx || _ancRx;
        if (haveSession && legsChanged)
        {
            bool ok = false;
            if (_videoRx)
            {
                ok = _videoRx->updateSource(target.legs);
            }
            else if (_audioRx)
            {
                ok = _audioRx->updateSource(target.legs);
            }
            else if (_ancRx)
            {
                ok = _ancRx->updateSource(target.legs);
            }
            log::info("ingest_source_updated", {{"group", _spec.group.label}, {"essence", _label}, {"ok", ok}});
            if (ok)
            {
                return;
            }
            _videoRx.reset();
            _audioRx.reset();
            _ancRx.reset();
            finishRetiredLocked(true);
        }
        if (_videoRx || _audioRx || _ancRx)
        {
            return;
        }
        try
        {
            switch (_spec.type)
            {
                case config::EssenceType::Video:
                {
                    auto const& e = _spec.group.video.at(_spec.index);
                    media::VideoRxParams p{_label, e.format, payloadTypeFor(target, e.payloadType), target.legs};
                    _expectedTag = _nextTag.load();
                    _videoRx = _backend.createVideoRx(p, *this);
                    break;
                }
                case config::EssenceType::Audio:
                {
                    auto const& e = _spec.group.audio.at(_spec.index);
                    _audioRx = _backend.createAudioRx({_label, e.format, payloadTypeFor(target, e.payloadType), target.legs});
                    break;
                }
                case config::EssenceType::Anc:
                {
                    auto const& e = _spec.group.anc.at(_spec.index);
                    _ancRx = _backend.createAncRx({_label, e.format, payloadTypeFor(target, e.payloadType), target.legs});
                    break;
                }
            }
            _lastFrameSteadyNs = steadyNs();
            _framesSeen = false;
            log::info("ingest_receiver_started", {{"group", _spec.group.label},
                                                  {"essence", _label},
                                                  {"payload_type", payloadTypeFor(target, commonOf(_spec.group, _spec.type, _spec.index).payloadType)}});
        }
        catch (std::exception const& ex)
        {
            log::error("ingest_receiver_failed", {{"group", _spec.group.label}, {"essence", _label}, {"error", ex.what()}});
            _state.set(EssenceState::Error, "rx_session_failed");
            return;
        }
        updateStateLocked();
        publishLocked();
        _cv.notify_all();
    }

    void IngestEssence::setSender(MxlSenderTarget const& target)
    {
        std::unique_lock lock{_mutex};
        Quiesce const quiesce(*this, lock);
        if (target.masterEnable == _senderEnabled && (_writer || !target.masterEnable))
        {
            return;
        }
        _senderEnabled = target.masterEnable;
        if (target.masterEnable)
        {
            openWriterLocked();
        }
        else
        {
            _reopenPending = false;
            releaseWriterLocked();
        }
        updateStateLocked();
        publishLocked();
        _cv.notify_all();
    }

    void IngestEssence::openWriterLocked()
    {
        if (_retired)
        {
            // MXL caches writers per flow id (Instance::createFlowWriter): wait until the old one is gone.
            _reopenPending = true;
            return;
        }
        _writerError.clear();
        try
        {
            auto w = std::make_unique<Writer>();
            std::vector<std::string> mismatch;
            nlohmann::json existing;
            if (_spec.type == config::EssenceType::Audio)
            {
                w->samples = std::make_unique<mxlbridge::SampleWriter>(_domain, _flowDef, _writerOptions);
                if (!w->samples->created())
                {
                    existing = w->samples->existingFlowDef();
                    mismatch = mxlbridge::compareAudio(existing, _spec.group.audio.at(_spec.index).format);
                }
            }
            else
            {
                w->grains = std::make_unique<mxlbridge::GrainWriter>(_domain, _flowDef, _writerOptions);
                if (!w->grains->created())
                {
                    existing = w->grains->existingFlowDef();
                    mismatch = _spec.type == config::EssenceType::Video ? mxlbridge::compareVideo(existing, _spec.group.video.at(_spec.index).format)
                                                                        : mxlbridge::compareAnc(existing, _spec.group.anc.at(_spec.index).format);
                }
            }
            if (!mismatch.empty())
            {
                // §8.4: an existing flow is never re-used silently with a different format.
                log::error("flow_def_mismatch",
                           {{"group", _spec.group.label}, {"essence", _label}, {"flow_id", _ids.flow.toString()}, {"differences", mismatch}});
                _writerError = "flow_def_mismatch";
                return;
            }
            _lastCommitted.store(noIndex);
            _consecutiveRejects = 0;
            _writer = std::move(w);
            _gateWriter.store(_writer.get(), std::memory_order_seq_cst);
            log::info("mxl_writer_started",
                      {{"group", _spec.group.label}, {"essence", _label}, {"flow_id", _ids.flow.toString()}, {"domain", _domain->path()}});
        }
        catch (std::exception const& ex)
        {
            log::error("mxl_writer_failed", {{"group", _spec.group.label}, {"essence", _label}, {"error", ex.what()}});
            _writerError = "writer_failed";
        }
    }

    void IngestEssence::retireWriterLocked()
    {
        if (!_writer)
        {
            return;
        }
        // Close the gate: no new grain is opened on the lcore, then wait for an acquire() in progress.
        _gateWriter.store(nullptr, std::memory_order_seq_cst);
        while (_gateUsers.load(std::memory_order_seq_cst) != 0)
        {
            std::this_thread::yield();
        }
        finishRetiredLocked(true);
        if (_videoRx && _grainOpen.load())
        {
            // MTL may still convert into the open grain (get_frame): keep the mapping alive until the
            // worker has seen every frame issued so far.
            _retired = std::move(_writer);
            _retiredUntilTag = _nextTag.load();
            return;
        }
        if (_grainOpen.exchange(false) && _writer->grains)
        {
            _writer->grains->cancel();
        }
        _writer.reset();
    }

    void IngestEssence::finishRetiredLocked(bool force)
    {
        if (!_retired)
        {
            return;
        }
        if (force || !_videoRx || _expectedTag >= _retiredUntilTag)
        {
            if (_grainOpen.exchange(false) && _retired->grains)
            {
                _retired->grains->cancel();
            }
            _retired.reset();
            if (_reopenPending && _senderEnabled && !_stop)
            {
                _reopenPending = false;
                openWriterLocked();
            }
        }
    }

    void IngestEssence::publishLocked()
    {
        std::lock_guard const lock{_snapMutex};
        _rxActive = _receiver.masterEnable;
        _writerActive = _writer != nullptr;
        if (_videoRx)
        {
            _rxStats = _videoRx->stats();
        }
        else if (_audioRx)
        {
            _rxStats = _audioRx->stats();
        }
        else if (_ancRx)
        {
            _rxStats = _ancRx->stats();
        }
        else
        {
            _rxStats = {};
        }
    }

    void IngestEssence::releaseWriterLocked()
    {
        if (_writer)
        {
            log::info("mxl_writer_stopped", {{"group", _spec.group.label}, {"essence", _label}, {"flow_id", _ids.flow.toString()}});
        }
        retireWriterLocked();
    }

    std::uint8_t* IngestEssence::acquire(media::FrameMeta const& meta, std::uint64_t& tag) noexcept
    {
        // REAL-TIME context: no locks, no allocation, no synchronous logging.
        auto const t = _nextTag.fetch_add(1, std::memory_order_relaxed);
        tag = t;
        auto& slot = _slots[t % slotCount];
        auto const ref = meta.receiveTai != 0 ? meta.receiveTai : media::hostTaiNs();
        slot.originTai = timing::unwrapRtp(meta.rtpTimestamp, timing::videoClockHz, ref);
        slot.index = timing::timestampToIndex(_grainRate, slot.originTai);
        slot.payload = nullptr;

        _gateUsers.fetch_add(1, std::memory_order_seq_cst);
        auto* writer = _gateWriter.load(std::memory_order_seq_cst);
        std::uint8_t* dst = _scratch.data();
        if (writer == nullptr || !writer->grains)
        {
            slot.kind = SlotKind::Discard;
        }
        else if (auto const last = _lastCommitted.load(std::memory_order_acquire); last != noIndex && slot.index <= last)
        {
            slot.kind = SlotKind::Reject;
        }
        else if (!_grainOpen.exchange(true, std::memory_order_acq_rel))
        {
            if (writer->grains->open(slot.index, slot.info, slot.payload) == MXL_STATUS_OK)
            {
                slot.kind = SlotKind::Grain;
                dst = slot.payload;
            }
            else
            {
                _grainOpen.store(false, std::memory_order_release);
                slot.kind = SlotKind::Reject;
            }
        }
        else
        {
            slot.kind = SlotKind::Copy;
        }
        slot.tag.store(t, std::memory_order_release);
        _gateUsers.fetch_sub(1, std::memory_order_seq_cst);
        return dst;
    }

    void IngestEssence::noteOrigin(std::int64_t originTai)
    {
        auto const age = media::hostTaiNs() - originTai;
        _originAgeNs.store(age, std::memory_order_relaxed);
        _haveOriginAge.store(true, std::memory_order_relaxed);
        // §5.6: outside [-history/2, +history/2] the source is not locked to our time base.
        std::int64_t historyNs = 200'000'000;
        if (_writer && _writer->grains && _grainRate.num > 0)
        {
            historyNs = static_cast<std::int64_t>(_writer->grains->grainCount()) * 1'000'000'000LL * _grainRate.den / _grainRate.num;
        }
        bool const drift = age > historyNs / 2 || age < -historyNs / 2;
        if (drift != _drift)
        {
            _drift = drift;
            if (drift)
            {
                log::warn("source_clock_drift", {{"group", _spec.group.label}, {"essence", _label}, {"origin_age_ns", age}});
            }
        }
    }

    void IngestEssence::noteRejected()
    {
        _writeErrors.fetch_add(1, std::memory_order_relaxed);
        ++_consecutiveRejects;
        std::uint64_t limit = 10;
        if (_writer && _writer->grains)
        {
            limit = std::max<std::uint64_t>(limit, _writer->grains->grainCount());
        }
        else if (_writer && _writer->samples && _spec.type == config::EssenceType::Audio)
        {
            auto const n = static_cast<std::uint64_t>(std::max(1, _spec.group.audio.at(_spec.index).format.samplesPerBlock()));
            limit = std::max<std::uint64_t>(limit, _writer->samples->bufferLength() / 2 / n);
        }
        if (_consecutiveRejects >= limit && _writer)
        {
            // §5.6: timestamps jumped backwards and stayed behind for a whole history — re-create the writer.
            log::warn("mxl_writer_resync",
                      {{"group", _spec.group.label}, {"essence", _label}, {"flow_id", _ids.flow.toString()}, {"rejected", _consecutiveRejects}});
            _writerResyncs.fetch_add(1, std::memory_order_relaxed);
            retireWriterLocked();
            openWriterLocked();
            _consecutiveRejects = 0;
        }
    }

    void IngestEssence::processVideo(media::FrameMeta const& meta)
    {
        auto const t = meta.tag;
        // Frames the backend dropped after acquire(): cancel a grain left open for them.
        for (auto u = _expectedTag; u < t && t - u < slotCount; ++u)
        {
            auto& skipped = _slots[u % slotCount];
            if (skipped.tag.load(std::memory_order_acquire) == u && skipped.kind == SlotKind::Grain)
            {
                auto* w = _retired ? _retired.get() : _writer.get();
                if (w != nullptr && w->grains)
                {
                    w->grains->cancel();
                }
                _grainOpen.store(false, std::memory_order_release);
                _framesDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        _expectedTag = std::max(_expectedTag, t + 1);
        auto& slot = _slots[t % slotCount];
        if (slot.tag.load(std::memory_order_acquire) != t)
        {
            _framesDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        noteOrigin(slot.originTai);
        auto const flags = meta.complete ? 0u : static_cast<std::uint32_t>(MXL_GRAIN_FLAG_INVALID);
        switch (slot.kind)
        {
            case SlotKind::Grain:
            {
                auto* w = _retired ? _retired.get() : _writer.get();
                if (w == nullptr || !w->grains)
                {
                    _grainOpen.store(false, std::memory_order_release);
                    break;
                }
                auto info = slot.info;
                info.flags = flags;
                info.validSlices = info.totalSlices; // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/include/mxl/flow.h:126-148 — complete when validSlices == totalSlices
                auto const status = w->grains->commit(info);
                _grainOpen.store(false, std::memory_order_release);
                if (status == MXL_STATUS_OK && w == _writer.get())
                {
                    _lastCommitted.store(slot.index, std::memory_order_release);
                    _grainsWritten.fetch_add(1, std::memory_order_relaxed);
                    _consecutiveRejects = 0;
                }
                else if (status != MXL_STATUS_OK)
                {
                    _writeErrors.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            case SlotKind::Copy:
            {
                if (!_writer || !_writer->grains || _grainOpen.exchange(true, std::memory_order_acq_rel))
                {
                    _framesDropped.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
                auto const last = _lastCommitted.load(std::memory_order_acquire);
                if (last != noIndex && slot.index <= last)
                {
                    _grainOpen.store(false, std::memory_order_release);
                    noteRejected();
                    break;
                }
                mxlGrainInfo info{};
                std::uint8_t* payload = nullptr;
                auto status = _writer->grains->open(slot.index, info, payload);
                if (status == MXL_STATUS_OK)
                {
                    std::memcpy(payload, _scratch.data(), std::min<std::size_t>(_scratch.size(), info.grainSize));
                    info.flags = flags;
                    info.validSlices = info.totalSlices;
                    status = _writer->grains->commit(info);
                }
                _grainOpen.store(false, std::memory_order_release);
                if (status == MXL_STATUS_OK)
                {
                    _lastCommitted.store(slot.index, std::memory_order_release);
                    _grainsWritten.fetch_add(1, std::memory_order_relaxed);
                    _consecutiveRejects = 0;
                }
                else
                {
                    noteRejected();
                }
                break;
            }
            case SlotKind::Reject: noteRejected(); break;
            case SlotKind::Discard:
            case SlotKind::Empty: break;
        }
        finishRetiredLocked(false);
    }

    void IngestEssence::processAudio(media::AudioBlock const& block)
    {
        auto const& f = _spec.group.audio.at(_spec.index).format;
        auto const ref = block.meta.receiveTai != 0 ? block.meta.receiveTai : media::hostTaiNs();
        auto const first = timing::unwrapRtpTicks(block.meta.rtpTimestamp, f.sampleRate, ref);
        noteOrigin(timing::taiOfTicks(first, f.sampleRate));
        if (!_writer || !_writer->samples || first < 0 || block.pcm == nullptr)
        {
            return;
        }
        auto& w = *_writer->samples;
        auto const bytesPerFrame = static_cast<std::size_t>(f.channels * f.bytesPerSample());
        std::size_t done = 0;
        while (done < block.samples)
        {
            auto const count = std::min(block.samples - done, std::max<std::size_t>(1, w.maxWriteLength()));
            auto const end = static_cast<std::uint64_t>(first) + done + count;
            mxlMutableWrappedMultiBufferSlice slices{};
            // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/include/mxl/flow.h:474-488 — OpenSamples addresses the `count` samples ENDING at `index`.
            auto status = w.open(end, count, slices);
            if (status == MXL_STATUS_OK)
            {
                codec::ChannelSlices dst;
                for (int i = 0; i < 2; ++i)
                {
                    dst.fragments[i].pointer = slices.base.fragments[i].pointer;
                    dst.fragments[i].size = slices.base.fragments[i].size;
                }
                dst.stride = slices.stride;
                dst.count = slices.count;
                codec::pcmToFloat(block.pcm + done * bytesPerFrame, count, f.channels, f.bitDepth, dst);
                status = w.commit();
            }
            if (status != MXL_STATUS_OK)
            {
                noteRejected();
                return;
            }
            _samplesWritten.fetch_add(count, std::memory_order_relaxed);
            _consecutiveRejects = 0;
            done += count;
        }
    }

    void IngestEssence::processAnc(media::AncReceived const& anc)
    {
        auto const ref = anc.meta.receiveTai != 0 ? anc.meta.receiveTai : media::hostTaiNs();
        auto const origin = timing::unwrapRtp(anc.meta.rtpTimestamp, timing::videoClockHz, ref);
        noteOrigin(origin);
        if (anc.droppedPackets > 0)
        {
            _framesDropped.fetch_add(anc.droppedPackets, std::memory_order_relaxed);
        }
        if (!_writer || !_writer->grains)
        {
            return;
        }
        auto const index = timing::timestampToIndex(_grainRate, origin);
        auto body = codec::serialiseGrain(anc.frame, config::ancGrainBytes);
        if (!body)
        {
            _writeErrors.fetch_add(1, std::memory_order_relaxed);
            body = codec::serialiseGrain(codec::AncFrame{anc.frame.field, {}}, config::ancGrainBytes);
        }
        // A data grain has 4096 one-byte slices; it is always committed complete (§6.3).
        auto const status = _writer->grains->write(index, *body, 0);
        if (status == MXL_STATUS_OK)
        {
            _grainsWritten.fetch_add(1, std::memory_order_relaxed);
            _consecutiveRejects = 0;
        }
        else
        {
            noteRejected();
        }
    }

    void IngestEssence::updateStateLocked()
    {
        if (!_writerError.empty())
        {
            _state.set(EssenceState::Error, _writerError);
            return;
        }
        if (!_receiver.masterEnable)
        {
            _state.set(EssenceState::Idle, _receiver.inactiveReason.empty() ? "receiver_inactive" : _receiver.inactiveReason);
            return;
        }
        if (!(_videoRx || _audioRx || _ancRx))
        {
            _state.set(EssenceState::Error, "rx_session_failed");
            return;
        }
        if (!_framesSeen || steadyNs() - _lastFrameSteadyNs > noSignalAfterNs)
        {
            _state.set(EssenceState::NoSignal, "no_packets");
            return;
        }
        if (!_senderEnabled)
        {
            _state.set(EssenceState::Idle, "sender_inactive");
            return;
        }
        if (_drift)
        {
            _state.set(EssenceState::Degraded, "source_clock_drift");
            return;
        }
        _state.set(EssenceState::Running);
    }

    void IngestEssence::run()
    {
        util::setThreadName("ingest-" + _label.substr(0, 8));
        if (!_spec.appCpus.empty())
        {
            if (auto const cpus = util::parseCpuList(_spec.appCpus))
            {
                util::pinToCpus(*cpus);
            }
        }
        util::tryRealtime(10);
        std::unique_lock lock{_mutex};
        auto lastState = steadyNs();
        while (!_stop)
        {
            auto* video = _videoRx.get();
            auto* audio = _audioRx.get();
            auto* anc = _ancRx.get();
            if (_quiesce > 0 || (video == nullptr && audio == nullptr && anc == nullptr))
            {
                _cv.wait_for(lock, std::chrono::milliseconds(200));
                continue;
            }
            _inNext = true;
            lock.unlock();
            std::optional<media::FrameMeta> frame;
            std::optional<media::AudioBlock> block;
            std::optional<media::AncReceived> data;
            if (video != nullptr)
            {
                frame = video->next(workerPoll);
            }
            else if (audio != nullptr)
            {
                block = audio->next(workerPoll);
            }
            else
            {
                data = anc->next(workerPoll);
            }
            lock.lock();
            _inNext = false;
            if (_quiesce > 0)
            {
                _cv.notify_all();
            }
            // The control thread waits for !_inNext and needs _mutex, so the sessions are unchanged here.
            bool const got = frame || block || data;
            if (frame)
            {
                processVideo(*frame);
                video->release();
            }
            else if (block)
            {
                processAudio(*block);
                audio->release();
            }
            else if (data)
            {
                processAnc(*data);
            }
            auto const now = steadyNs();
            if (got)
            {
                _lastFrameSteadyNs = now;
                _framesSeen = true;
                if (_state.get().state != EssenceState::Running)
                {
                    updateStateLocked();
                }
            }
            if (now - lastState > 250'000'000)
            {
                updateStateLocked();
                publishLocked();
                lastState = now;
            }
        }
    }

    EssenceSnapshot IngestEssence::snapshot() const
    {
        EssenceSnapshot s;
        s.groupUid = _spec.group.uid;
        s.groupLabel = _spec.group.label;
        s.uid = _uid;
        s.label = _label;
        s.type = _spec.type;
        s.direction = config::Direction::Ingest;
        s.state = _state.get();
        s.flowId = _ids.flow;
        {
            std::lock_guard const lock{_snapMutex};
            s.receiverActive = _rxActive;
            s.senderActive = _writerActive;
            s.rx = _rxStats;
        }
        s.grainsWritten = _grainsWritten.load(std::memory_order_relaxed);
        s.samplesWritten = _samplesWritten.load(std::memory_order_relaxed);
        s.writeErrors = _writeErrors.load(std::memory_order_relaxed);
        s.writerResyncs = _writerResyncs.load(std::memory_order_relaxed);
        s.framesDropped = _framesDropped.load(std::memory_order_relaxed);
        if (_haveOriginAge.load(std::memory_order_relaxed))
        {
            s.originAgeNs = _originAgeNs.load(std::memory_order_relaxed);
        }
        return s;
    }
}
