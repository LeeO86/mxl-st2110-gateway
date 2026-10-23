// SPDX-License-Identifier: MIT
#include "mxlbridge/instance.hpp"

#include <algorithm>

#include <sys/stat.h>

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

    std::optional<FlowFileId> flowFileId(std::string const& domainPath, std::string const& flowId)
    {
        struct stat st
        {};
        auto const file = domainPath + "/" + flowId + ".mxl-flow/data";
        if (::stat(file.c_str(), &st) != 0)
        {
            return std::nullopt;
        }
        return FlowFileId{static_cast<std::uint64_t>(st.st_dev), static_cast<std::uint64_t>(st.st_ino)};
    }

    FlowClaim::FlowClaim(InstanceRegistry& registry, std::shared_ptr<Instance> instance, std::string flowId)
        : _registry(registry)
        , _instance(std::move(instance))
        , _flowId(std::move(flowId))
    {}

    FlowClaim::~FlowClaim()
    {
        _registry.release(_instance.get(), _flowId);
    }

    std::shared_ptr<Instance> InstanceRegistry::create(std::string const& path)
    {
        auto instance = std::make_shared<Instance>(path);
        instance->_registry = this;
        return instance;
    }

    std::shared_ptr<Instance> InstanceRegistry::pin(std::string const& path)
    {
        auto lock = std::lock_guard{_mutex};
        if (auto const it = _pinned.find(path); it != _pinned.end())
        {
            return it->second;
        }
        auto instance = create(path);
        _pinned.emplace(path, instance);
        auto& list = _cache[path];
        list.insert(list.begin(), instance);
        return instance;
    }

    std::shared_ptr<Instance> InstanceRegistry::acquire(std::string const& path)
    {
        auto lock = std::lock_guard{_mutex};
        if (auto const it = _pinned.find(path); it != _pinned.end())
        {
            return it->second;
        }
        auto& list = _cache[path];
        for (auto const& weak : list)
        {
            if (auto existing = weak.lock())
            {
                return existing;
            }
        }
        auto instance = create(path);
        list.push_back(instance);
        return instance;
    }

    std::unique_ptr<FlowClaim> InstanceRegistry::claimFlow(std::string const& path, std::string const& flowId, FlowFileId const& file)
    {
        auto lock = std::lock_guard{_mutex};
        auto& list = _cache[path];
        std::erase_if(list, [](auto const& weak) { return weak.expired(); });
        std::shared_ptr<Instance> same;
        std::shared_ptr<Instance> free;
        for (auto const& weak : list)
        {
            auto instance = weak.lock();
            if (!instance)
            {
                continue; // released since the erase above
            }
            Claim const* claim = nullptr;
            if (auto const claims = _claims.find(instance.get()); claims != _claims.end())
            {
                if (auto const it = claims->second.find(flowId); it != claims->second.end())
                {
                    claim = &it->second;
                }
            }
            if (claim == nullptr)
            {
                free = free ? free : instance;
            }
            else if (claim->file == file)
            {
                same = instance;
                break;
            }
        }
        auto instance = same ? same : free;
        if (!instance)
        {
            // Every instance of the path still reads an older file of this flow (the writer re-created it).
            instance = create(path);
            list.push_back(instance);
            log::debug("mxl_instance_for_recreated_flow", {{"path", path}, {"mxl_flow_id", flowId}, {"instances", list.size()}});
        }
        auto& claim = _claims[instance.get()][flowId];
        claim.file = file;
        ++claim.refs;
        return std::make_unique<FlowClaim>(*this, std::move(instance), flowId);
    }

    void InstanceRegistry::release(Instance const* instance, std::string const& flowId)
    {
        auto lock = std::lock_guard{_mutex};
        auto const claims = _claims.find(instance);
        if (claims == _claims.end())
        {
            return;
        }
        if (auto const it = claims->second.find(flowId); it != claims->second.end() && --it->second.refs == 0)
        {
            claims->second.erase(it);
        }
        if (claims->second.empty())
        {
            _claims.erase(claims);
        }
    }

    std::vector<std::string> InstanceRegistry::openPaths() const
    {
        auto lock = std::lock_guard{_mutex};
        std::vector<std::string> paths;
        for (auto const& [path, list] : _cache)
        {
            if (std::any_of(list.begin(), list.end(), [](auto const& weak) { return !weak.expired(); }))
            {
                paths.push_back(path);
            }
        }
        return paths;
    }

    std::size_t InstanceRegistry::instanceCount(std::string const& path) const
    {
        auto lock = std::lock_guard{_mutex};
        auto const it = _cache.find(path);
        return it == _cache.end()
                   ? 0
                   : static_cast<std::size_t>(std::count_if(it->second.begin(), it->second.end(), [](auto const& weak) { return !weak.expired(); }));
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
