// SPDX-License-Identifier: MIT
#include "mxlbridge/reader.hpp"

#include <mxl/dataformat.h>

#include "util/logging.hpp"

namespace mxlgw::mxlbridge
{
    FlowReader::FlowReader(std::shared_ptr<Instance> instance, std::string flowId)
        : _instance(std::move(instance))
        , _flowId(std::move(flowId))
    {}

    FlowReader::~FlowReader()
    {
        if (_reader != nullptr)
        {
            ::mxlReleaseFlowReader(_instance->get(), _reader);
        }
    }

    mxlStatus FlowReader::init()
    {
        if (auto* registry = _instance->registry(); registry != nullptr)
        {
            // Not the domain's shared instance as such: MXL hands back a reader cached per flow id, which
            // maps the deleted files when the writer re-created the flow (InstanceRegistry::claimFlow).
            auto const file = flowFileId(_instance->path(), _flowId);
            if (!file)
            {
                return MXL_ERR_FLOW_NOT_FOUND;
            }
            try
            {
                _claim = registry->claimFlow(_instance->path(), _flowId, *file);
            }
            catch (std::exception const& ex)
            {
                // mxlCreateInstance failed (e.g. fs.inotify.max_user_instances reached): never let it reach
                // a worker or HTTP thread (std::terminate); the caller retries.
                static log::RateLimiter limiter{std::chrono::seconds(30)};
                if (limiter.allow(_instance->path() + "/" + _flowId))
                {
                    log::warn("mxl_instance_failed", {{"path", _instance->path()}, {"mxl_flow_id", _flowId}, {"error", ex.what()}});
                }
                _instanceUnavailable = true;
                return MXL_ERR_UNKNOWN;
            }
            _instance = _claim->instance();
        }
        auto status = ::mxlCreateFlowReader(_instance->get(), _flowId.c_str(), nullptr, &_reader);
        if (status != MXL_STATUS_OK)
        {
            _reader = nullptr;
            return status;
        }
        status = ::mxlFlowReaderGetConfigInfo(_reader, &_config);
        if (status != MXL_STATUS_OK)
        {
            return status;
        }
        auto const text = _instance->flowDef(_flowId);
        try
        {
            _flowDef = text.empty() ? nlohmann::json() : nlohmann::json::parse(text);
        }
        catch (std::exception const&)
        {
            _flowDef = nlohmann::json();
        }
        return MXL_STATUS_OK;
    }

    bool FlowReader::continuous() const
    {
        return _config.common.format == MXL_DATA_FORMAT_AUDIO;
    }

    std::optional<mxlFlowRuntimeInfo> FlowReader::runtime() const
    {
        mxlFlowRuntimeInfo info{};
        if (_reader == nullptr || ::mxlFlowReaderGetRuntimeInfo(_reader, &info) != MXL_STATUS_OK)
        {
            return std::nullopt;
        }
        return info;
    }

    std::unique_ptr<GrainReader> GrainReader::open(std::shared_ptr<Instance> instance, std::string const& flowId, mxlStatus& status, bool* instanceUnavailable)
    {
        std::unique_ptr<GrainReader> reader(new GrainReader(std::move(instance), flowId));
        status = reader->init();
        if (instanceUnavailable != nullptr)
        {
            *instanceUnavailable = reader->_instanceUnavailable;
        }
        if (status != MXL_STATUS_OK)
        {
            return nullptr;
        }
        if (reader->continuous())
        {
            status = MXL_ERR_INVALID_FLOW_READER;
            return nullptr;
        }
        return reader;
    }

    mxlStatus GrainReader::get(std::uint64_t index, std::uint64_t timeoutNs, mxlGrainInfo& info, std::uint8_t const*& payload) const noexcept
    {
        std::uint8_t* p = nullptr;
        auto const status = ::mxlFlowReaderGetGrain(_reader, index, timeoutNs, &info, &p);
        payload = p;
        return status;
    }

    mxlStatus GrainReader::getNonBlocking(std::uint64_t index, mxlGrainInfo& info, std::uint8_t const*& payload) const noexcept
    {
        std::uint8_t* p = nullptr;
        auto const status = ::mxlFlowReaderGetGrainNonBlocking(_reader, index, &info, &p);
        payload = p;
        return status;
    }

    std::unique_ptr<SampleReader> SampleReader::open(std::shared_ptr<Instance> instance, std::string const& flowId, mxlStatus& status,
                                                     bool* instanceUnavailable)
    {
        std::unique_ptr<SampleReader> reader(new SampleReader(std::move(instance), flowId));
        status = reader->init();
        if (instanceUnavailable != nullptr)
        {
            *instanceUnavailable = reader->_instanceUnavailable;
        }
        if (status != MXL_STATUS_OK)
        {
            return nullptr;
        }
        if (!reader->continuous())
        {
            status = MXL_ERR_INVALID_FLOW_READER;
            return nullptr;
        }
        std::size_t maxRead = 0;
        if (::mxlFlowReaderGetMaxReadLengthSamples(reader->_reader, &maxRead) == MXL_STATUS_OK && maxRead > 0)
        {
            reader->_maxRead = maxRead;
        }
        else
        {
            reader->_maxRead = reader->_config.continuous.bufferLength / 2;
        }
        return reader;
    }

    mxlStatus SampleReader::get(std::uint64_t endIndex, std::size_t count, std::uint64_t timeoutNs, mxlWrappedMultiBufferSlice& slices) const noexcept
    {
        return ::mxlFlowReaderGetSamples(_reader, endIndex, count, timeoutNs, &slices);
    }

    mxlStatus SampleReader::getNonBlocking(std::uint64_t endIndex, std::size_t count, mxlWrappedMultiBufferSlice& slices) const noexcept
    {
        return ::mxlFlowReaderGetSamplesNonBlocking(_reader, endIndex, count, &slices);
    }

    std::unique_ptr<FlowReader> openReader(std::shared_ptr<Instance> instance, std::string const& flowId, bool continuous, mxlStatus& status,
                                           bool* instanceUnavailable)
    {
        if (continuous)
        {
            return SampleReader::open(std::move(instance), flowId, status, instanceUnavailable);
        }
        return GrainReader::open(std::move(instance), flowId, status, instanceUnavailable);
    }
}
