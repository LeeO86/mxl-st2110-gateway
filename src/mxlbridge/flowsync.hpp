// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

#include <mxl/flow.h>

#include "mxlbridge/instance.hpp"
#include "mxlbridge/reader.hpp"

namespace mxlgw::mxlbridge
{
    /// RAII mxlFlowSynchronizationGroup on a private mxlInstance of a domain. Not thread-safe: owned and used
    /// by one egress group worker (VERIFIED: dmf-mxl/mxl@v1.1.0 lib/internal/src/FlowSynchronizationGroup.cpp:77-131
    /// — waitForDataAt reorders the reader list).
    ///
    /// VERIFIED: dmf-mxl/mxl@218ddaa lib/internal/src/Instance.cpp:145-165, 439-455 — releasing a reader's last
    /// reference calls removeReader on every sync group of the reader's instance, while creating and releasing a
    /// group change that list without the instance mutex, and a group has no lock. On the domain's shared instance
    /// a reader released by another worker (the audio worker on a re-created flow or at stop) walked a group being
    /// released or changed: SIGSEGV in FlowSynchronizationGroup::removeReader. The private instance has no readers.
    ///
    /// VERIFIED: dmf-mxl/mxl@v1.1.0 lib/src/flow.cpp:702-730 — AddReader does not check which instance
    /// created the reader, so readers of different domains (configured, discovered, mirror) can share
    /// one group; readers MUST be removed before they are released.
    class SyncGroup
    {
    public:
        explicit SyncGroup(std::string const& domainPath);
        ~SyncGroup();
        SyncGroup(SyncGroup const&) = delete;
        SyncGroup& operator=(SyncGroup const&) = delete;

        mxlStatus add(FlowReader const& reader);
        mxlStatus remove(FlowReader const& reader);
        /// Waits until every reader has data for TAI `timestampNs` (complete grain for discrete flows,
        /// the sample at that time for continuous flows), or until `timeoutNs` expires
        /// (MXL_ERR_OUT_OF_RANGE_TOO_EARLY).
        mxlStatus waitForDataAt(std::uint64_t timestampNs, std::uint64_t timeoutNs);
        std::size_t size() const { return _count; }

    private:
        Instance _instance;
        mxlFlowSynchronizationGroup _group = nullptr;
        std::size_t _count = 0;
    };
}
