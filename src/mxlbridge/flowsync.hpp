// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>

#include <mxl/flow.h>

#include "mxlbridge/instance.hpp"
#include "mxlbridge/reader.hpp"

namespace mxlgw::mxlbridge
{
    /// RAII mxlFlowSynchronizationGroup. Not thread-safe: owned and used by one egress group worker
    /// (VERIFIED: dmf-mxl/mxl@v1.1.0 lib/internal/src/FlowSynchronizationGroup.cpp:77-131 — waitForDataAt
    /// reorders the reader list).
    ///
    /// VERIFIED: dmf-mxl/mxl@v1.1.0 lib/src/flow.cpp:702-730 — AddReader does not check which instance
    /// created the reader, so readers of different domains (configured, discovered, mirror) can share
    /// one group; readers MUST be removed before they are released.
    class SyncGroup
    {
    public:
        explicit SyncGroup(std::shared_ptr<Instance> owner);
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
        std::shared_ptr<Instance> _owner;
        mxlFlowSynchronizationGroup _group = nullptr;
        std::size_t _count = 0;
    };
}
