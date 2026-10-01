// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "group/essence_state.hpp"
#include "group/pipeline_types.hpp"
#include "mtl/backend.hpp"
#include "mxlbridge/instance.hpp"
#include "nmos/ids.hpp"

namespace mxlgw::group
{
    /// Everything an ingest essence needs to know about its place in the configuration.
    struct IngestSpec
    {
        util::Uuid nodeId;
        config::Group group;
        config::EssenceType type = config::EssenceType::Video;
        std::size_t index = 0; // position within the group's list of this type
        std::string appCpus;   // optional worker affinity (nic.app_cpus)
    };

    /// Builds the IS-04 Flow body / flow_def.json of an essence (shared with the NMOS layer).
    nlohmann::json flowDefinition(util::Uuid const& nodeId, config::Group const& group, config::EssenceType type, std::size_t index,
                                  std::string const& version);
    /// "<group label>:<Role> <n>" (BCP-002-01, §7.2).
    std::string groupHint(config::Group const& group, config::EssenceType type, std::size_t index);
    ids::EssenceIds essenceIds(config::Group const& group, config::EssenceType type, std::size_t index);

    /// ST 2110 -> MXL for one essence (§3.4): MTL RX session + MXL FlowWriter + one worker thread.
    /// setReceiver()/setSender() are called from the control thread only.
    class IngestEssence : private media::VideoRxHandler
    {
    public:
        IngestEssence(IngestSpec spec, media::MediaBackend& backend, std::shared_ptr<mxlbridge::Instance> domain);
        ~IngestEssence() override;
        IngestEssence(IngestEssence const&) = delete;
        IngestEssence& operator=(IngestEssence const&) = delete;

        void setReceiver(RtpTarget const& target);
        void setSender(MxlSenderTarget const& target);
        EssenceSnapshot snapshot() const;
        util::Uuid const& uid() const { return _uid; }
        util::Uuid const& flowId() const { return _ids.flow; }
        nlohmann::json const& flowDef() const { return _flowDef; }

    private:
        enum class SlotKind : int
        {
            Empty,
            Grain,   // converted directly into an opened grain
            Copy,    // converted into scratch; worker opens + copies
            Reject,  // index not after the last committed one
            Discard, // no writer
        };
        struct Slot;
        class Writer;
        class Quiesce;

        std::uint8_t* acquire(media::FrameMeta const& meta, std::uint64_t& tag) noexcept override;
        void run();
        void processVideo(media::FrameMeta const& meta);
        void processAudio(media::AudioBlock const& block);
        void processAnc(media::AncReceived const& anc);
        void openWriterLocked();
        void releaseWriterLocked();
        void retireWriterLocked();
        void finishRetiredLocked(bool force);
        void publishLocked();
        void noteOrigin(std::int64_t originTai);
        void noteRejected();
        void updateStateLocked();
        std::size_t grainBytes() const;

        IngestSpec _spec;
        media::MediaBackend& _backend;
        std::shared_ptr<mxlbridge::Instance> _domain;
        util::Uuid _uid;
        std::string _label;
        ids::EssenceIds _ids;
        nlohmann::json _flowDef;
        std::string _writerOptions;
        util::Rational _grainRate;
        StateHolder _state;

        // Control/worker shared state (guarded by _mutex).
        mutable std::mutex _mutex;
        std::condition_variable _cv;
        bool _stop = false;
        bool _inNext = false; // worker is blocked in the session's next() without holding _mutex
        int _quiesce = 0;     // control thread wants the worker parked
        RtpTarget _receiver;
        bool _senderEnabled = false;
        std::unique_ptr<media::VideoRxSession> _videoRx;
        std::unique_ptr<media::AudioRxSession> _audioRx;
        std::unique_ptr<media::AncRxSession> _ancRx;
        std::unique_ptr<Writer> _writer;
        std::unique_ptr<Writer> _retired;
        std::uint64_t _retiredUntilTag = 0;
        std::string _writerError;
        std::int64_t _lastFrameSteadyNs = 0;
        std::uint64_t _consecutiveRejects = 0;
        bool _drift = false;
        bool _reopenPending = false;

        // Snapshot cache (never blocks on the worker's poll).
        mutable std::mutex _snapMutex;
        media::SessionStats _rxStats;
        bool _rxActive = false;
        bool _writerActive = false;

        // Real-time handoff (MTL lcore <-> worker), video only.
        std::unique_ptr<Slot[]> _slots;
        std::vector<std::uint8_t> _scratch;
        std::atomic<std::uint64_t> _nextTag{1};
        std::uint64_t _expectedTag = 1;
        std::atomic<Writer*> _gateWriter{nullptr};
        std::atomic<int> _gateUsers{0};
        std::atomic<bool> _grainOpen{false};
        std::atomic<std::uint64_t> _lastCommitted{~std::uint64_t{0}};

        // Counters (relaxed atomics, read by snapshot()).
        std::atomic<std::uint64_t> _grainsWritten{0};
        std::atomic<std::uint64_t> _samplesWritten{0};
        std::atomic<std::uint64_t> _writeErrors{0};
        std::atomic<std::uint64_t> _writerResyncs{0};
        std::atomic<std::uint64_t> _framesDropped{0};
        std::atomic<std::int64_t> _originAgeNs{0};
        std::atomic<bool> _haveOriginAge{false};

        std::thread _thread;
    };
}
