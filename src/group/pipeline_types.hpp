// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "group/essence_state.hpp"
#include "mtl/backend.hpp"
#include "util/uuid.hpp"

namespace mxlgw::group
{
    /// IS-05 /active of an rtp.mcast Receiver (ingest) or Sender (egress).
    struct RtpTarget
    {
        bool masterEnable = false;
        std::vector<media::LegAddress> legs; // leg.enabled = rtp_enabled
        std::string inactiveReason;          // essence state reason while disabled, else "receiver_inactive"
        bool operator==(RtpTarget const& o) const { return masterEnable == o.masterEnable && legs == o.legs && inactiveReason == o.inactiveReason; }
    };

    /// IS-05 /active of an MXL Sender (ingest).
    struct MxlSenderTarget
    {
        bool masterEnable = false;
    };

    /// IS-05 /active of an MXL Receiver (egress); `auto` is resolved before it gets here (§7.4).
    struct MxlReceiverTarget
    {
        bool masterEnable = false;
        std::optional<util::Uuid> domainId;
        std::optional<util::Uuid> flowId;
        std::string inactiveReason; // essence state reason while disabled, else "receiver_inactive"
        bool operator==(MxlReceiverTarget const& o) const
        {
            return masterEnable == o.masterEnable && domainId == o.domainId && flowId == o.flowId && inactiveReason == o.inactiveReason;
        }
    };

    /// Resolved domain of an enabled MXL Receiver (`mxl_st2110_gateway_mxl_reader_info`, §12.1).
    struct ReaderInfo
    {
        util::Uuid domainId;
        std::string domainPath;
        std::string domainKind; // configured | discovered | mirror
        util::Uuid flowId;
    };

    /// Point-in-time view of one essence for /api/status and /metrics.
    struct EssenceSnapshot
    {
        util::Uuid groupUid;
        std::string groupLabel;
        util::Uuid uid;
        std::string label;
        config::EssenceType type = config::EssenceType::Video;
        config::Direction direction = config::Direction::Ingest;
        EssenceStatus state;
        util::Uuid flowId; // ingest: MXL flow written; egress: staged flow (nil if none)

        // ingest
        bool receiverActive = false; // ST 2110 RX
        bool senderActive = false;   // MXL writer
        media::SessionStats rx;
        std::uint64_t grainsWritten = 0;
        std::uint64_t samplesWritten = 0;
        std::uint64_t writeErrors = 0;
        std::uint64_t writerResyncs = 0;
        std::uint64_t framesDropped = 0; // gateway-side drops (busy writer, no slot)
        std::optional<std::int64_t> originAgeNs;

        // egress
        bool mxlReceiverActive = false;
        bool rtpSenderActive = false;
        media::SessionStats tx;
        std::uint64_t grainsRead = 0;
        std::uint64_t readTimeouts = 0;
        std::uint64_t lateReads = 0;
        std::uint64_t grainsInvalid = 0;
        std::uint64_t flowNotFound = 0;
        std::uint64_t txDropped = 0;
        std::optional<double> readLagGrains;
        std::optional<std::int64_t> leadNs;
        std::optional<ReaderInfo> reader;
        std::int64_t readOffsetNs = 0;
    };

    struct GroupSnapshot
    {
        util::Uuid uid;
        std::string label;
        config::Direction direction = config::Direction::Ingest;
        bool enabled = true;
        std::string domain;
        std::int64_t outputDelayNs = 0;
        std::vector<EssenceSnapshot> essences;
    };
}
