// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "config/config.hpp"
#include "config/env.hpp"

namespace mxlgw::config
{
    /// Thrown by ConfigStore::load() for an invalid file (exit 78, every error printed, §9.2).
    class ConfigError : public std::runtime_error
    {
    public:
        explicit ConfigError(ValidationErrors errors);
        ValidationErrors const& errors() const { return _errors; }

    private:
        ValidationErrors _errors;
    };

    struct Snapshot
    {
        nlohmann::ordered_json file; // file content as stored
        std::string raw;             // file bytes
        std::string etag;            // quoted strong ETag of `raw`
        EnvOverlay overlay;          // environment applied on top of the file
        Config config;               // effective, typed configuration
        std::map<std::string, std::string> provenance;
    };

    struct UpdateResult
    {
        ValidationErrors errors;
        bool conflict = false; // If-Match mismatch or file changed on disk
        bool ok() const { return errors.empty() && !conflict; }
    };

    /// Owns the configuration file (§9): atomic writes with `.bak`, ETag, changed-on-disk detection,
    /// environment overlay and write-back of generated ids.
    class ConfigStore
    {
    public:
        ConfigStore(std::string path, EnvLookup env);

        enum class LoadResult
        {
            Loaded,
            CreatedMinimal,
        };

        /// Loads the file, creating the minimal configuration if it does not exist. Generated ids
        /// (node.id, missing uids) are written back. Throws ConfigError if invalid.
        LoadResult load();

        Snapshot snapshot() const;
        std::string const& path() const { return _path; }

        /// Full replace (PUT /api/config). `ifMatch` must equal the current ETag when given.
        UpdateResult replace(nlohmann::ordered_json newFile, std::optional<std::string> const& ifMatch);

        /// Read-modify-write of the file JSON (group create/edit/delete, id write-back).
        UpdateResult update(std::function<void(nlohmann::ordered_json&)> const& mutate, bool ignoreDiskChange = false);

        /// Validates an import; on success writes it (never applied live, §9.4).
        UpdateResult importText(std::string const& text, bool keepIds);

        /// Writes a generated value back unless the pointer is bound to the environment (§9.1).
        bool writeBack(std::string const& pointer, nlohmann::ordered_json value);

        /// True if the file was modified by someone else since we last read or wrote it (§9.2).
        bool changedOnDisk() const;

        /// Re-reads the file from disk (operator chose "reload").
        void acceptDiskVersion();

        std::string exportRaw() const;

        /// Validates `fileJson` as it would be after the environment overlay.
        ParseResult validateFile(nlohmann::ordered_json const& fileJson, EnvOverlay* overlayOut = nullptr) const;

    private:
        UpdateResult commit(nlohmann::ordered_json newFile);
        void rebuildSnapshot(nlohmann::ordered_json file, std::string raw);
        static std::string computeEtag(std::string const& raw);

        std::string _path;
        EnvLookup _env;
        mutable std::mutex _mutex;
        Snapshot _snapshot;
        std::optional<std::int64_t> _knownMtime;
    };

    /// Assigns new uids to groups/essences and clears node/domain ids (import with keep_ids=false).
    void regenerateIds(nlohmann::ordered_json& file);

    /// Ensures node.id and every group/essence uid exist; returns true if something was generated.
    bool fillGeneratedIds(nlohmann::ordered_json& file, EnvOverlay const& overlay);

    /// Serialises the file the way the gateway writes it (2-space indentation, trailing newline).
    std::string renderFile(nlohmann::ordered_json const& file);
}
