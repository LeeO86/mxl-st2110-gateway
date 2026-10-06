// SPDX-License-Identifier: MIT
// Boundary between the gateway (C++20) and the nmos-cpp layer (C++17). Must stay C++17-clean and must
// not include nmos-cpp headers.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "ops/http.hpp"
#include "util/uuid.hpp"

namespace mxlgw::app
{
    class ConnectionState;
}

namespace mxlgw::nmosnode
{
    /// One entry of the IS-04 Node `interfaces` (§4.5).
    struct InterfaceInfo
    {
        std::string name;       // configured port name (media-p) or kernel ifname (management)
        std::string mac;        // port_id
        std::string chassisMac; // chassis_id ("" = null)
        std::vector<std::string> addresses;
    };

    /// A configured domain after bootstrap (id from domain_def.json).
    struct DomainInfo
    {
        std::string name;
        std::string path;
        util::Uuid id;
    };

    /// Node clock for SDP ts-refclk (§7.2).
    struct ClockInfo
    {
        bool ptp = false; // false: internal clock (test backends)
        std::string gmid; // "08-00-11-ff-fe-21-e1-b0"
        bool locked = false;
        int domain = 127;
        bool traceable = false;
        std::string localMac; // ts-refclk:localmac for internal clocks
    };

    /// An IS-05 activation of one of our Senders/Receivers (§7.4, §7.5).
    struct Activation
    {
        util::Uuid essenceUid;
        util::Uuid resourceId;
        bool sender = false;   // IS-04 type
        std::string transport; // "rtp" | "mxl"
        nlohmann::json active; // IS-05 /active endpoint (master_enable, transport_params, ...)
        std::string note;      // why a restored activation came up disabled (essence state reason), else empty
    };

    /// Result of the MXL flow format check at staging (§6.4): empty = acceptable.
    using MxlFlowCheck = std::function<std::string(util::Uuid const& essenceUid, std::optional<util::Uuid> const& domainId, util::Uuid const& flowId)>;

    struct Callbacks
    {
        /// Called on nmos-cpp's activation thread; must not block (post to the control thread).
        std::function<void(Activation const&)> activated;
        /// BCP-007-03 `auto` for an MXL Receiver: group domain or the domain holding the flow (rescans).
        std::function<std::optional<util::Uuid>(util::Uuid const& groupDomainId, std::optional<util::Uuid> const& flowId)> resolveMxlDomain;
        MxlFlowCheck checkMxlFlow;
        /// Unknown domain at staging (C3): log `mxl_domain_unknown`, still accept.
        std::function<bool(util::Uuid const& domainId)> domainAccessible;
    };

    /// The gateway runs two NMOS nodes (§7.1): the MXL node (MXL Senders of ingest groups, MXL Receivers of egress
    /// groups) registers with the MXL registry; the ST 2110 node (RTP Receivers of ingest groups, RTP Senders of
    /// egress groups) has its own port and its own, optional registry.
    enum class Side
    {
        Mxl,
        St2110,
    };

    char const* toName(Side side);

    struct Setup
    {
        Side side = Side::Mxl;
        config::Config config;
        std::vector<InterfaceInfo> interfaces; // media ports first (primary, redundant), then management
        std::vector<DomainInfo> domains;
        ClockInfo clock;
        app::ConnectionState* connections = nullptr; // §7.6; nullptr = no persistence
        ops::Router const* routes = nullptr;         // gateway routes on the node's listener (§7.1); nullptr = none
        std::string gatewayVersion;
        std::string hostAddress; // IPv4 literal announced in hrefs and api.endpoints (§7.1, G5)
    };

    class Node
    {
    public:
        virtual ~Node() = default;
        /// Opens the node's listener and registers. Throws ListenError when the port cannot be bound.
        virtual void start() = 0;
        virtual void stop() = 0;
        /// Removes every resource so the registry receives DELETEs (§14.4), then waits up to `timeout` for the
        /// node's own DELETE. Returns true when the node is no longer registered.
        virtual bool deregister(std::chrono::milliseconds timeout) = 0;
        /// Live group add/edit/remove (§9.3): re-registers the affected resources.
        virtual nlohmann::json applyGroups(config::Config const& config) = 0;
        virtual void updateClock(ClockInfo const& clock) = 0;
        virtual nlohmann::json status() const = 0;
        virtual bool registered() const = 0;
        virtual Side side() const = 0;
    };

    /// A listener could not be opened (port in use, no permission): exit 75 (§14.4).
    class ListenError : public std::runtime_error
    {
    public:
        ListenError(int port, std::string const& what)
            : std::runtime_error("cannot listen on port " + std::to_string(port) + ": " + what)
            , _port(port)
        {}
        int port() const { return _port; }

    private:
        int _port;
    };

    std::unique_ptr<Node> createNode(Setup setup, Callbacks callbacks);

    /// Setup mode, or node.web_port set: a bare server with only the gateway routes on `port` (§7.1).
    class HttpServer
    {
    public:
        virtual ~HttpServer() = default;
        /// Throws ListenError when the port cannot be bound.
        virtual void start() = 0;
        virtual void stop() = 0;
    };
    std::unique_ptr<HttpServer> createHttpServer(int port, config::Tls const& tls, ops::Router const& routes);
}
