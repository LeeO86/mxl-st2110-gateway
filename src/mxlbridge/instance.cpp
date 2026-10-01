// SPDX-License-Identifier: MIT
#include "mxlbridge/instance.hpp"

#include <mxl/flow.h>

#include "util/logging.hpp"

namespace mxlgw::mxlbridge
{
    char const* statusName(mxlStatus status)
    {
        switch (status)
        {
            case MXL_STATUS_OK: return "MXL_STATUS_OK";
            case MXL_ERR_UNKNOWN: return "MXL_ERR_UNKNOWN";
            case MXL_ERR_FLOW_NOT_FOUND: return "MXL_ERR_FLOW_NOT_FOUND";
            case MXL_ERR_OUT_OF_RANGE_TOO_LATE: return "MXL_ERR_OUT_OF_RANGE_TOO_LATE";
            case MXL_ERR_OUT_OF_RANGE_TOO_EARLY: return "MXL_ERR_OUT_OF_RANGE_TOO_EARLY";
            case MXL_ERR_INVALID_FLOW_READER: return "MXL_ERR_INVALID_FLOW_READER";
            case MXL_ERR_INVALID_FLOW_WRITER: return "MXL_ERR_INVALID_FLOW_WRITER";
            case MXL_ERR_TIMEOUT: return "MXL_ERR_TIMEOUT";
            case MXL_ERR_INVALID_ARG: return "MXL_ERR_INVALID_ARG";
            case MXL_ERR_CONFLICT: return "MXL_ERR_CONFLICT";
            case MXL_ERR_PERMISSION_DENIED: return "MXL_ERR_PERMISSION_DENIED";
            case MXL_ERR_FLOW_INVALID: return "MXL_ERR_FLOW_INVALID";
            default: return "MXL_ERR_OTHER";
        }
    }

    Instance::Instance(std::string path)
        : _path(std::move(path))
    {
        _instance = ::mxlCreateInstance(_path.c_str(), nullptr);
        if (_instance == nullptr)
        {
            throw MxlError("mxlCreateInstance failed for " + _path, MXL_ERR_UNKNOWN);
        }
        log::debug("mxl_instance_created", {{"path", _path}});
    }

    Instance::~Instance()
    {
        if (_instance != nullptr)
        {
            ::mxlDestroyInstance(_instance);
            log::debug("mxl_instance_destroyed", {{"path", _path}});
        }
    }

    void Instance::garbageCollectAll()
    {
        auto const status = ::mxlGarbageCollectFlows(_instance);
        log::info("mxl_domain_gc", {{"path", _path}, {"status", statusName(status)}});
    }

    std::string Instance::flowDef(std::string const& flowId) const
    {
        std::size_t size = 0;
        (void)::mxlGetFlowDef(_instance, flowId.c_str(), nullptr, &size);
        if (size == 0)
        {
            return {};
        }
        std::string buffer(size, '\0');
        auto const status = ::mxlGetFlowDef(_instance, flowId.c_str(), buffer.data(), &size);
        if (status != MXL_STATUS_OK)
        {
            return {};
        }
        while (!buffer.empty() && buffer.back() == '\0')
        {
            buffer.pop_back();
        }
        return buffer;
    }

    std::shared_ptr<Instance> InstanceRegistry::pin(std::string const& path)
    {
        auto lock = std::lock_guard{_mutex};
        if (auto const it = _pinned.find(path); it != _pinned.end())
        {
            return it->second;
        }
        auto instance = std::make_shared<Instance>(path);
        _pinned.emplace(path, instance);
        _cache[path] = instance;
        return instance;
    }

    std::shared_ptr<Instance> InstanceRegistry::acquire(std::string const& path)
    {
        auto lock = std::lock_guard{_mutex};
        if (auto const it = _pinned.find(path); it != _pinned.end())
        {
            return it->second;
        }
        if (auto const it = _cache.find(path); it != _cache.end())
        {
            if (auto existing = it->second.lock())
            {
                return existing;
            }
        }
        auto instance = std::make_shared<Instance>(path);
        _cache[path] = instance;
        return instance;
    }

    std::vector<std::string> InstanceRegistry::openPaths() const
    {
        auto lock = std::lock_guard{_mutex};
        std::vector<std::string> paths;
        for (auto const& [path, weak] : _cache)
        {
            if (!weak.expired())
            {
                paths.push_back(path);
            }
        }
        return paths;
    }

    void InstanceRegistry::clear()
    {
        auto lock = std::lock_guard{_mutex};
        _pinned.clear();
        _cache.clear();
    }

    std::string mxlVersionString()
    {
        mxlVersionType v{};
        if (::mxlGetVersion(&v) != MXL_STATUS_OK)
        {
            return "unknown";
        }
        if (v.full != nullptr)
        {
            return v.full;
        }
        return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.bugfix);
    }
}
