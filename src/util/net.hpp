// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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
}
