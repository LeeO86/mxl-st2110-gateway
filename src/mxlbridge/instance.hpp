// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <mxl/mxl.h>

namespace mxlgw::mxlbridge
{
    char const* statusName(mxlStatus status);

    class MxlError : public std::runtime_error
    {
    public:
        MxlError(std::string const& what, mxlStatus status)
            : std::runtime_error(what + " (" + statusName(status) + ")")
            , _status(status)
        {}
        mxlStatus status() const { return _status; }

    private:
        mxlStatus _status;
    };

    /// RAII mxlInstance for one domain directory.
    class Instance
    {
    public:
        explicit Instance(std::string path);
        ~Instance();
        Instance(Instance const&) = delete;
        Instance& operator=(Instance const&) = delete;

        mxlInstance get() const { return _instance; }
        std::string const& path() const { return _path; }

        /// Domain-wide MXL garbage collection (only with gc_on_start, owner decision Q9).
        void garbageCollectAll();
        /// Reads `<domain>/<flowId>.mxl-flow/flow_def.json` through MXL; empty if absent.
        std::string flowDef(std::string const& flowId) const;

    private:
        std::string _path;
        mxlInstance _instance = nullptr;
    };

    /// One mxlInstance per domain path (§3.1, §8.5). Configured domains are pinned for the process
    /// lifetime; discovered domains are opened on first use and destroyed when the last user drops
    /// its reference. Thread-safe; never called from MTL lcores.
    class InstanceRegistry
    {
    public:
        std::shared_ptr<Instance> pin(std::string const& path);
        std::shared_ptr<Instance> acquire(std::string const& path);
        std::vector<std::string> openPaths() const;
        void clear();

    private:
        mutable std::mutex _mutex;
        std::map<std::string, std::shared_ptr<Instance>> _pinned;
        std::map<std::string, std::weak_ptr<Instance>> _cache;
    };

    /// MXL library version string (mxlGetVersion).
    std::string mxlVersionString();
}
