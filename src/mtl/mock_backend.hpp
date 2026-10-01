// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "config/config.hpp"
#include "mtl/backend.hpp"

namespace mxlgw::media
{
    /// Test-only backend (nic.backend = mock): no network. TX sessions deliver frames to RX sessions of the
    /// same process whose group address and port match, at the frame's transmit time (CLOCK_TAI). Lets the
    /// whole gateway (NMOS, MXL, egress/ingest pipelines, UI) run without MTL, DPDK or hugepages.
    std::unique_ptr<MediaBackend> createMockBackend(config::Config const& config);
}
