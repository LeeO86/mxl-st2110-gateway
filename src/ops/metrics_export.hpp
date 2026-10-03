// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "group/pipeline_types.hpp"
#include "mtl/backend.hpp"
#include "mxlbridge/domainscan.hpp"
#include "ops/clock_supervisor.hpp"
#include "ops/metrics.hpp"

namespace mxlgw::ops
{
    struct ConfiguredDomainUsage
    {
        std::string name;
        std::uint64_t usedBytes = 0;
        std::uint64_t freeBytes = 0;
        std::size_t flows = 0;
    };

    using ActivationKey = std::tuple<std::string, std::string, std::string>; // kind, transport, result

    /// Everything /metrics exports (§12.1). Metric names and labels are a public interface.
    struct MetricsInput
    {
        bool ready = false;
        bool restartRequired = false;
        std::optional<media::BackendStatus> backend;
        std::optional<ClockSample> clock;
        std::vector<group::GroupSnapshot> groups;
        std::optional<mxlbridge::ScanResult> scan;
        std::vector<ConfiguredDomainUsage> domains;
        std::map<std::string, bool> nmosRegistered; // running NMOS node ("mxl", "st2110") -> registered
        std::map<ActivationKey, std::uint64_t> activations;
    };

    void exportMetrics(MetricsWriter& out, MetricsInput const& in);

    /// "p" for the primary port, "r" for the redundant one.
    char const* portLabel(std::size_t index);
}
