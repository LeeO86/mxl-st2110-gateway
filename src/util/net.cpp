// SPDX-License-Identifier: MIT
#include "util/net.hpp"

#include "util/strings.hpp"

namespace mxlgw::util
{
    std::string Ipv4::toString() const
    {
        auto const o = octets();
        return std::to_string(o[0]) + "." + std::to_string(o[1]) + "." + std::to_string(o[2]) + "." + std::to_string(o[3]);
    }

    std::optional<Ipv4> parseIpv4(std::string_view text)
    {
        auto const parts = split(text, '.', false);
        if (parts.size() != 4)
        {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (auto const& part : parts)
        {
            if (part.empty() || part.size() > 3)
            {
                return std::nullopt;
            }
            for (char const c : part)
            {
                if (c < '0' || c > '9')
                {
                    return std::nullopt;
                }
            }
            auto const v = parseInt(part);
            if (!v || *v > 255)
            {
                return std::nullopt;
            }
            value = (value << 8) | static_cast<std::uint32_t>(*v);
        }
        return Ipv4{value};
    }

    bool isMulticast(Ipv4 ip)
    {
        return (ip.value >> 28) == 0xE;
    }

    bool isNetmask(Ipv4 mask)
    {
        std::uint32_t const inverted = ~mask.value;
        return (inverted & (inverted + 1)) == 0;
    }

    bool sameSubnet(Ipv4 a, Ipv4 b, Ipv4 mask)
    {
        return (a.value & mask.value) == (b.value & mask.value);
    }

    int prefixLength(Ipv4 mask)
    {
        int n = 0;
        for (std::uint32_t v = mask.value; v & 0x80000000u; v <<= 1)
        {
            ++n;
        }
        return n;
    }
}
