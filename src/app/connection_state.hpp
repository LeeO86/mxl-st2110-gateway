// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "util/uuid.hpp"

namespace mxlgw::app
{
    /// Last IS-05 /active endpoint of every Sender/Receiver (§7.6), persisted atomically to
    /// `state/connections.json` next to the configuration. Not part of config import/export.
    class ConnectionState
    {
    public:
        explicit ConnectionState(std::string path);

        /// Loads the file; an unreadable or invalid file is ignored with a warning (start from defaults).
        void load();
        std::optional<nlohmann::json> active(util::Uuid const& resourceId) const;
        /// Stores and writes the file. Returns false if the write failed (logged).
        bool save(util::Uuid const& resourceId, std::string const& type, nlohmann::json const& active);
        void erase(util::Uuid const& resourceId);
        std::size_t size() const;
        std::string const& path() const { return _path; }

    private:
        bool writeLocked();

        std::string _path;
        mutable std::mutex _mutex;
        std::map<std::string, nlohmann::json> _entries; // id -> {type, active}
    };
}
