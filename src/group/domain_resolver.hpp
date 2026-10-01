// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "mxlbridge/domainscan.hpp"
#include "mxlbridge/instance.hpp"
#include "util/logging.hpp"

namespace mxlgw::group
{
    struct Resolution
    {
        enum class Result
        {
            Found,
            DomainNotFound,
            DomainConflict,
            FlowNotFound,
            InstanceFailed,
        };
        Result result = Result::DomainNotFound;
        mxlbridge::DomainEntry domain;
        std::shared_ptr<mxlbridge::Instance> instance;

        bool found() const { return result == Result::Found; }
        char const* reason() const;
    };

    /// Resolves MXL Receiver targets to domains (§7.4, §8.5) off the media threads (§3.6): scans run on
    /// the resolver thread (or inline on the control thread); a failed lookup always rescans and its
    /// "not found" outcome is never remembered.
    class DomainResolver
    {
    public:
        using Callback = std::function<void(Resolution)>;

        DomainResolver(mxlbridge::DomainDirectory& directory, mxlbridge::InstanceRegistry& instances);
        ~DomainResolver();
        DomainResolver(DomainResolver const&) = delete;
        DomainResolver& operator=(DomainResolver const&) = delete;

        /// Asynchronous resolution; `key` de-duplicates requests of one receiver.
        void request(std::string const& key, util::Uuid const& domainId, util::Uuid const& flowId, Callback callback);
        /// Synchronous resolution (control thread). `flowId` optional: domain only.
        Resolution resolve(util::Uuid const& domainId, std::optional<util::Uuid> const& flowId);
        /// BCP-007-03 `auto`: the group's domain, or the accessible domain that holds `flowId`.
        std::optional<mxlbridge::DomainEntry> resolveAuto(util::Uuid const& groupDomainId, std::optional<util::Uuid> const& flowId);

        mxlbridge::DomainDirectory& directory() { return _directory; }

    private:
        struct Request
        {
            std::string key;
            util::Uuid domainId;
            util::Uuid flowId;
            Callback callback;
        };
        void run();

        mxlbridge::DomainDirectory& _directory;
        mxlbridge::InstanceRegistry& _instances;
        std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<Request> _queue;
        std::set<std::string> _pending;
        bool _stop = false;
        std::thread _thread;
    };
}
