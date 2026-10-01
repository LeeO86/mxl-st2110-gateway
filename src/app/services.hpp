// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "config/store.hpp"
#include "ops/health.hpp"
#include "ops/preflight.hpp"

namespace mxlgw::app
{
    /// What the web API (§11.3) needs from the running gateway. Implemented by app::Application,
    /// faked in unit tests.
    class Services
    {
    public:
        virtual ~Services() = default;

        virtual config::ConfigStore& store() = 0;
        virtual bool setupMode() const = 0;
        virtual bool restartRequired() const = 0;
        virtual std::vector<std::string> restartReasons() const = 0;
        virtual void markRestartRequired(std::string const& reason) = 0;

        /// Applies the groups of the current effective configuration live (§9.3) on the control thread
        /// and returns per-group results.
        virtual nlohmann::json applyGroups() = 0;

        virtual nlohmann::json status() = 0;
        virtual nlohmann::json nic() = 0;
        virtual nlohmann::json ptp() = 0;
        virtual nlohmann::json domains() = 0;
        /// Flows of a domain identified by its domain_def.json id; nullopt if the domain is unknown.
        virtual std::optional<nlohmann::json> flows(std::string const& domainId) = 0;
        virtual nlohmann::json nmos() = 0;
        virtual std::vector<ops::CheckResult> preflight() = 0;
        virtual ops::Readiness readiness() = 0;
        virtual std::string metrics() = 0;
        virtual std::string_view adminHtml() const = 0;
        /// Graceful exit with code 0 so the orchestrator restarts the container (§9.2).
        virtual void requestRestart() = 0;
    };
}
