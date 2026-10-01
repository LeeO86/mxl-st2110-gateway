// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mxlgw::ops
{
    /// Human-readable plain-text rendering of /api/status for /statusz (mxl-decklink parity, §10).
    std::string renderStatusz(nlohmann::json const& status);
}
