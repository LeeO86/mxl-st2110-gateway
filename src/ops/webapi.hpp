// SPDX-License-Identifier: MIT
#pragma once

#include "app/services.hpp"
#include "ops/http.hpp"

namespace mxlgw::ops
{
    /// Builds the gateway's own routes (§10, §11.3): /admin, /api, /metrics, /livez, /readyz, /statusz.
    /// They are mounted on the nmos-cpp listener so everything shares node.http_port.
    Router makeGatewayRouter(app::Services& services);

    /// Sections whose changes need a restart (§9.3) between two effective configurations.
    std::vector<std::string> restartRelevantChanges(nlohmann::json const& before, nlohmann::json const& after);
}
