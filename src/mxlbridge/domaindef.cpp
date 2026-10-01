// SPDX-License-Identifier: MIT
#include "mxlbridge/domaindef.hpp"

#include <filesystem>

namespace mxlgw::mxlbridge
{
    std::optional<DomainDef> parseDomainDef(std::string const& text, std::string& error)
    {
        nlohmann::json j;
        try
        {
            j = nlohmann::json::parse(text);
        }
        catch (std::exception const& ex)
        {
            error = std::string("invalid JSON: ") + ex.what();
            return std::nullopt;
        }
        if (!j.is_object())
        {
            error = "not a JSON object";
            return std::nullopt;
        }
        for (auto const* key : {"id", "label", "description", "tags"})
        {
            if (!j.contains(key))
            {
                error = std::string("missing required field '") + key + "'";
                return std::nullopt;
            }
        }
        if (!j["id"].is_string())
        {
            error = "id must be a string";
            return std::nullopt;
        }
        auto const id = util::parseUuid(j["id"].get<std::string>());
        if (!id)
        {
            error = "id is not a UUID";
            return std::nullopt;
        }
        if (!j["label"].is_string() || !j["description"].is_string())
        {
            error = "label and description must be strings";
            return std::nullopt;
        }
        if (!j["tags"].is_object())
        {
            error = "tags must be an object";
            return std::nullopt;
        }
        for (auto const& [key, value] : j["tags"].items())
        {
            if (!value.is_array())
            {
                error = "tag '" + key + "' must be an array of strings";
                return std::nullopt;
            }
            for (auto const& v : value)
            {
                if (!v.is_string())
                {
                    error = "tag '" + key + "' must be an array of strings";
                    return std::nullopt;
                }
            }
        }
        DomainDef def;
        def.id = *id;
        def.label = j["label"].get<std::string>();
        def.description = j["description"].get<std::string>();
        def.tags = j["tags"];
        if (j.contains("x-mxl-fabrics-agent") && j["x-mxl-fabrics-agent"].is_object())
        {
            auto const& m = j["x-mxl-fabrics-agent"];
            // The marker's presence alone makes it a mirror domain (write protection is conservative).
            def.mirror = true;
            if (m.contains("source_host_id") && m["source_host_id"].is_string())
            {
                def.sourceHostId = m["source_host_id"].get<std::string>();
            }
            if (m.contains("owner_host_id") && m["owner_host_id"].is_string())
            {
                def.ownerHostId = m["owner_host_id"].get<std::string>();
            }
        }
        return def;
    }

    std::string renderDomainDef(util::Uuid const& id, std::string const& label, std::string const& description)
    {
        nlohmann::ordered_json j;
        j["id"] = id.toString();
        j["label"] = label;
        j["description"] = description;
        j["tags"] = nlohmann::ordered_json::object();
        return j.dump(4) + "\n";
    }

    bool isMirrorBasename(std::string const& path)
    {
        auto p = std::filesystem::path(path);
        if (!p.has_filename())
        {
            p = p.parent_path();
        }
        return p.filename().string().rfind("mirror-", 0) == 0;
    }

    std::string domainDefPath(std::string const& domainPath)
    {
        return (std::filesystem::path(domainPath) / "domain_def.json").string();
    }

    std::string optionsPath(std::string const& domainPath)
    {
        return (std::filesystem::path(domainPath) / "options.json").string();
    }
}
