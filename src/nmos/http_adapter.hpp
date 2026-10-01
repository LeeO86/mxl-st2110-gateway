// SPDX-License-Identifier: MIT
// cpprest <-> ops::Router (C++17, nmos-cpp side).
#pragma once

#include "cpprest/api_router.h"
#include "nmos/settings.h"
#include "ops/http.hpp"

namespace slog
{
    class base_gate;
}

namespace mxlgw::nmosnode
{
    /// Mounts every top-level prefix of `routes` (/admin, /api, /metrics, ...) on `api`, in front of
    /// nmos-cpp's catch-all 404 handler (VERIFIED: sony/nmos-cpp@fe30384 Development/nmos/api_utils.cpp
    /// add_api_finally_handler appends a ".*" handler; cpprest/api_router.cpp pop_back removes it).
    /// `replaceFinallyHandler`: the router already ends with nmos-cpp's finally handler (after
    /// make_node_server); it is removed and re-added behind the gateway routes. Otherwise the caller adds
    /// it (make_api_listener does).
    void mountGatewayRoutes(web::http::experimental::listener::api_router& api, ops::Router const& routes, nmos::settings const& settings,
                            slog::base_gate& gate, bool replaceFinallyHandler);

    /// slog gate that forwards nmos-cpp messages into the gateway's structured log (component=nmos).
    slog::base_gate& nmosGate();
}
