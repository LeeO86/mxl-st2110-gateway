// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "codec/anc8331.hpp"
#include "config/config.hpp"
#include "timing/ptp.hpp"

namespace mxlgw::media
{
    /// One ST 2022-7 leg. RX: `destination` = multicast group (or unicast address), `source` = SSM filter.
    /// TX: `destination` = destination address, `source` = sending port IP (informational).
    struct LegAddress
    {
        std::string destination;
        std::string source;
        int port = 0;
        bool enabled = true;

        bool operator==(LegAddress const& o) const { return destination == o.destination && source == o.source && port == o.port && enabled == o.enabled; }
        bool operator!=(LegAddress const& o) const { return !(*this == o); }
    };

    struct LegStats
    {
        std::uint64_t packets = 0;
        std::uint64_t bytes = 0;
        std::uint64_t lost = 0;
    };

    struct SessionStats
    {
        std::array<LegStats, 2> legs{};
        std::uint64_t packets = 0; // after 2022-7 merge (RX) / sent (TX)
        std::uint64_t framesComplete = 0;
        std::uint64_t framesIncomplete = 0;
        std::uint64_t framesDropped = 0;
        std::uint64_t framesLate = 0; // TX: deadline missed
    };

    struct FrameMeta
    {
        std::uint32_t rtpTimestamp = 0;
        std::int64_t receiveTai = 0; // TAI ns of the first packet (RX)
        bool complete = true;
        bool secondField = false;
        std::uint32_t pktsTotal = 0;
        std::array<std::uint32_t, 2> pktsRecv{};
    };

    // ------------------------------------------------------------------ video
    struct VideoRxParams
    {
        std::string name;
        config::VideoFormat format;
        int payloadType = 96;
        std::vector<LegAddress> legs;
    };

    /// Ingest video: the backend converts RFC 4175 into the buffer returned by acquire().
    class VideoRxHandler
    {
    public:
        virtual ~VideoRxHandler() = default;
        /// REAL-TIME context (MTL lcore): must not block, allocate or log synchronously.
        /// Returns the destination of the v210 conversion (stride = format.v210Stride(),
        /// size = format.grainBytes()) or nullptr to drop the frame.
        virtual std::uint8_t* acquire(FrameMeta const& meta) noexcept = 0;
    };

    class VideoRxSession
    {
    public:
        virtual ~VideoRxSession() = default;
        /// Blocks up to `timeout` for the next converted frame (worker thread).
        virtual std::optional<FrameMeta> next(std::chrono::nanoseconds timeout) = 0;
        /// Returns the frame of the last next() to the backend.
        virtual void release() = 0;
        virtual bool updateSource(std::vector<LegAddress> const& legs) = 0;
        virtual SessionStats stats() const = 0;
    };

    struct VideoTxParams
    {
        std::string name;
        config::VideoFormat format;
        int payloadType = 96;
        config::Pacing pacing = config::Pacing::Narrow;
        config::Packing packing = config::Packing::Bpm;
        std::vector<LegAddress> legs;
    };

    class VideoTxSession
    {
    public:
        virtual ~VideoTxSession() = default;
        /// Queues one frame/field (v210, MXL stride) for transmission at `transmitTai`; RTP timestamp
        /// derived from `transmitTai` (§5.4). `v210` must stay valid until the next send() of this session.
        /// Returns false if no frame slot was free within `timeout`.
        virtual bool send(std::uint8_t const* v210, std::int64_t transmitTai, bool secondField, std::chrono::nanoseconds timeout) = 0;
        virtual bool updateDestination(std::vector<LegAddress> const& legs) = 0;
        virtual SessionStats stats() const = 0;
    };

    // ------------------------------------------------------------------ audio
    struct AudioParams
    {
        std::string name;
        config::AudioFormat format;
        int payloadType = 97;
        std::vector<LegAddress> legs;
    };

    struct AudioBlock
    {
        FrameMeta meta;
        std::uint8_t const* pcm = nullptr; // big-endian interleaved L16/L24
        std::size_t samples = 0;
    };

    class AudioRxSession
    {
    public:
        virtual ~AudioRxSession() = default;
        virtual std::optional<AudioBlock> next(std::chrono::nanoseconds timeout) = 0;
        virtual void release() = 0;
        virtual bool updateSource(std::vector<LegAddress> const& legs) = 0;
        virtual SessionStats stats() const = 0;
    };

    class AudioTxSession
    {
    public:
        virtual ~AudioTxSession() = default;
        /// Gets a block buffer to fill (`samplesPerBlock` × channels × bytes) or nullptr on timeout.
        virtual std::uint8_t* acquire(std::chrono::nanoseconds timeout) = 0;
        /// Sends the acquired block at `transmitTai` (RTP derived from the pacing time, §5.4).
        virtual void send(std::int64_t transmitTai) = 0;
        virtual bool updateDestination(std::vector<LegAddress> const& legs) = 0;
        virtual SessionStats stats() const = 0;
    };

    // ------------------------------------------------------------------ ANC
    struct AncParams
    {
        std::string name;
        config::AncFormat format;
        int payloadType = 100;
        std::vector<LegAddress> legs;
    };

    struct AncReceived
    {
        FrameMeta meta;
        codec::AncFrame frame;
        std::size_t droppedPackets = 0; // beyond the backend's per-frame limit
    };

    class AncRxSession
    {
    public:
        virtual ~AncRxSession() = default;
        virtual std::optional<AncReceived> next(std::chrono::nanoseconds timeout) = 0;
        virtual bool updateSource(std::vector<LegAddress> const& legs) = 0;
        virtual SessionStats stats() const = 0;
    };

    class AncTxSession
    {
    public:
        virtual ~AncTxSession() = default;
        /// Returns the number of packets that did not fit the backend's per-frame limit (20 in MTL).
        virtual std::size_t send(codec::AncFrame const& frame, std::int64_t transmitTai, bool secondField, std::chrono::nanoseconds timeout) = 0;
        virtual bool updateDestination(std::vector<LegAddress> const& legs) = 0;
        virtual SessionStats stats() const = 0;
    };

    // ------------------------------------------------------------------ backend
    struct PortStatus
    {
        std::string name;
        std::string pci;
        std::string ifname;
        std::string mac;
        std::string ip;
        bool linkUp = false;
        std::uint32_t linkSpeedMbps = 0;
        std::string bindMode; // "pf", "vf", "kernel", "mock"
        std::string driver;
        std::string ddpPackage;
        std::uint64_t rxPackets = 0, txPackets = 0, rxBytes = 0, txBytes = 0, rxErrors = 0, rxMissed = 0;
    };

    struct PtpPortStatus
    {
        bool active = false;
        bool locked = false;
        bool selected = false;
        std::optional<timing::PortIdentity> parent;
        std::optional<timing::AnnounceInfo> announce;
        int domain = 0;
        int utcOffset = 37;
        std::int64_t lastDeltaNs = 0, minDeltaNs = 0, maxDeltaNs = 0, avgDeltaNs = 0;
        std::int64_t lastPathDelayNs = 0, minPathDelayNs = 0, maxPathDelayNs = 0, avgPathDelayNs = 0;
        std::uint64_t syncCount = 0;
        std::uint64_t gmChanges = 0;
        std::uint64_t errorsRxSync = 0, errorsTxSync = 0, errorsResult = 0, errorsTimeout = 0;
    };

    struct BackendStatus
    {
        std::string backend; // dpdk / kernel / mock
        bool testBackend = false;
        std::vector<PortStatus> ports; // primary first
        std::array<PtpPortStatus, 2> ptp{};
        bool ptpAvailable = false; // false: external / no patched API
        std::uint64_t ptpSelectionChanges = 0;
        bool phc2sysLocked = false;
        std::string mtlVersion;
    };

    class MediaBackend
    {
    public:
        virtual ~MediaBackend() = default;

        virtual std::string name() const = 0;
        /// PTP time (TAI ns) used for RX timestamps and TX pacing.
        virtual std::int64_t ptpTimeNs() const = 0;
        virtual BackendStatus status() const = 0;

        virtual std::unique_ptr<VideoRxSession> createVideoRx(VideoRxParams const& params, VideoRxHandler& handler) = 0;
        virtual std::unique_ptr<VideoTxSession> createVideoTx(VideoTxParams const& params) = 0;
        virtual std::unique_ptr<AudioRxSession> createAudioRx(AudioParams const& params) = 0;
        virtual std::unique_ptr<AudioTxSession> createAudioTx(AudioParams const& params) = 0;
        virtual std::unique_ptr<AncRxSession> createAncRx(AncParams const& params) = 0;
        virtual std::unique_ptr<AncTxSession> createAncTx(AncParams const& params) = 0;
    };

    /// Builds RX/TX legs from IS-05 transport parameters or config defaults.
    std::vector<LegAddress> legsFromConfig(std::vector<config::Leg> const& legs, bool redundant);

    /// Current host CLOCK_TAI in ns.
    std::int64_t hostTaiNs();
    /// Sleeps until CLOCK_TAI reaches `tai` (returns immediately if in the past).
    void sleepUntilTai(std::int64_t tai);
}
