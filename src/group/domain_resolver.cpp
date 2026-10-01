// SPDX-License-Identifier: MIT
#include "group/domain_resolver.hpp"

#include "util/threading.hpp"

namespace mxlgw::group
{
    char const* Resolution::reason() const
    {
        switch (result)
        {
            case Result::Found: return "found";
            case Result::DomainNotFound: return "domain_not_found";
            case Result::DomainConflict: return "domain_conflict";
            case Result::FlowNotFound: return "flow_not_found";
            case Result::InstanceFailed: return "domain_open_failed";
        }
        return "domain_not_found";
    }

    DomainResolver::DomainResolver(mxlbridge::DomainDirectory& directory, mxlbridge::InstanceRegistry& instances)
        : _directory(directory)
        , _instances(instances)
        , _thread([this] { run(); })
    {}

    DomainResolver::~DomainResolver()
    {
        {
            std::lock_guard const lock{_mutex};
            _stop = true;
        }
        _cv.notify_all();
        _thread.join();
    }

    void DomainResolver::request(std::string const& key, util::Uuid const& domainId, util::Uuid const& flowId, Callback callback)
    {
        {
            std::lock_guard const lock{_mutex};
            if (!_pending.insert(key).second)
            {
                return;
            }
            _queue.push_back({key, domainId, flowId, std::move(callback)});
        }
        _cv.notify_one();
    }

    Resolution DomainResolver::resolve(util::Uuid const& domainId, std::optional<util::Uuid> const& flowId)
    {
        Resolution r;
        auto const domain = _directory.findById(domainId); // rescans on a miss (no negative caching)
        if (!domain)
        {
            auto const last = _directory.last();
            bool conflict = false;
            for (auto const& c : last.conflicts)
            {
                conflict = conflict || c.id == domainId;
            }
            r.result = conflict ? Resolution::Result::DomainConflict : Resolution::Result::DomainNotFound;
            return r;
        }
        r.domain = *domain;
        if (flowId && !mxlbridge::flowDirExists(domain->path, *flowId))
        {
            r.result = Resolution::Result::FlowNotFound;
            return r;
        }
        try
        {
            r.instance = _instances.acquire(domain->path);
        }
        catch (std::exception const& ex)
        {
            log::warn("mxl_domain_open_failed", {{"path", domain->path}, {"error", ex.what()}});
            r.result = Resolution::Result::InstanceFailed;
            return r;
        }
        r.result = Resolution::Result::Found;
        return r;
    }

    std::optional<mxlbridge::DomainEntry> DomainResolver::resolveAuto(util::Uuid const& groupDomainId, std::optional<util::Uuid> const& flowId)
    {
        if (flowId)
        {
            if (auto const d = _directory.findFlow(*flowId, groupDomainId))
            {
                return d;
            }
        }
        return _directory.findById(groupDomainId);
    }

    void DomainResolver::run()
    {
        util::setThreadName("mxl-resolver");
        std::unique_lock lock{_mutex};
        while (!_stop)
        {
            if (_queue.empty())
            {
                _cv.wait(lock);
                continue;
            }
            auto req = std::move(_queue.front());
            _queue.pop_front();
            lock.unlock();
            auto result = resolve(req.domainId, req.flowId);
            lock.lock();
            _pending.erase(req.key);
            lock.unlock();
            req.callback(std::move(result));
            lock.lock();
        }
    }
}
