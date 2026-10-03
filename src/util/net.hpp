// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mxlgw::util
{
    struct Ipv4
    {
        std::uint32_t value = 0; // host byte order

        std::array<std::uint8_t, 4> octets() const
        {
            return {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                    static_cast<std::uint8_t>(value)};
        }
        std::string toString() const;
        bool operator==(Ipv4 const& o) const { return value == o.value; }
        bool operator!=(Ipv4 const& o) const { return value != o.value; }
    };

    std::optional<Ipv4> parseIpv4(std::string_view text);
    bool isMulticast(Ipv4 ip);
    /// True if `mask` is a contiguous netmask.
    bool isNetmask(Ipv4 mask);
    bool sameSubnet(Ipv4 a, Ipv4 b, Ipv4 mask);
    int prefixLength(Ipv4 mask);
    /// An address other hosts can reach: not 0/8, 127/8, 169.254/16, multicast or broadcast.
    bool isAnnounceable(Ipv4 ip);

    struct InterfaceAddress
    {
        std::string ifname;
        std::string address;
    };

    /// IPv4 addresses of the kernel interfaces in kernel order (getifaddrs).
    std::vector<InterfaceAddress> interfaceIpv4Addresses();
    /// Interface of the default route with the lowest metric from /proc/net/route content.
    std::optional<std::string> defaultRouteInterface(std::string const& procNetRoute);
    /// The address of the default-route interface, else the first announceable address (G5).
    std::optional<std::string> pickHostAddress(std::vector<InterfaceAddress> const& addresses, std::optional<std::string> const& defaultInterface);
    /// pickHostAddress() for this host (network namespace).
    std::optional<std::string> defaultHostAddress();
}
