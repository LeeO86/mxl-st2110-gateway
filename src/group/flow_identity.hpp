// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "nmos/ids.hpp"

namespace mxlgw::group
{
    /// "<group label>:<Role> <n>" (BCP-002-01, §7.2; roles Video, Audio, Data, numbered per type from 1).
    std::string groupHint(config::Group const& group, config::EssenceType type, std::size_t index);
    ids::EssenceIds essenceIds(config::Group const& group, config::EssenceType type, std::size_t index);
    /// IS-04 Flow body that is also the MXL flow_def.json of an ingest essence (§7.2). Shared by the
    /// writer and the NMOS layer so both describe the flow identically.
    nlohmann::json flowDefinition(util::Uuid const& nodeId, config::Group const& group, config::EssenceType type, std::size_t index,
                                  std::string const& version);
}
