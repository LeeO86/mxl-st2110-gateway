// SPDX-License-Identifier: MIT
#include "mtl/ethdev.hpp"

#include <cstdio>

#include <rte_ethdev.h>

namespace mxlgw::media::ethdev
{
    PortInfo query(std::string const& pciName)
    {
        PortInfo info;
        std::uint16_t portId = 0;
        if (rte_eth_dev_get_port_by_name(pciName.c_str(), &portId) != 0)
        {
            return info;
        }
        info.found = true;
        rte_ether_addr mac{};
        if (rte_eth_macaddr_get(portId, &mac) == 0)
        {
            char buf[RTE_ETHER_ADDR_FMT_SIZE];
            std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac.addr_bytes[0], mac.addr_bytes[1], mac.addr_bytes[2], mac.addr_bytes[3],
                          mac.addr_bytes[4], mac.addr_bytes[5]);
            info.mac = buf;
        }
        rte_eth_link link{};
        if (rte_eth_link_get_nowait(portId, &link) == 0)
        {
            info.linkUp = link.link_status == RTE_ETH_LINK_UP;
            info.speedMbps = link.link_speed == RTE_ETH_SPEED_NUM_UNKNOWN ? 0 : link.link_speed;
        }
        rte_eth_dev_info dev{};
        if (rte_eth_dev_info_get(portId, &dev) == 0 && dev.driver_name != nullptr)
        {
            info.driver = dev.driver_name;
        }
        return info;
    }
}
