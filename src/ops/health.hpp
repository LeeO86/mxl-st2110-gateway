// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mxlgw::ops
{
    /// Inputs of /readyz (§10). Discovered/mirror domains and waiting/no-signal essences do not count.
    struct ReadinessInputs
    {
        bool configValid = true;
        bool setupMode = false;
        bool mediaUp = false;
        bool testBackend = false;
        bool requireLock = true;
        std::optional<bool> ptpLocked;             // nullopt = external clock (no MTL PTP)
        std::optional<std::int64_t> clockOffsetNs; // MTL − host CLOCK_TAI
        std::int64_t maxOffsetNs = 1'000'000;
        bool domainsOk = true;
        /// MXL node: registration is only required when a registry is configured (DNS-SD or static, G7).
        bool registryConfigured = false;
        bool nmosRegistered = false;
        /// ST 2110 node (node.st2110.registry).
        bool st2110RegistryConfigured = false;
        bool st2110Registered = false;
        bool shuttingDown = false;
        std::vector<std::string> extraReasons;
    };

    struct Readiness
    {
        bool ready = false;
        std::vector<std::string> reasons;
        std::vector<std::string> warnings;
        nlohmann::json toJson() const;
    };

    Readiness evaluateReadiness(ReadinessInputs const& in);
}
