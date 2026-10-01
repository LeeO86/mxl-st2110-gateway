// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <mxl/flow.h>
#include <nlohmann/json.hpp>

#include "mxlbridge/instance.hpp"

namespace mxlgw::mxlbridge
{
    /// Common part of an MXL FlowWriter: creation, definition check and release (§8.4).
    class FlowWriter
    {
    public:
        /// Creates or opens the flow. Throws MxlError on failure.
        FlowWriter(std::shared_ptr<Instance> instance, nlohmann::json const& flowDef, std::string const& options);
        virtual ~FlowWriter();
        FlowWriter(FlowWriter const&) = delete;
        FlowWriter& operator=(FlowWriter const&) = delete;

        /// false if MXL opened an existing flow (MXL does not compare definitions, flow.h).
        bool created() const { return _created; }
        /// Definition of the existing flow when created() is false (§8.4 comparison input).
        nlohmann::json const& existingFlowDef() const { return _existingDef; }
        mxlFlowConfigInfo const& configInfo() const { return _config; }
        std::string const& flowId() const { return _flowId; }
        std::string const& domainPath() const { return _instance->path(); }
        mxlFlowWriter handle() const { return _writer; }

    protected:
        std::shared_ptr<Instance> _instance;
        mxlFlowWriter _writer = nullptr;
        mxlFlowConfigInfo _config{};
        std::string _flowId;
        bool _created = false;
        nlohmann::json _existingDef;
    };

    /// Discrete flows (video/v210, video/smpte291). MXL keeps one open grain per writer
    /// (PosixDiscreteFlowWriter::_currentIndex); open/commit must be serialised by the caller.
    class GrainWriter : public FlowWriter
    {
    public:
        using FlowWriter::FlowWriter;

        /// Lock- and allocation-free (MXL flow.cpp / PosixDiscreteFlowWriter::openGrain); MAY run on an MTL lcore.
        mxlStatus open(std::uint64_t index, mxlGrainInfo& info, std::uint8_t*& payload) noexcept
        {
            return ::mxlFlowWriterOpenGrain(_writer, index, &info, &payload);
        }
        mxlStatus commit(mxlGrainInfo const& info) noexcept { return ::mxlFlowWriterCommitGrain(_writer, &info); }
        mxlStatus cancel() noexcept { return ::mxlFlowWriterCancelGrain(_writer); }

        /// Open + copy + complete commit (validSlices = totalSlices) in one call.
        mxlStatus write(std::uint64_t index, std::span<std::uint8_t const> data, std::uint32_t flags = 0) noexcept;

        std::uint32_t grainCount() const { return _config.discrete.grainCount; }
        std::uint32_t sliceSize() const { return _config.discrete.sliceSizes[0]; }
        mxlRational grainRate() const { return _config.common.grainRate; }
    };

    /// Continuous flows (audio/float32): per-channel ring buffers addressed by the END sample index.
    class SampleWriter : public FlowWriter
    {
    public:
        SampleWriter(std::shared_ptr<Instance> instance, nlohmann::json const& flowDef, std::string const& options);

        /// Opens the `count` samples ending at `endIndex` (flow.h mxlFlowWriterOpenSamples).
        mxlStatus open(std::uint64_t endIndex, std::size_t count, mxlMutableWrappedMultiBufferSlice& slices) noexcept
        {
            return ::mxlFlowWriterOpenSamples(_writer, endIndex, count, &slices);
        }
        mxlStatus commit() noexcept { return ::mxlFlowWriterCommitSamples(_writer); }
        mxlStatus cancel() noexcept { return ::mxlFlowWriterCancelSamples(_writer); }

        std::size_t maxWriteLength() const { return _maxWrite; }
        std::uint32_t channelCount() const { return _config.continuous.channelCount; }
        std::uint32_t bufferLength() const { return _config.continuous.bufferLength; }

    private:
        std::size_t _maxWrite = 0;
    };
}
