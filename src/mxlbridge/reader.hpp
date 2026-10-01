// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <mxl/flow.h>
#include <nlohmann/json.hpp>

#include "mxlbridge/instance.hpp"

namespace mxlgw::mxlbridge
{
    /// MXL FlowReader. Creation failures (notably MXL_ERR_FLOW_NOT_FOUND) are normal operating
    /// states for egress (§5.8), so open() reports a status instead of throwing.
    class FlowReader
    {
    public:
        virtual ~FlowReader();
        FlowReader(FlowReader const&) = delete;
        FlowReader& operator=(FlowReader const&) = delete;

        mxlFlowReader handle() const { return _reader; }
        mxlFlowConfigInfo const& configInfo() const { return _config; }
        std::string const& flowId() const { return _flowId; }
        std::shared_ptr<Instance> const& instance() const { return _instance; }
        bool continuous() const;
        /// Current head index and last write time (TAI ns); nullopt if the reader is unusable.
        std::optional<mxlFlowRuntimeInfo> runtime() const;
        /// flow_def.json of the opened flow.
        nlohmann::json const& flowDef() const { return _flowDef; }

    protected:
        FlowReader(std::shared_ptr<Instance> instance, std::string flowId);
        mxlStatus init();

        std::shared_ptr<Instance> _instance;
        std::string _flowId;
        mxlFlowReader _reader = nullptr;
        mxlFlowConfigInfo _config{};
        nlohmann::json _flowDef;
    };

    class GrainReader : public FlowReader
    {
    public:
        static std::unique_ptr<GrainReader> open(std::shared_ptr<Instance> instance, std::string const& flowId, mxlStatus& status);

        mxlStatus get(std::uint64_t index, std::uint64_t timeoutNs, mxlGrainInfo& info, std::uint8_t const*& payload) const noexcept;
        mxlStatus getNonBlocking(std::uint64_t index, mxlGrainInfo& info, std::uint8_t const*& payload) const noexcept;
        mxlRational grainRate() const { return _config.common.grainRate; }

    private:
        using FlowReader::FlowReader;
    };

    class SampleReader : public FlowReader
    {
    public:
        static std::unique_ptr<SampleReader> open(std::shared_ptr<Instance> instance, std::string const& flowId, mxlStatus& status);

        /// Reads the `count` samples ending at `endIndex`.
        mxlStatus get(std::uint64_t endIndex, std::size_t count, std::uint64_t timeoutNs, mxlWrappedMultiBufferSlice& slices) const noexcept;
        mxlStatus getNonBlocking(std::uint64_t endIndex, std::size_t count, mxlWrappedMultiBufferSlice& slices) const noexcept;
        std::size_t maxReadLength() const { return _maxRead; }
        std::uint32_t channelCount() const { return _config.continuous.channelCount; }

    private:
        using FlowReader::FlowReader;
        std::size_t _maxRead = 0;
    };

    /// Opens a GrainReader or SampleReader depending on the flow's format.
    std::unique_ptr<FlowReader> openReader(std::shared_ptr<Instance> instance, std::string const& flowId, bool continuous, mxlStatus& status);
}
