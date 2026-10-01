// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/schema.hpp"

namespace mxlgw::config
{
    using EnvLookup = std::function<std::optional<std::string>(std::string const& name)>;

    /// Reads the process environment.
    EnvLookup processEnvironment();

    struct EnvBinding
    {
        std::string pointer;  // JSON pointer of the setting
        std::string variable; // variable that set it (canonical or alias)
    };

    struct EnvOverlay
    {
        nlohmann::json effective;         // file content with environment values applied
        std::vector<EnvBinding> bindings; // settings that come from the environment
        ValidationErrors errors;          // unparsable environment values

        std::optional<std::string> variableFor(std::string const& pointer) const;
    };

    /// Applies §9.1 overrides (environment > file) to the file JSON. Defaults are applied later by
    /// fromJson(); `provenanceOf` distinguishes the three sources.
    EnvOverlay applyEnvironment(nlohmann::json const& fileJson, EnvLookup const& env);

    /// "default", "file" or "env:<VAR>" for a JSON pointer.
    std::string provenanceOf(std::string const& pointer, nlohmann::json const& fileJson, EnvOverlay const& overlay);

    /// Provenance of every scalar setting in node/nic/ptp/mxl (for /api/config and the UI).
    std::map<std::string, std::string> provenanceMap(nlohmann::json const& fileJson, EnvOverlay const& overlay);

    /// Documentation table: canonical variable, aliases, JSON pointer.
    struct EnvVariableDoc
    {
        std::string variable;
        std::vector<std::string> aliases;
        std::string pointer;
        std::string type;
    };
    std::vector<EnvVariableDoc> documentedVariables();
}
