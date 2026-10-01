// SPDX-License-Identifier: MIT
#include "app/targets.hpp"

namespace mxlgw::app
{
    namespace
    {
        using json = nlohmann::json;

        std::string text(json const& j, char const* key)
        {
            return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
        }

        int integer(json const& j, char const* key, int fallback)
        {
            return j.contains(key) && j[key].is_number_integer() ? j[key].get<int>() : fallback;
        }

        bool boolean(json const& j, char const* key, bool fallback)
        {
            return j.contains(key) && j[key].is_boolean() ? j[key].get<bool>() : fallback;
        }

        json const& legsOf(json const& active)
        {
            static json const empty = json::array();
            return active.contains("transport_params") && active["transport_params"].is_array() ? active["transport_params"] : empty;
        }

        std::optional<util::Uuid> uuidField(json const& j, char const* key)
        {
            if (!j.contains(key) || !j[key].is_string())
            {
                return std::nullopt;
            }
            return util::parseUuid(j[key].get<std::string>());
        }
    }

    group::RtpTarget rtpReceiverTarget(nlohmann::json const& active)
    {
        group::RtpTarget t;
        t.masterEnable = boolean(active, "master_enable", false);
        for (auto const& leg : legsOf(active))
        {
            media::LegAddress a;
            a.destination = text(leg, "multicast_ip");
            if (a.destination.empty())
            {
                a.destination = text(leg, "interface_ip"); // unicast reception
            }
            a.source = text(leg, "source_ip");
            a.port = integer(leg, "destination_port", 0);
            a.enabled = boolean(leg, "rtp_enabled", true);
            t.legs.push_back(a);
        }
        return t;
    }

    group::RtpTarget rtpSenderTarget(nlohmann::json const& active)
    {
        group::RtpTarget t;
        t.masterEnable = boolean(active, "master_enable", false);
        for (auto const& leg : legsOf(active))
        {
            media::LegAddress a;
            a.destination = text(leg, "destination_ip");
            a.source = text(leg, "source_ip");
            a.port = integer(leg, "destination_port", 5004);
            a.enabled = boolean(leg, "rtp_enabled", true);
            t.legs.push_back(a);
        }
        return t;
    }

    group::MxlSenderTarget mxlSenderTarget(nlohmann::json const& active)
    {
        return {boolean(active, "master_enable", false)};
    }

    group::MxlReceiverTarget mxlReceiverTarget(nlohmann::json const& active)
    {
        group::MxlReceiverTarget t;
        t.masterEnable = boolean(active, "master_enable", false);
        auto const& legs = legsOf(active);
        if (!legs.empty())
        {
            t.domainId = uuidField(legs[0], "mxl_domain_id");
            t.flowId = uuidField(legs[0], "mxl_flow_id");
        }
        return t;
    }

    nlohmann::json defaultRtpReceiverParams(config::EssenceCommon const& essence, bool redundant)
    {
        json legs = json::array();
        for (std::size_t i = 0; i < (redundant ? 2u : 1u); ++i)
        {
            json leg;
            leg["interface_ip"] = "auto";
            if (i < essence.legs.size())
            {
                auto const& l = essence.legs[i];
                leg["multicast_ip"] = l.multicast;
                leg["source_ip"] = l.source.empty() ? json() : json(l.source);
                leg["destination_port"] = l.port;
                leg["rtp_enabled"] = true;
            }
            else
            {
                leg["multicast_ip"] = nullptr;
                leg["source_ip"] = nullptr;
                leg["destination_port"] = "auto";
                leg["rtp_enabled"] = false;
            }
            legs.push_back(leg);
        }
        return legs;
    }

    nlohmann::json defaultRtpSenderParams(config::EssenceCommon const& essence, bool redundant, config::PortPair const& ports)
    {
        json legs = json::array();
        for (std::size_t i = 0; i < (redundant ? 2u : 1u); ++i)
        {
            json leg;
            auto const& port = i == 0 ? ports.primary : (ports.redundant ? *ports.redundant : ports.primary);
            leg["source_ip"] = port.ip.empty() ? json("auto") : json(port.ip);
            leg["source_port"] = "auto";
            if (i < essence.legs.size())
            {
                leg["destination_ip"] = essence.legs[i].multicast;
                leg["destination_port"] = essence.legs[i].port;
                leg["rtp_enabled"] = true;
            }
            else
            {
                leg["destination_ip"] = "auto";
                leg["destination_port"] = "auto";
                leg["rtp_enabled"] = i == 0;
            }
            legs.push_back(leg);
        }
        return legs;
    }
}
