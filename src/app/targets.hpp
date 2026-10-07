// SPDX-License-Identifier: MIT
#pragma once

#include <optional>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "group/pipeline_types.hpp"

namespace mxlgw::app
{
    /// IS-05 /active of an rtp.mcast Receiver -> ingest RX legs (§7.5). interface_ip is ignored (DPDK
    /// port IP); a disabled leg keeps rtp_enabled=false.
    group::RtpTarget rtpReceiverTarget(nlohmann::json const& active);
    /// Payload type of the first a=rtpmap line of an SDP, 0 without one.
    int sdpPayloadType(std::string const& sdp);
    /// IS-05 /active of an rtp.mcast Sender -> egress TX legs.
    group::RtpTarget rtpSenderTarget(nlohmann::json const& active);
    group::MxlSenderTarget mxlSenderTarget(nlohmann::json const& active);
    group::MxlReceiverTarget mxlReceiverTarget(nlohmann::json const& active);

    /// Default IS-05 transport parameters from the essence configuration (§7.6 first start).
    nlohmann::json defaultRtpReceiverParams(config::EssenceCommon const& essence, bool redundant);
    nlohmann::json defaultRtpSenderParams(config::EssenceCommon const& essence, bool redundant, config::PortPair const& ports);
}
