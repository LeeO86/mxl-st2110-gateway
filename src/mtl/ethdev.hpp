// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace mxlgw::media::ethdev
{
    struct PortInfo
    {
        bool found = false;
        std::string mac; // "aa:bb:cc:dd:ee:ff"
        bool linkUp = false;
        std::uint32_t speedMbps = 0;
        std::string driver;
    };

    /// Read-only DPDK ethdev queries for a port MTL owns (§4.5, owner decision Q6): MTL v26.09 has no
    /// public MAC/link API, so the gateway asks DPDK directly in the same process.
    PortInfo query(std::string const& pciName);
}
