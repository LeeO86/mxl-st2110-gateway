// SPDX-License-Identifier: MIT
#include "mxlbridge/writer.hpp"

#include <algorithm>
#include <cstring>

#include "util/logging.hpp"

namespace mxlgw::mxlbridge
{
    FlowWriter::FlowWriter(std::shared_ptr<Instance> instance, nlohmann::json const& flowDef, std::string const& options)
        : _instance(std::move(instance))
        , _flowId(flowDef.value("id", std::string()))
    {
        auto const def = flowDef.dump();
        auto const status = ::mxlCreateFlowWriter(_instance->get(), def.c_str(), options.empty() ? nullptr : options.c_str(), &_writer, &_config, &_created);
        if (status != MXL_STATUS_OK || _writer == nullptr)
        {
            _writer = nullptr;
            throw MxlError("mxlCreateFlowWriter failed for flow " + _flowId + " in " + _instance->path(), status);
        }
        if (!_created)
        {
            // VERIFIED: dmf-mxl/mxl@v1.1.0 lib/include/mxl/flow.h:155-159 — an existing flow is opened without
            // comparing definitions, so the caller compares existingFlowDef() with its descriptor (§8.4).
            auto const text = _instance->flowDef(_flowId);
            try
            {
                _existingDef = text.empty() ? nlohmann::json() : nlohmann::json::parse(text);
            }
            catch (std::exception const&)
            {
                _existingDef = nlohmann::json();
            }
        }
        log::debug("mxl_flow_writer_created", {{"flow_id", _flowId}, {"domain", _instance->path()}, {"created", _created}});
    }

    FlowWriter::~FlowWriter()
    {
        if (_writer != nullptr)
        {
            ::mxlReleaseFlowWriter(_instance->get(), _writer);
            log::debug("mxl_flow_writer_released", {{"flow_id", _flowId}, {"domain", _instance->path()}});
        }
    }

    mxlStatus GrainWriter::write(std::uint64_t index, std::span<std::uint8_t const> data, std::uint32_t flags) noexcept
    {
        mxlGrainInfo info{};
        std::uint8_t* payload = nullptr;
        auto status = open(index, info, payload);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }
        auto const n = std::min<std::size_t>(data.size(), info.grainSize);
        if (n > 0)
        {
            std::memcpy(payload, data.data(), n);
        }
        info.flags = flags;
        info.validSlices = info.totalSlices;
        return commit(info);
    }

    SampleWriter::SampleWriter(std::shared_ptr<Instance> instance, nlohmann::json const& flowDef, std::string const& options)
        : FlowWriter(std::move(instance), flowDef, options)
    {
        std::size_t maxWrite = 0;
        if (::mxlFlowWriterGetMaxWriteLengthSamples(_writer, &maxWrite) == MXL_STATUS_OK && maxWrite > 0)
        {
            _maxWrite = maxWrite;
        }
        else
        {
            _maxWrite = _config.continuous.bufferLength / 2;
        }
    }
}
