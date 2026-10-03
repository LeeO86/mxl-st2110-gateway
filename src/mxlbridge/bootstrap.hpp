// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "util/uuid.hpp"

namespace mxlgw::mxlbridge
{
    /// A domain that cannot be used; the process exits with 78 (EX_CONFIG, §8.3, §14.4).
    class BootstrapError : public std::runtime_error
    {
    public:
        BootstrapError(std::string event, std::string const& message, nlohmann::json fields)
            : std::runtime_error(message)
            , _event(std::move(event))
            , _fields(std::move(fields))
        {}
        std::string const& event() const { return _event; }
        nlohmann::json const& fields() const { return _fields; }

    private:
        std::string _event;
        nlohmann::json _fields;
    };

    struct BootstrapResult
    {
        std::string name;
        std::string path;
        util::Uuid id;
        std::string label;
        std::string description;
        std::string fsType;
        bool directoryCreated = false;
        bool domainDefCreated = false;
        bool optionsCreated = false;
        /// Id to write back into the config file (owner decision C6); nullopt when the config already
        /// holds it or the id comes from the environment.
        std::optional<util::Uuid> writeBackId;
        std::vector<std::string> warnings; // event names already logged
    };

    struct BootstrapOptions
    {
        /// The domain id is set by an environment variable or derived from node.seed: never written back (§9.1, §7.3).
        bool idNotPersisted = false;
        /// Root filesystem used for the "not the container root" test.
        std::string rootPath = "/";
    };

    /// Steps 1–5 of §8.3 (mount check, mirror check, directory, domain_def.json, options.json).
    /// Pure filesystem work; never overwrites an existing domain_def.json or options.json.
    /// Throws BootstrapError for every exit-78 condition.
    BootstrapResult bootstrapDomain(config::Domain const& domain, BootstrapOptions const& options = {});

    /// Own-flow garbage collection (owner decision Q9): removes `<domain>/<id>.mxl-flow` for each
    /// given flow id whose data file is not locked by any process (same test as MXL's
    /// mxlGarbageCollectFlows). Returns the ids removed.
    std::vector<util::Uuid> removeStaleFlows(std::string const& domainPath, std::vector<util::Uuid> const& flowIds);

    /// True if some process holds a lock on the flow's data file (writer or reader active).
    bool flowInUse(std::string const& domainPath, util::Uuid const& flowId);
}
