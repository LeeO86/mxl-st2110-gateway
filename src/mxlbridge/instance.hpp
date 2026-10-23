// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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

    /// Device and inode of a flow's data file (`<domain>/<flowId>.mxl-flow/data`). A writer that re-creates
    /// a flow under the same id creates a new file, so the pair changes; MXL itself compares the inode
    /// (VERIFIED: dmf-mxl/mxl@218ddaa lib/internal/src/PosixDiscreteFlowReader.cpp:244-255).
    struct FlowFileId
    {
        std::uint64_t dev = 0;
        std::uint64_t ino = 0;
        bool operator==(FlowFileId const&) const = default;
    };
    /// nullopt if the data file does not exist.
    std::optional<FlowFileId> flowFileId(std::string const& domainPath, std::string const& flowId);

    class InstanceRegistry;

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
        /// The registry that created this instance; nullptr for a standalone instance.
        InstanceRegistry* registry() const { return _registry; }

        /// Domain-wide MXL garbage collection (only with gc_on_start, owner decision Q9).
        void garbageCollectAll();
        /// Reads `<domain>/<flowId>.mxl-flow/flow_def.json` through MXL; empty if absent.
        std::string flowDef(std::string const& flowId) const;

    private:
        friend class InstanceRegistry;
        std::string _path;
        mxlInstance _instance = nullptr;
        InstanceRegistry* _registry = nullptr;
    };

    /// A flow reader's hold on one flow in one instance (InstanceRegistry::claimFlow). Released after the
    /// MXL reader (FlowReader releases its reader first).
    class FlowClaim
    {
    public:
        FlowClaim(InstanceRegistry& registry, std::shared_ptr<Instance> instance, std::string flowId);
        ~FlowClaim();
        FlowClaim(FlowClaim const&) = delete;
        FlowClaim& operator=(FlowClaim const&) = delete;

        std::shared_ptr<Instance> const& instance() const { return _instance; }

    private:
        InstanceRegistry& _registry;
        std::shared_ptr<Instance> _instance;
        std::string _flowId;
    };

    /// mxlInstances per domain path (§3.1, §8.5). Configured domains are pinned for the process
    /// lifetime; discovered domains are opened on first use and destroyed when the last user drops
    /// its reference. Thread-safe; never called from MTL lcores. Must outlive every reader of its instances.
    ///
    /// Usually one instance per path. MXL shares one reader per flow id within an instance
    /// (VERIFIED: dmf-mxl/mxl@218ddaa lib/internal/src/Instance.cpp:121-143 — getFlowReader only adds a
    /// reference to a cached reader). A reader still open on a flow that its writer deleted and created
    /// again maps the deleted files, and every later open of that flow in the same instance got it back.
    /// claimFlow therefore keeps one data file (FlowFileId) per flow and instance and opens a further
    /// instance of the path only while an older file of the flow is still read in every existing one.
    class InstanceRegistry
    {
    public:
        std::shared_ptr<Instance> pin(std::string const& path);
        std::shared_ptr<Instance> acquire(std::string const& path);
        /// The instance to open a reader of `flowId` (data file `file`) in: one that reads this file of the
        /// flow already, else one that does not read the flow, else a new one.
        std::unique_ptr<FlowClaim> claimFlow(std::string const& path, std::string const& flowId, FlowFileId const& file);
        std::vector<std::string> openPaths() const;
        /// Live instances of `path` (tests, diagnostics).
        std::size_t instanceCount(std::string const& path) const;
        void clear();

    private:
        friend class FlowClaim;
        struct Claim
        {
            FlowFileId file;
            std::size_t refs = 0;
        };
        std::shared_ptr<Instance> create(std::string const& path);
        void release(Instance const* instance, std::string const& flowId);

        mutable std::mutex _mutex;
        std::map<std::string, std::shared_ptr<Instance>> _pinned;
        /// Every instance of a path; the first live one is the path's default (acquire()).
        std::map<std::string, std::vector<std::weak_ptr<Instance>>> _cache;
        /// Flows read per instance; an entry exists only while a claim holds the instance.
        std::map<Instance const*, std::map<std::string, Claim>> _claims;
    };

    /// MXL library version string (mxlGetVersion).
    std::string mxlVersionString();
}
