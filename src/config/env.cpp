// SPDX-License-Identifier: MIT
#include "config/env.hpp"

#include <algorithm>
#include <cstdlib>

#include "util/strings.hpp"

namespace mxlgw::config
{
    namespace
    {
        using json = nlohmann::json;

        enum class Kind
        {
            String,
            NullableString,
            Integer,
            NullableInteger,
            Boolean,
            List,
            MillisecondsToNs,
        };

        struct Entry
        {
            std::string variable;
            std::vector<std::string> aliases;
            std::string pointer;
            Kind kind;
        };

        char const* kindName(Kind k)
        {
            switch (k)
            {
                case Kind::String: return "string";
                case Kind::NullableString: return "string";
                case Kind::Integer: return "integer";
                case Kind::NullableInteger: return "integer";
                case Kind::Boolean: return "boolean";
                case Kind::List: return "comma list";
                case Kind::MillisecondsToNs: return "integer (ms)";
            }
            return "string";
        }

        std::vector<Entry> const& staticEntries()
        {
            static std::vector<Entry> const entries = []
            {
                std::vector<Entry> e = {
                    {"MXLGW_NODE_ID", {}, "/node/id", Kind::NullableString},
                    {"MXLGW_NODE_LABEL", {}, "/node/label", Kind::String},
                    {"MXLGW_NODE_DESCRIPTION", {}, "/node/description", Kind::String},
                    {"MXLGW_NODE_HTTP_PORT", {"MXLGW_HTTP_PORT"}, "/node/http_port", Kind::Integer},
                    {"MXLGW_NODE_PUBLIC_ADDRESS", {}, "/node/public_address", Kind::NullableString},
                    {"MXLGW_NODE_PUBLIC_PORT", {}, "/node/public_port", Kind::NullableInteger},
                    {"MXLGW_NODE_MANAGEMENT_ADDRESSES", {}, "/node/management_addresses", Kind::List},
                    {"MXLGW_NODE_RESUME_CONNECTIONS", {}, "/node/resume_connections", Kind::Boolean},
                    {"MXLGW_NODE_LOG_LEVEL", {"MXLGW_LOG_LEVEL"}, "/node/log_level", Kind::String},
                    {"MXLGW_NODE_REGISTRY_MODE", {}, "/node/registry/mode", Kind::String},
                    {"MXLGW_NODE_REGISTRY_ADDRESS", {}, "/node/registry/address", Kind::NullableString},
                    {"MXLGW_NODE_REGISTRY_PORT", {}, "/node/registry/port", Kind::NullableInteger},
                    {"MXLGW_NODE_TLS_ENABLED", {}, "/node/tls/enabled", Kind::Boolean},
                    {"MXLGW_NODE_TLS_CERTIFICATE", {}, "/node/tls/certificate", Kind::NullableString},
                    {"MXLGW_NODE_TLS_PRIVATE_KEY", {}, "/node/tls/private_key", Kind::NullableString},
                    {"MXLGW_NIC_BACKEND", {}, "/nic/backend", Kind::String},
                    {"MXLGW_NIC_LCORES", {}, "/nic/lcores", Kind::NullableString},
                    {"MXLGW_NIC_APP_CPUS", {}, "/nic/app_cpus", Kind::NullableString},
                    {"MXLGW_PTP_MODE", {}, "/ptp/mode", Kind::String},
                    {"MXLGW_PTP_DOMAIN", {}, "/ptp/domain", Kind::Integer},
                    {"MXLGW_PTP_REQUIRE_LOCK", {}, "/ptp/require_lock", Kind::Boolean},
                    {"MXLGW_PTP_WARN_OFFSET_NS", {}, "/ptp/warn_offset_ns", Kind::Integer},
                    {"MXLGW_PTP_MAX_OFFSET_NS", {}, "/ptp/max_offset_ns", Kind::Integer},
                    {"MXLGW_MXL_SCAN_PATH", {"MXL_DOMAIN_SCAN_PATH"}, "/mxl/scan_path", Kind::NullableString},
                    {"MXLGW_MXL_DEFAULT_READ_OFFSET_GRAINS", {"MXL_READ_OFFSET_GRAINS"}, "/mxl/default_read_offset_grains", Kind::NullableInteger},
                    {"MXLGW_MXL_DEFAULT_READ_OFFSET_NS", {}, "/mxl/default_read_offset_ns", Kind::NullableInteger},
                    {"MXL_READ_OFFSET_MS", {}, "/mxl/default_read_offset_ns", Kind::MillisecondsToNs},
                };
                for (auto const* side : {"PRIMARY", "REDUNDANT"})
                {
                    std::string const ptrSide = std::string(side) == "PRIMARY" ? "primary" : "redundant";
                    for (auto const* field : {"NAME", "PCI", "IFNAME", "IP", "NETMASK", "GATEWAY"})
                    {
                        e.push_back({std::string("MXLGW_NIC_") + side + "_" + field,
                                     {},
                                     "/nic/port_pairs/0/" + ptrSide + "/" + util::toLower(field),
                                     Kind::NullableString});
                    }
                }
                return e;
            }();
            return entries;
        }

        std::optional<json> parseValue(Kind kind, std::string const& raw, std::string& error)
        {
            auto const text = util::trim(raw);
            switch (kind)
            {
                case Kind::String: return json(text);
                case Kind::NullableString:
                    if (text.empty() || text == "null")
                    {
                        return json(nullptr);
                    }
                    return json(text);
                case Kind::Integer:
                case Kind::NullableInteger:
                case Kind::MillisecondsToNs:
                {
                    if (kind != Kind::Integer && (text.empty() || text == "null"))
                    {
                        return json(nullptr);
                    }
                    auto const v = util::parseInt(text);
                    if (!v)
                    {
                        error = "invalid integer '" + text + "'";
                        return std::nullopt;
                    }
                    return kind == Kind::MillisecondsToNs ? json(*v * 1'000'000) : json(*v);
                }
                case Kind::Boolean:
                {
                    auto const v = util::parseBool(text);
                    if (!v)
                    {
                        error = "invalid boolean '" + text + "' (use true/false)";
                        return std::nullopt;
                    }
                    return json(*v);
                }
                case Kind::List:
                {
                    auto arr = json::array();
                    for (auto const& item : util::split(text, ','))
                    {
                        arr.push_back(item);
                    }
                    return arr;
                }
            }
            return std::nullopt;
        }

        void setAt(json& root, std::string const& pointer, json value)
        {
            // Create missing objects/arrays along the way (port_pairs/0 may not exist yet).
            auto const tokens = util::split(pointer, '/');
            json* node = &root;
            for (std::size_t i = 0; i < tokens.size(); ++i)
            {
                auto const& tok = tokens[i];
                bool const last = i + 1 == tokens.size();
                bool const index = !tok.empty() && std::all_of(tok.begin(), tok.end(), [](char c) { return c >= '0' && c <= '9'; });
                if (index)
                {
                    if (!node->is_array())
                    {
                        *node = json::array();
                    }
                    auto const idx = static_cast<std::size_t>(std::stoul(tok));
                    while (node->size() <= idx)
                    {
                        node->push_back(json::object());
                    }
                    node = &(*node)[idx];
                }
                else
                {
                    if (!node->is_object())
                    {
                        *node = json::object();
                    }
                    node = &(*node)[tok];
                }
                if (last)
                {
                    *node = std::move(value);
                }
            }
        }

        bool fileHas(json const& fileJson, std::string const& pointer)
        {
            try
            {
                return fileJson.contains(json::json_pointer(pointer));
            }
            catch (...)
            {
                return false;
            }
        }
    }

    EnvLookup processEnvironment()
    {
        return [](std::string const& name) -> std::optional<std::string>
        {
            char const* v = std::getenv(name.c_str());
            if (v == nullptr)
            {
                return std::nullopt;
            }
            return std::string(v);
        };
    }

    std::optional<std::string> EnvOverlay::variableFor(std::string const& pointer) const
    {
        for (auto const& b : bindings)
        {
            if (b.pointer == pointer)
            {
                return b.variable;
            }
        }
        return std::nullopt;
    }

    EnvOverlay applyEnvironment(json const& fileJson, EnvLookup const& env)
    {
        EnvOverlay overlay;
        overlay.effective = fileJson.is_object() ? fileJson : json::object();

        auto apply = [&](std::string const& variable, std::string const& pointer, Kind kind)
        {
            auto const raw = env(variable);
            if (!raw)
            {
                return false;
            }
            std::string error;
            auto const value = parseValue(kind, *raw, error);
            if (!value)
            {
                overlay.errors.push_back({pointer, "environment variable " + variable + ": " + error});
                return true;
            }
            setAt(overlay.effective, pointer, *value);
            for (auto& b : overlay.bindings)
            {
                if (b.pointer == pointer)
                {
                    b.variable = variable;
                    return true;
                }
            }
            overlay.bindings.push_back({pointer, variable});
            return true;
        };

        for (auto const& entry : staticEntries())
        {
            // Canonical name wins over aliases.
            if (!apply(entry.variable, entry.pointer, entry.kind))
            {
                for (auto const& alias : entry.aliases)
                {
                    if (apply(alias, entry.pointer, entry.kind))
                    {
                        break;
                    }
                }
            }
        }

        // Default port names when the environment creates a port pair from scratch.
        if (overlay.effective.contains("nic") && overlay.effective["nic"].contains("port_pairs"))
        {
            auto& pairs = overlay.effective["nic"]["port_pairs"];
            if (pairs.is_array() && !pairs.empty())
            {
                auto& pair = pairs[0];
                if (pair.contains("primary") && pair["primary"].is_object() && !pair["primary"].contains("name"))
                {
                    pair["primary"]["name"] = "media-p";
                }
                if (pair.contains("redundant") && pair["redundant"].is_object() && !pair["redundant"].contains("name"))
                {
                    pair["redundant"]["name"] = "media-r";
                }
            }
        }

        // Per-domain overrides for domains defined in the file.
        if (overlay.effective.contains("mxl") && overlay.effective["mxl"].contains("domains") && overlay.effective["mxl"]["domains"].is_array())
        {
            auto const count = overlay.effective["mxl"]["domains"].size();
            for (std::size_t i = 0; i < count; ++i)
            {
                auto const& d = overlay.effective["mxl"]["domains"][i];
                if (!d.contains("name") || !d["name"].is_string())
                {
                    continue;
                }
                auto const prefix = "MXLGW_MXL_DOMAIN_" + util::toUpperSnake(d["name"].get<std::string>()) + "_";
                auto const base = "/mxl/domains/" + std::to_string(i) + "/";
                apply(prefix + "PATH", base + "path", Kind::String);
                apply(prefix + "ID", base + "id", Kind::NullableString);
                apply(prefix + "LABEL", base + "label", Kind::NullableString);
                apply(prefix + "DESCRIPTION", base + "description", Kind::NullableString);
                apply(prefix + "HISTORY_DURATION_NS", base + "history_duration_ns", Kind::NullableInteger);
                apply(prefix + "GC_ON_START", base + "gc_on_start", Kind::Boolean);
            }
        }
        return overlay;
    }

    std::string provenanceOf(std::string const& pointer, json const& fileJson, EnvOverlay const& overlay)
    {
        if (auto const var = overlay.variableFor(pointer))
        {
            return "env:" + *var;
        }
        return fileHas(fileJson, pointer) ? "file" : "default";
    }

    std::map<std::string, std::string> provenanceMap(json const& fileJson, EnvOverlay const& overlay)
    {
        std::map<std::string, std::string> out;
        for (auto const& entry : staticEntries())
        {
            out[entry.pointer] = provenanceOf(entry.pointer, fileJson, overlay);
        }
        for (auto const& b : overlay.bindings)
        {
            out[b.pointer] = "env:" + b.variable;
        }
        if (fileJson.contains("mxl") && fileJson["mxl"].contains("domains") && fileJson["mxl"]["domains"].is_array())
        {
            for (std::size_t i = 0; i < fileJson["mxl"]["domains"].size(); ++i)
            {
                for (auto const* field : {"path", "id", "label", "description", "history_duration_ns", "gc_on_start"})
                {
                    auto const ptr = "/mxl/domains/" + std::to_string(i) + "/" + field;
                    out[ptr] = provenanceOf(ptr, fileJson, overlay);
                }
            }
        }
        return out;
    }

    std::vector<EnvVariableDoc> documentedVariables()
    {
        std::vector<EnvVariableDoc> docs;
        for (auto const& e : staticEntries())
        {
            docs.push_back({e.variable, e.aliases, e.pointer, kindName(e.kind)});
        }
        docs.push_back({"MXLGW_MXL_DOMAIN_<NAME>_{PATH,ID,LABEL,DESCRIPTION,HISTORY_DURATION_NS,GC_ON_START}", {}, "/mxl/domains/<i>/...", "per field"});
        docs.push_back({"MXLGW_CONFIG", {}, "(bootstrap) configuration file path", "string"});
        docs.push_back({"MXLGW_LOG_FORMAT", {}, "(bootstrap) json | text", "string"});
        return docs;
    }
}
