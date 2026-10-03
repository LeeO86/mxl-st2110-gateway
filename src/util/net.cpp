// SPDX-License-Identifier: MIT
#include "util/net.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include "util/fs.hpp"
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

    bool isAnnounceable(Ipv4 ip)
    {
        auto const o = ip.octets();
        return ip.value != 0xFFFFFFFFu && o[0] != 0 && o[0] != 127 && !(o[0] == 169 && o[1] == 254) && !isMulticast(ip);
    }

    std::vector<InterfaceAddress> interfaceIpv4Addresses()
    {
        std::vector<InterfaceAddress> out;
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0)
        {
            return out;
        }
        for (auto const* a = list; a != nullptr; a = a->ifa_next)
        {
            if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET || (a->ifa_flags & IFF_UP) == 0)
            {
                continue;
            }
            char text[INET_ADDRSTRLEN] = {};
            auto const* in = reinterpret_cast<sockaddr_in const*>(a->ifa_addr);
            if (::inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) != nullptr)
            {
                out.push_back({a->ifa_name, text});
            }
        }
        ::freeifaddrs(list);
        return out;
    }

    std::optional<std::string> defaultRouteInterface(std::string const& procNetRoute)
    {
        std::optional<std::string> best;
        long bestMetric = 0;
        bool header = true;
        for (auto const& line : split(procNetRoute, '\n'))
        {
            if (header)
            {
                header = false;
                continue;
            }
            std::vector<std::string> fields;
            std::string field;
            for (char const c : line + "\t")
            {
                if (c == '\t' || c == ' ')
                {
                    if (!field.empty())
                    {
                        fields.push_back(field);
                    }
                    field.clear();
                }
                else
                {
                    field += c;
                }
            }
            // Iface Destination Gateway Flags RefCnt Use Metric Mask ...
            if (fields.size() < 8 || fields[1] != "00000000" || fields[7] != "00000000")
            {
                continue;
            }
            auto const metric = parseInt(fields[6]);
            if (!metric)
            {
                continue;
            }
            if (!best || *metric < bestMetric)
            {
                best = fields[0];
                bestMetric = static_cast<long>(*metric);
            }
        }
        return best;
    }

    std::optional<std::string> pickHostAddress(std::vector<InterfaceAddress> const& addresses, std::optional<std::string> const& defaultInterface)
    {
        auto usable = [](InterfaceAddress const& a)
        {
            auto const ip = parseIpv4(a.address);
            return ip && isAnnounceable(*ip);
        };
        if (defaultInterface)
        {
            for (auto const& a : addresses)
            {
                if (a.ifname == *defaultInterface && usable(a))
                {
                    return a.address;
                }
            }
        }
        for (auto const& a : addresses)
        {
            if (usable(a))
            {
                return a.address;
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> defaultHostAddress()
    {
        auto const routes = readFile("/proc/net/route");
        return pickHostAddress(interfaceIpv4Addresses(), routes ? defaultRouteInterface(*routes) : std::nullopt);
    }
}
