// SPDX-License-Identifier: MIT
#include "mxlbridge/flowsync.hpp"

namespace mxlgw::mxlbridge
{
    SyncGroup::SyncGroup(std::shared_ptr<Instance> owner)
        : _owner(std::move(owner))
    {
        auto const status = ::mxlCreateFlowSynchronizationGroup(_owner->get(), &_group);
        if (status != MXL_STATUS_OK || _group == nullptr)
        {
            _group = nullptr;
            throw MxlError("mxlCreateFlowSynchronizationGroup failed", status);
        }
    }

    SyncGroup::~SyncGroup()
    {
        if (_group != nullptr)
        {
            ::mxlReleaseFlowSynchronizationGroup(_owner->get(), _group);
        }
    }

    mxlStatus SyncGroup::add(FlowReader const& reader)
    {
        auto const status = ::mxlFlowSynchronizationGroupAddReader(_group, reader.handle());
        if (status == MXL_STATUS_OK)
        {
            ++_count;
        }
        return status;
    }

    mxlStatus SyncGroup::remove(FlowReader const& reader)
    {
        auto const status = ::mxlFlowSynchronizationGroupRemoveReader(_group, reader.handle());
        if (status == MXL_STATUS_OK && _count > 0)
        {
            --_count;
        }
        return status;
    }

    mxlStatus SyncGroup::waitForDataAt(std::uint64_t timestampNs, std::uint64_t timeoutNs)
    {
        if (_count == 0)
        {
            return MXL_STATUS_OK;
        }
        return ::mxlFlowSynchronizationGroupWaitForDataAt(_group, timestampNs, timeoutNs);
    }
}
