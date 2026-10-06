// SPDX-License-Identifier: MIT
#include "config/env.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>

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
            NullableBoolean,
            List,
            MillisecondsToNs,
            AutoOrInteger,
            JsonObject,
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
                case Kind::NullableBoolean: return "boolean";
                case Kind::List: return "comma list";
                case Kind::MillisecondsToNs: return "integer (ms)";
                case Kind::AutoOrInteger: return "auto | integer";
                case Kind::JsonObject: return "JSON object";
            }
            return "string";
        }

        // The platform's standard names (NMOS_*, MXL_*, WEB_PORT, SHUTDOWN_TIMEOUT_S) are canonical where they
        // exist; the MXLGW_ + upper-snake JSON path names stay valid as aliases (§9.1).
        std::vector<Entry> const& staticEntries()
        {
            static std::vector<Entry> const entries = []
            {
                std::vector<Entry> e = {
                    {"NMOS_SEED", {"MXLGW_NODE_SEED"}, "/node/seed", Kind::NullableString},
                    {"MXLGW_NODE_ID", {}, "/node/id", Kind::NullableString},
                    {"NMOS_LABEL", {"MXLGW_NODE_LABEL"}, "/node/label", Kind::String},
                    {"MXLGW_NODE_DESCRIPTION", {}, "/node/description", Kind::String},
                    {"NMOS_TAGS", {"MXLGW_NODE_TAGS"}, "/node/tags", Kind::JsonObject},
                    {"NMOS_PORT", {"MXLGW_NODE_HTTP_PORT", "MXLGW_HTTP_PORT"}, "/node/http_port", Kind::Integer},
                    {"WEB_PORT", {"MXLGW_NODE_WEB_PORT"}, "/node/web_port", Kind::NullableInteger},
                    {"NMOS_HOST_ADDRESS", {"MXLGW_NODE_HOST_ADDRESS", "MXLGW_NODE_PUBLIC_ADDRESS"}, "/node/host_address", Kind::NullableString},
                    {"MXLGW_NODE_PUBLIC_PORT", {}, "/node/public_port", Kind::NullableInteger},
                    {"MXLGW_NODE_MANAGEMENT_ADDRESSES", {}, "/node/management_addresses", Kind::List},
                    {"MXLGW_NODE_RESUME_CONNECTIONS", {}, "/node/resume_connections", Kind::Boolean},
                    {"MXLGW_NODE_LOG_LEVEL", {"MXLGW_LOG_LEVEL"}, "/node/log_level", Kind::String},
                    {"SHUTDOWN_TIMEOUT_S", {"MXLGW_NODE_SHUTDOWN_TIMEOUT_S"}, "/node/shutdown_timeout_s", Kind::Integer},
                    {"NMOS_DNS_SD", {"MXLGW_NODE_REGISTRY_DNS_SD"}, "/node/registry/dns_sd", Kind::NullableBoolean},
                    {"MXLGW_NODE_REGISTRY_MODE", {}, "/node/registry/mode", Kind::String},
                    {"NMOS_REGISTRY_ADDRESS", {"MXLGW_NODE_REGISTRY_ADDRESS"}, "/node/registry/address", Kind::NullableString},
                    {"NMOS_REGISTRY_PORT", {"MXLGW_NODE_REGISTRY_PORT"}, "/node/registry/port", Kind::NullableInteger},
                    {"NMOS_QUERY_ADDRESS", {"MXLGW_NODE_REGISTRY_QUERY_ADDRESS"}, "/node/registry/query_address", Kind::NullableString},
                    {"NMOS_QUERY_PORT", {"MXLGW_NODE_REGISTRY_QUERY_PORT"}, "/node/registry/query_port", Kind::NullableInteger},
                    {"MXLGW_NODE_ST2110_ENABLED", {}, "/node/st2110/enabled", Kind::Boolean},
                    {"MXLGW_NODE_ST2110_LABEL", {}, "/node/st2110/label", Kind::NullableString},
                    {"MXLGW_NODE_ST2110_HTTP_PORT", {}, "/node/st2110/http_port", Kind::NullableInteger},
                    {"MXLGW_NODE_ST2110_HOST_ADDRESS", {}, "/node/st2110/host_address", Kind::NullableString},
                    {"MXLGW_NODE_ST2110_REGISTRY_DNS_SD", {}, "/node/st2110/registry/dns_sd", Kind::NullableBoolean},
                    {"MXLGW_NODE_ST2110_REGISTRY_ADDRESS", {}, "/node/st2110/registry/address", Kind::NullableString},
                    {"MXLGW_NODE_ST2110_REGISTRY_PORT", {}, "/node/st2110/registry/port", Kind::NullableInteger},
                    {"MXLGW_NODE_TLS_ENABLED", {}, "/node/tls/enabled", Kind::Boolean},
                    {"MXLGW_NODE_TLS_CERTIFICATE", {}, "/node/tls/certificate", Kind::NullableString},
                    {"MXLGW_NODE_TLS_PRIVATE_KEY", {}, "/node/tls/private_key", Kind::NullableString},
                    {"MXLGW_NIC_BACKEND", {}, "/nic/backend", Kind::String},
                    {"MXLGW_NIC_LCORES", {}, "/nic/lcores", Kind::NullableString},
                    {"MXLGW_NIC_LCORE_COUNT", {}, "/nic/lcore_count", Kind::Integer},
                    {"MXLGW_NIC_TX_PACING", {}, "/nic/tx_pacing", Kind::String},
                    {"MXLGW_NIC_APP_CPUS", {}, "/nic/app_cpus", Kind::NullableString},
                    {"MXLGW_NIC_HUGEPAGE_SOCKET", {}, "/nic/hugepage_socket", Kind::AutoOrInteger},
                    {"MXLGW_PTP_MODE", {}, "/ptp/mode", Kind::String},
                    {"MXLGW_PTP_DOMAIN", {}, "/ptp/domain", Kind::Integer},
                    {"MXLGW_PTP_REQUIRE_LOCK", {}, "/ptp/require_lock", Kind::Boolean},
                    {"MXLGW_PTP_WARN_OFFSET_NS", {}, "/ptp/warn_offset_ns", Kind::Integer},
                    {"MXLGW_PTP_MAX_OFFSET_NS", {}, "/ptp/max_offset_ns", Kind::Integer},
                    {"MXL_DOMAIN_SCAN_PATH", {"MXLGW_MXL_SCAN_PATH"}, "/mxl/scan_path", Kind::NullableString},
                    {"MXLGW_MXL_DEFAULT_READ_OFFSET_GRAINS", {"MXL_READ_OFFSET_GRAINS"}, "/mxl/default_read_offset_grains", Kind::NullableInteger},
                    {"MXLGW_MXL_DEFAULT_READ_OFFSET_NS", {}, "/mxl/default_read_offset_ns", Kind::NullableInteger},
                    {"MXL_READ_OFFSET_MS", {}, "/mxl/default_read_offset_ns", Kind::MillisecondsToNs},
                    {"MXL_CLEANUP_ON_EXIT", {"MXLGW_MXL_CLEANUP_ON_EXIT"}, "/mxl/cleanup_on_exit", Kind::Boolean},
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
                case Kind::NullableBoolean:
                {
                    if (kind == Kind::NullableBoolean && (text.empty() || text == "null"))
                    {
                        return json(nullptr);
                    }
                    auto const v = util::parseBool(text);
                    if (!v)
                    {
                        error = "invalid boolean '" + text + "' (use true/false)";
                        return std::nullopt;
                    }
                    return json(*v);
                }
                case Kind::JsonObject:
                {
                    if (text.empty())
                    {
                        return json::object();
                    }
                    try
                    {
                        auto j = json::parse(text);
                        if (!j.is_object())
                        {
                            error = "must be a JSON object";
                            return std::nullopt;
                        }
                        return j;
                    }
                    catch (json::parse_error const& ex)
                    {
                        error = std::string("invalid JSON: ") + ex.what();
                        return std::nullopt;
                    }
                }
                case Kind::AutoOrInteger:
                {
                    if (text == "auto")
                    {
                        return json("auto");
                    }
                    auto const v = util::parseInt(text);
                    if (!v)
                    {
                        error = "invalid value '" + text + "' (use auto or an integer)";
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

        struct Candidate
        {
            std::string variable;
            Kind kind;
        };

        // All variables of one setting: canonical name first. Several of them set to different values is a
        // configuration error (never guessed silently); equal values are fine.
        auto apply = [&](std::string const& pointer, std::vector<Candidate> const& candidates)
        {
            std::optional<json> chosen;
            std::string chosenVariable;
            bool failed = false;
            for (auto const& c : candidates)
            {
                auto const raw = env(c.variable);
                if (!raw)
                {
                    continue;
                }
                std::string error;
                auto const value = parseValue(c.kind, *raw, error);
                if (!value)
                {
                    overlay.errors.push_back({pointer, "environment variable " + c.variable + ": " + error});
                    failed = true;
                    continue;
                }
                if (!chosen)
                {
                    chosen = value;
                    chosenVariable = c.variable;
                }
                else if (*chosen != *value)
                {
                    overlay.errors.push_back({pointer, "environment variables " + chosenVariable + " and " + c.variable +
                                                           " are both set, to different values; set only one (" + chosenVariable + ")"});
                    failed = true;
                }
            }
            if (!chosen || failed)
            {
                return;
            }
            setAt(overlay.effective, pointer, *chosen);
            for (auto& b : overlay.bindings)
            {
                if (b.pointer == pointer)
                {
                    b.variable = chosenVariable;
                    return;
                }
            }
            overlay.bindings.push_back({pointer, chosenVariable});
        };

        std::vector<std::pair<std::string, std::vector<Candidate>>> settings;
        for (auto const& entry : staticEntries())
        {
            auto it = std::find_if(settings.begin(), settings.end(), [&](auto const& s) { return s.first == entry.pointer; });
            if (it == settings.end())
            {
                settings.push_back({entry.pointer, {}});
                it = std::prev(settings.end());
            }
            it->second.push_back({entry.variable, entry.kind});
            for (auto const& alias : entry.aliases)
            {
                it->second.push_back({alias, entry.kind});
            }
        }
        for (auto const& [pointer, candidates] : settings)
        {
            apply(pointer, candidates);
        }

        // MXL_OUTPUT_DOMAIN_* configure the first configured domain, created as "main" when the file has none.
        bool const outputDomainFromEnv = env("MXL_OUTPUT_DOMAIN_DIR") || env("MXL_OUTPUT_DOMAIN_ID") || env("MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS");
        if (outputDomainFromEnv)
        {
            auto& mxl = overlay.effective["mxl"];
            if (!mxl.is_object())
            {
                mxl = json::object();
            }
            if (!mxl.contains("domains") || !mxl["domains"].is_array())
            {
                mxl["domains"] = json::array();
            }
            if (mxl["domains"].empty())
            {
                mxl["domains"].push_back({{"name", "main"}});
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
                auto withOutput = [&](char const* outputVariable, std::string const& own, Kind kind)
                {
                    std::vector<Candidate> c;
                    if (i == 0)
                    {
                        c.push_back({outputVariable, kind});
                    }
                    c.push_back({own, kind});
                    return c;
                };
                apply(base + "path", withOutput("MXL_OUTPUT_DOMAIN_DIR", prefix + "PATH", Kind::String));
                apply(base + "id", withOutput("MXL_OUTPUT_DOMAIN_ID", prefix + "ID", Kind::NullableString));
                apply(base + "label", {{prefix + "LABEL", Kind::NullableString}});
                apply(base + "description", {{prefix + "DESCRIPTION", Kind::NullableString}});
                apply(base + "history_duration_ns", withOutput("MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS", prefix + "HISTORY_DURATION_NS", Kind::NullableInteger));
                apply(base + "gc_on_start", {{prefix + "GC_ON_START", Kind::Boolean}});
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
        docs.push_back({"MXL_OUTPUT_DOMAIN_DIR", {}, "/mxl/domains/0/path", "string"});
        docs.push_back({"MXL_OUTPUT_DOMAIN_ID", {}, "/mxl/domains/0/id", "string"});
        docs.push_back({"MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS", {}, "/mxl/domains/0/history_duration_ns", "integer"});
        docs.push_back({"MXLGW_MXL_DOMAIN_<NAME>_{PATH,ID,LABEL,DESCRIPTION,HISTORY_DURATION_NS,GC_ON_START}", {}, "/mxl/domains/<i>/...", "per field"});
        docs.push_back({"MXLGW_CONFIG", {}, "(bootstrap) configuration file path", "string"});
        docs.push_back({"MXLGW_LOG_FORMAT", {}, "(bootstrap) json | text", "string"});
        return docs;
    }
}
