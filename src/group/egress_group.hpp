// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "config/config.hpp"
#include "group/domain_resolver.hpp"
#include "group/pipeline_types.hpp"
#include "mtl/backend.hpp"
#include "mxlbridge/instance.hpp"
#include "util/logging.hpp"

namespace mxlgw::group
{
    struct EgressSpec
    {
        util::Uuid nodeId;
        config::Group group;
        /// Effective read offset per essence uid (§5.7, essence value or mxl.default_read_offset_*).
        std::map<util::Uuid, std::int64_t> readOffsetNs;
        std::string appCpus;
        /// Instance that owns the group's sync group (the group's configured domain).
        std::shared_ptr<mxlbridge::Instance> owner;
        /// Margin between the read deadline and the transmit time (MTL needs the frame before T_tx).
        std::int64_t txLeadNs = 1'000'000;
    };

    /// MXL -> ST 2110 for one group (§3.5, §5.7, §5.8). The grain worker owns the group's
    /// mxlFlowSynchronizationGroup and the video/ANC readers and TX sessions (one grain period at a time);
    /// the audio worker owns the audio readers and TX sessions (one audio block at a time, never blocking).
    /// setReceiver()/setSender()/setReadOffset() only post targets; the owning worker applies them.
    class EgressGroup
    {
    public:
        EgressGroup(EgressSpec spec, media::MediaBackend& backend, DomainResolver& resolver);
        ~EgressGroup();
        EgressGroup(EgressGroup const&) = delete;
        EgressGroup& operator=(EgressGroup const&) = delete;

        void setReceiver(util::Uuid const& essenceUid, MxlReceiverTarget const& target);
        void setSender(util::Uuid const& essenceUid, RtpTarget const& target);
        /// Live read-offset edit (§9.3); applied from the next grain.
        void setReadOffset(util::Uuid const& essenceUid, std::int64_t readOffsetNs);
        GroupSnapshot snapshot() const;
        util::Uuid const& uid() const { return _spec.group.uid; }
        /// Transmit time after the grain time, fixed for the group's lifetime (§5.7).
        std::int64_t outputDelayNs() const { return _outputDelayNs; }

    private:
        struct Essence;
        struct Mailbox;

        void run();
        void runAudio();
        /// Reconciles the essences of one worker: audio (audio worker) or video/ANC (grain worker).
        void reconcile(std::int64_t steadyNow, bool audio);
        void reconcileTx(Essence& e);
        void reconcileRx(Essence& e, std::int64_t steadyNow);
        void attach(Essence& e, Resolution const& r, std::int64_t steadyNow);
        void detach(Essence& e);
        void stopTx(Essence& e);
        void scheduleRetry(Essence& e, std::int64_t steadyNow, char const* reason);
        void processPeriod(std::uint64_t i);
        void processGrain(Essence& e, std::int64_t periodOrigin, std::int64_t deadline);
        void processAudio(Essence& e, std::int64_t now);
        void markData(Essence& e, bool good, std::int64_t steadyNow);
        void publish(bool audio);
        std::int64_t maxReadOffset() const;

        EgressSpec _spec;
        media::MediaBackend& _backend;
        DomainResolver& _resolver;
        std::shared_ptr<Mailbox> _mailbox;
        std::vector<std::unique_ptr<Essence>> _essences;
        util::Rational _rate;
        std::int64_t _cadenceNs = 0;
        std::int64_t _outputDelayNs = 0;
        /// Audio worker tick: the shortest audio block of the group (0 = no audio).
        std::int64_t _audioTickNs = 0;
        /// Guards the TX session pointers between their worker (create/reset) and snapshot() (stats): MTL's
        /// stats calls spin on a lock the MTL scheduler holds, so they never run on a real-time worker.
        mutable std::mutex _sessionMutex;
        log::RateLimiter _notFoundLog{std::chrono::seconds(30)};
        log::RateLimiter _behindLog{std::chrono::seconds(10)};
        std::atomic<std::uint64_t> _periodsLate{0};
        std::thread _thread;
        std::thread _audioThread;
    };
}
