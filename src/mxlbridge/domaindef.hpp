// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "util/uuid.hpp"

namespace mxlgw::mxlbridge
{
    /// BCP-007-03 domain definition (`domain_def.json`), schema mxl_domain_definition.json:
    /// required id, label, description, tags; unknown fields are ignored (§8.1).
    struct DomainDef
    {
        util::Uuid id;
        std::string label;
        std::string description;
        nlohmann::json tags = nlohmann::json::object();
        /// mxl-fabrics-agent marker ("x-mxl-fabrics-agent": {"mirror": true, ...}), §8.1.
        bool mirror = false;
        std::string sourceHostId;
        std::string ownerHostId;
    };

    /// Parses and validates; `error` describes the first problem on failure.
    std::optional<DomainDef> parseDomainDef(std::string const& text, std::string& error);

    std::string renderDomainDef(util::Uuid const& id, std::string const& label, std::string const& description);

    /// True for a directory whose basename starts with "mirror-" (mxl-fabrics-agent layout).
    bool isMirrorBasename(std::string const& path);

    std::string domainDefPath(std::string const& domainPath);
    std::string optionsPath(std::string const& domainPath);
    inline constexpr char historyDurationOption[] = "urn:x-mxl:option:history_duration/v1.0";
}
