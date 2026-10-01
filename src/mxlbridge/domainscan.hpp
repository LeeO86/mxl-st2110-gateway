// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mxlbridge/domaindef.hpp"
#include "util/uuid.hpp"

namespace mxlgw::mxlbridge
{
    enum class DomainKind
    {
        Configured,
        Discovered,
        Mirror,
        Conflict,
    };

    char const* toName(DomainKind kind);

    struct ConfiguredDomainRef
    {
        std::string name;
        std::string path;
    };

    struct DomainEntry
    {
        std::string path;
        util::Uuid id;
        std::string label;
        std::string description;
        DomainKind kind = DomainKind::Discovered;
        std::string configuredName; // for configured domains
        std::string sourceHostId;   // mirror marker
        std::string ownerHostId;
        std::string fsType;
        bool tmpfs = false;
        std::uint64_t totalBytes = 0;
        std::uint64_t freeBytes = 0;
        std::size_t flowCount = 0;
    };

    struct SkippedDomain
    {
        std::string path;
        std::string reason;
    };

    struct ScanResult
    {
        std::vector<DomainEntry> domains;   // accessible (configured, discovered, mirror)
        std::vector<DomainEntry> conflicts; // duplicate ids, excluded from resolution
        std::vector<SkippedDomain> skipped;

        DomainEntry const* findById(util::Uuid const& id) const;
    };

    /// Scans `scanPath` (itself + direct subdirectories with a domain_def.json) plus the configured
    /// domains (§8.5). Pure filesystem work; never writes.
    ScanResult scanDomains(std::optional<std::string> const& scanPath, std::vector<ConfiguredDomainRef> const& configured);

    /// Flow directories of a domain: "<uuid>.mxl-flow".
    struct FlowDirEntry
    {
        util::Uuid id;
        std::string path;
        std::optional<nlohmann::json> flowDef;
    };
    std::vector<FlowDirEntry> listFlowDirs(std::string const& domainPath, bool readDefs = true);
    bool flowDirExists(std::string const& domainPath, util::Uuid const& flowId);
    std::string flowDirPath(std::string const& domainPath, util::Uuid const& flowId);

    /// Thread-safe directory of domains with "no negative caching": a failed lookup always rescans.
    class DomainDirectory
    {
    public:
        DomainDirectory(std::optional<std::string> scanPath, std::vector<ConfiguredDomainRef> configured);

        ScanResult rescan();
        ScanResult last() const;

        /// Finds an accessible domain by id; rescans when not found in the last result.
        std::optional<DomainEntry> findById(util::Uuid const& id);

        /// Finds the accessible domain that contains flow `flowId`, preferring `preferred` (§7.4 auto).
        std::optional<DomainEntry> findFlow(util::Uuid const& flowId, std::optional<util::Uuid> const& preferred);

        std::optional<std::string> const& scanPath() const { return _scanPath; }

    private:
        std::optional<std::string> _scanPath;
        std::vector<ConfiguredDomainRef> _configured;
        mutable std::mutex _mutex;
        ScanResult _last;
    };
}
