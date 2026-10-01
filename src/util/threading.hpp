// SPDX-License-Identifier: MIT
#pragma once

#include <set>
#include <string>

namespace mxlgw::util
{
    /// Names the calling thread (truncated to 15 characters).
    void setThreadName(std::string const& name);

    /// Tries SCHED_FIFO for the calling thread; returns false (no exception) if not permitted (§3.6).
    bool tryRealtime(int priority);

    /// Pins the calling thread to `cpus` (no-op for an empty set). Returns false on failure.
    bool pinToCpus(std::set<int> const& cpus);
}
