// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "mtl/sdp_map.hpp"

namespace mxlgw::nmosnode
{
    /// Parses an ST 2110 SDP with nmos-cpp and maps the first media description to SdpMedia (§7.5).
    /// Throws std::exception (nmos-cpp sdp_exception / json_exception) for a malformed SDP.
    sdpmap::SdpMedia parseSdpMedia(std::string const& sdpText);
}
