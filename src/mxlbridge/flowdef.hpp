// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "util/uuid.hpp"

namespace mxlgw::mxlbridge
{
    /// Identity and labelling shared by the IS-04 Flow and the MXL flow definition (§7.2).
    struct FlowIdentity
    {
        util::Uuid flowId;
        util::Uuid sourceId;
        util::Uuid deviceId;
        std::string label;
        std::string description;
        std::string groupHint; // "<group label>:<Role> <n>" (BCP-002-01)
        std::string version;   // IS-04 "<sec>:<nsec>"
    };

    /// IS-04 v1.3 Flow body that is also written as flow_def.json (MXL requires id, format, label,
    /// tags with the grouphint, media_type and the format attributes; BCP-007-03 examples).
    nlohmann::json videoFlowDef(FlowIdentity const& id, config::VideoFormat const& format);
    nlohmann::json audioFlowDef(FlowIdentity const& id, config::AudioFormat const& format);
    nlohmann::json ancFlowDef(FlowIdentity const& id, config::AncFormat const& format);

    /// Compares an existing flow_def.json with the essence format (§6.4 / §8.4). Empty = match.
    std::vector<std::string> compareVideo(nlohmann::json const& flowDef, config::VideoFormat const& format);
    std::vector<std::string> compareAudio(nlohmann::json const& flowDef, config::AudioFormat const& format);
    std::vector<std::string> compareAnc(nlohmann::json const& flowDef, config::AncFormat const& format);

    /// IS-04 version string ("<seconds>:<nanoseconds>") from TAI ns.
    std::string nmosVersion(std::int64_t taiNs);

    /// Writer options (MXL FlowOptionsParser): maxCommitBatchSizeHint / maxSyncBatchSizeHint.
    std::string writerOptions(std::uint32_t commitBatchHint, std::uint32_t syncBatchHint);
}
