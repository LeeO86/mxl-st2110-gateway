// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"

namespace mxlgw::ops
{
    enum class CheckLevel
    {
        Ok,
        Info,
        Warn,
        Fail,
    };

    char const* toName(CheckLevel level);

    struct CheckResult
    {
        std::string id;
        CheckLevel level = CheckLevel::Ok;
        std::string message;
        std::string anchor; // README section ("#preflight-<id>")
    };

    /// Filesystem roots (overridable by tests).
    struct PreflightEnv
    {
        std::string sysRoot = "/sys";
        std::string procRoot = "/proc";
        std::string devRoot = "/dev";
        std::string firmwareRoot = "/lib/firmware";
        bool checkPortInUse = true;
    };

    /// Preflight checks of §14.3. Hard failures abort startup with exit 78.
    std::vector<CheckResult> runPreflight(config::Config const& config, PreflightEnv const& env = {});
    bool hasFailures(std::vector<CheckResult> const& results);
    nlohmann::json toJson(std::vector<CheckResult> const& results);

    /// Kernel TAI offset from adjtimex() (seconds).
    int kernelTaiOffset();

    /// Effective capability bits of this process (/proc/self/status CapEff).
    unsigned long long effectiveCapabilities(std::string const& procRoot = "/proc");
}
