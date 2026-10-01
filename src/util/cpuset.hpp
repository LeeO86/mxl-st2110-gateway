// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace mxlgw::util
{
    /// Parses a Linux CPU list ("4-9,12,14-15"). nullopt on syntax errors or empty input.
    std::optional<std::set<int>> parseCpuList(std::string_view text);

    std::string formatCpuList(std::set<int> const& cpus);

    bool disjoint(std::set<int> const& a, std::set<int> const& b);

    /// CPUs present on this machine (/sys/devices/system/cpu/present); empty if unknown.
    std::set<int> presentCpus();
}
