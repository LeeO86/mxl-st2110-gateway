// SPDX-License-Identifier: MIT
#include "config/store.hpp"

#include <filesystem>

#include <openssl/evp.h>

#include "config/minimal_embedded.hpp"
#include "util/fs.hpp"
#include "util/logging.hpp"
#include "util/uuid.hpp"

namespace mxlgw::config
{
    namespace
    {
        using ojson = nlohmann::ordered_json;

        nlohmann::json toPlain(ojson const& j)
        {
            return nlohmann::json::parse(j.dump());
        }

        ojson parseOrdered(std::string const& text, ValidationErrors& errors)
        {
            try
            {
                return ojson::parse(text);
            }
            catch (nlohmann::json::parse_error const& ex)
            {
                errors.push_back({"", std::string("invalid JSON: ") + ex.what()});
                return ojson();
            }
        }

        void forEachEssence(ojson& group, std::function<void(ojson&)> const& fn)
        {
            for (auto const* type : {"video", "audio", "anc"})
            {
                if (group.contains(type) && group[type].is_array())
                {
                    for (auto& e : group[type])
                    {
                        fn(e);
                    }
                }
            }
        }

        bool missingOrNull(ojson const& obj, char const* key)
        {
            return !obj.contains(key) || obj[key].is_null() || (obj[key].is_string() && obj[key].get<std::string>().empty());
        }
    }

    ConfigError::ConfigError(ValidationErrors errors)
        : std::runtime_error("invalid configuration:\n" + formatErrors(errors))
        , _errors(std::move(errors))
    {}

    std::string renderFile(ojson const& file)
    {
        return file.dump(2) + "\n";
    }

    void regenerateIds(ojson& file)
    {
        if (file.contains("node") && file["node"].is_object())
        {
            file["node"]["id"] = nullptr;
        }
        if (file.contains("mxl") && file["mxl"].contains("domains") && file["mxl"]["domains"].is_array())
        {
            for (auto& d : file["mxl"]["domains"])
            {
                d["id"] = nullptr;
            }
        }
        if (file.contains("groups") && file["groups"].is_array())
        {
            for (auto& g : file["groups"])
            {
                g["uid"] = util::uuidV4().toString();
                forEachEssence(g, [](ojson& e) { e["uid"] = util::uuidV4().toString(); });
            }
        }
    }

    bool fillGeneratedIds(ojson& file, EnvOverlay const& overlay)
    {
        bool changed = false;
        if (!overlay.variableFor("/node/id"))
        {
            if (!file.contains("node") || !file["node"].is_object())
            {
                file["node"] = ojson::object();
            }
            if (missingOrNull(file["node"], "id"))
            {
                file["node"]["id"] = util::uuidV4().toString();
                changed = true;
            }
        }
        if (file.contains("groups") && file["groups"].is_array())
        {
            for (auto& g : file["groups"])
            {
                if (g.is_object() && missingOrNull(g, "uid"))
                {
                    g["uid"] = util::uuidV4().toString();
                    changed = true;
                }
                forEachEssence(g,
                               [&](ojson& e)
                               {
                                   if (e.is_object() && missingOrNull(e, "uid"))
                                   {
                                       e["uid"] = util::uuidV4().toString();
                                       changed = true;
                                   }
                               });
            }
        }
        return changed;
    }

    ConfigStore::ConfigStore(std::string path, EnvLookup env)
        : _path(std::move(path))
        , _env(std::move(env))
    {}

    std::string ConfigStore::computeEtag(std::string const& raw)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_Digest(raw.data(), raw.size(), digest, &len, EVP_sha256(), nullptr);
        static constexpr char hex[] = "0123456789abcdef";
        std::string out = "\"";
        for (unsigned int i = 0; i < 12 && i < len; ++i)
        {
            out += hex[digest[i] >> 4];
            out += hex[digest[i] & 0xF];
        }
        out += "\"";
        return out;
    }

    ParseResult ConfigStore::validateFile(ojson const& fileJson, EnvOverlay* overlayOut) const
    {
        auto overlay = applyEnvironment(toPlain(fileJson), _env);
        ParseResult result;
        if (!overlay.errors.empty())
        {
            result.errors = overlay.errors;
        }
        else
        {
            result = parseAndValidate(overlay.effective);
        }
        if (overlayOut != nullptr)
        {
            *overlayOut = std::move(overlay);
        }
        return result;
    }

    void ConfigStore::rebuildSnapshot(ojson file, std::string raw)
    {
        EnvOverlay overlay;
        auto parsed = validateFile(file, &overlay);
        if (!parsed.ok())
        {
            throw ConfigError(parsed.errors);
        }
        Snapshot s;
        s.provenance = provenanceMap(toPlain(file), overlay);
        s.file = std::move(file);
        s.etag = computeEtag(raw);
        s.raw = std::move(raw);
        s.overlay = std::move(overlay);
        s.config = std::move(*parsed.config);
        _snapshot = std::move(s);
    }

    ConfigStore::LoadResult ConfigStore::load()
    {
        std::lock_guard const lock{_mutex};
        auto result = LoadResult::Loaded;
        auto text = util::readFile(_path);
        if (!text)
        {
            auto const dir = std::filesystem::path(_path).parent_path();
            if (!dir.empty())
            {
                std::filesystem::create_directories(dir);
            }
            text = std::string(embedded::minimalConfigJson());
            result = LoadResult::CreatedMinimal;
        }
        ValidationErrors syntax;
        auto file = parseOrdered(*text, syntax);
        if (!syntax.empty())
        {
            throw ConfigError(syntax);
        }
        EnvOverlay overlay = applyEnvironment(toPlain(file), _env);
        bool const generated = fillGeneratedIds(file, overlay);
        std::string raw = *text;
        if (generated || result == LoadResult::CreatedMinimal)
        {
            // Validate before writing: an invalid file must not be rewritten.
            auto parsed = validateFile(file);
            if (!parsed.ok())
            {
                throw ConfigError(parsed.errors);
            }
            raw = renderFile(file);
            util::atomicWrite(_path, raw, 0644);
            log::info(result == LoadResult::CreatedMinimal ? "config_created" : "config_ids_generated", {{"path", _path}});
        }
        rebuildSnapshot(std::move(file), std::move(raw));
        _knownMtime = util::mtimeNs(_path);
        return result;
    }

    Snapshot ConfigStore::snapshot() const
    {
        std::lock_guard const lock{_mutex};
        return _snapshot;
    }

    bool ConfigStore::changedOnDisk() const
    {
        std::lock_guard const lock{_mutex};
        auto const now = util::mtimeNs(_path);
        return now != _knownMtime;
    }

    void ConfigStore::acceptDiskVersion()
    {
        std::lock_guard const lock{_mutex};
        _knownMtime = util::mtimeNs(_path);
    }

    std::string ConfigStore::exportRaw() const
    {
        std::lock_guard const lock{_mutex};
        return _snapshot.raw;
    }

    UpdateResult ConfigStore::commit(ojson newFile)
    {
        // caller holds _mutex
        UpdateResult result;
        EnvOverlay overlay;
        auto parsed = validateFile(newFile, &overlay);
        if (!parsed.ok())
        {
            result.errors = parsed.errors;
            return result;
        }
        // Environment-bound settings cannot be changed through the API (§9.1).
        auto const oldPlain = toPlain(_snapshot.file);
        auto const newPlain = toPlain(newFile);
        for (auto const& b : overlay.bindings)
        {
            auto const ptr = nlohmann::json::json_pointer(b.pointer);
            auto const oldV = oldPlain.contains(ptr) ? oldPlain.at(ptr) : nlohmann::json();
            auto const newV = newPlain.contains(ptr) ? newPlain.at(ptr) : nlohmann::json();
            if (oldV != newV)
            {
                result.errors.push_back({b.pointer, "is set via environment variable " + b.variable + " and cannot be changed here"});
            }
        }
        if (!result.errors.empty())
        {
            return result;
        }
        auto raw = renderFile(newFile);
        if (!_snapshot.raw.empty())
        {
            util::atomicWrite(_path + ".bak", _snapshot.raw, 0644);
        }
        util::atomicWrite(_path, raw, 0644);
        _knownMtime = util::mtimeNs(_path);
        rebuildSnapshot(std::move(newFile), std::move(raw));
        return result;
    }

    UpdateResult ConfigStore::replace(ojson newFile, std::optional<std::string> const& ifMatch)
    {
        std::lock_guard const lock{_mutex};
        UpdateResult result;
        if (!ifMatch || *ifMatch != _snapshot.etag || util::mtimeNs(_path) != _knownMtime)
        {
            result.conflict = true;
            return result;
        }
        return commit(std::move(newFile));
    }

    UpdateResult ConfigStore::update(std::function<void(ojson&)> const& mutate, bool ignoreDiskChange)
    {
        std::lock_guard const lock{_mutex};
        UpdateResult result;
        if (!ignoreDiskChange && util::mtimeNs(_path) != _knownMtime)
        {
            result.conflict = true;
            return result;
        }
        auto file = _snapshot.file;
        mutate(file);
        return commit(std::move(file));
    }

    UpdateResult ConfigStore::importText(std::string const& text, bool keepIds)
    {
        std::lock_guard const lock{_mutex};
        UpdateResult result;
        if (util::mtimeNs(_path) != _knownMtime)
        {
            result.conflict = true;
            return result;
        }
        auto file = parseOrdered(text, result.errors);
        if (!result.errors.empty())
        {
            return result;
        }
        if (!keepIds)
        {
            regenerateIds(file);
        }
        EnvOverlay overlay = applyEnvironment(toPlain(file), _env);
        fillGeneratedIds(file, overlay);
        return commit(std::move(file));
    }

    bool ConfigStore::writeBack(std::string const& pointer, ojson value)
    {
        std::lock_guard const lock{_mutex};
        if (_snapshot.overlay.variableFor(pointer))
        {
            return false;
        }
        auto file = _snapshot.file;
        auto const ptr = ojson::json_pointer(pointer);
        if (file.contains(ptr) && file.at(ptr) == value)
        {
            return false;
        }
        file[ptr] = std::move(value);
        auto const result = commit(std::move(file));
        if (!result.ok())
        {
            log::warn("config_write_back_failed", {{"pointer", pointer}, {"details", formatErrors(result.errors)}});
            return false;
        }
        return true;
    }
}
