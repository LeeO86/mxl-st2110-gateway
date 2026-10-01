// SPDX-License-Identifier: MIT
// Shared by the MTL session implementations (mtl_*.cpp). Not part of the backend interface.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <mtl/mtl_api.h>
#include <mtl/st30_pipeline_api.h>
#include <mtl/st40_pipeline_api.h>
#include <mtl/st_pipeline_api.h>

#include "config/config.hpp"
#include "mtl/backend.hpp"

namespace mxlgw::media::mtlimpl
{
    /// What every session needs to know about the MTL instance.
    struct Context
    {
        mtl_handle mt = nullptr;
        bool kernel = false;
        bool redundantPort = false;
        std::array<std::string, 2> portNames; // MTL port strings ("0000:31:00.0" / "kernel:veth0")
        std::array<std::string, 2> portIps;
    };

    bool parseIp(std::string const& text, std::uint8_t out[MTL_IP_ADDR_LEN]);
    std::string ipText(std::uint8_t const ip[MTL_IP_ADDR_LEN]);

    /// Session port count and per-leg addresses (legs beyond the instance's ports are ignored).
    struct SessionLegs
    {
        int count = 1;
        std::array<LegAddress, 2> legs{};
    };
    SessionLegs sessionLegs(Context const& ctx, std::vector<LegAddress> const& legs);

    void fillRxPort(Context const& ctx, st_rx_port& port, std::vector<LegAddress> const& legs, int payloadType);
    void fillTxPort(Context const& ctx, st_tx_port& port, std::vector<LegAddress> const& legs, int payloadType);
    st_rx_source_info rxSource(Context const& ctx, std::vector<LegAddress> const& legs);
    st_tx_dest_info txDestination(Context const& ctx, std::vector<LegAddress> const& legs);

    /// MTL fps for a grain rate (field rate for interlaced, MTL doc/design.md "FPS is fields per second").
    enum st_fps fpsOf(util::Rational grainRate);

    /// st_rx_user_stats/st_tx_user_stats -> SessionStats.
    SessionStats rxStats(st_rx_user_stats const& common, std::uint64_t incomplete);
    SessionStats txStats(st_tx_user_stats const& common);

    /// Frames queued ahead of their transmit time; bounded for MTL.
    std::uint16_t queueDepth(int requested, int fallback, int maximum);
}
