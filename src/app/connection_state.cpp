// SPDX-License-Identifier: MIT
#include "app/connection_state.hpp"

#include <filesystem>

#include "util/fs.hpp"
#include "util/logging.hpp"

namespace mxlgw::app
{
    ConnectionState::ConnectionState(std::string path)
        : _path(std::move(path))
    {}

    void ConnectionState::load()
    {
        std::lock_guard const lock{_mutex};
        _entries.clear();
        auto const text = util::readFile(_path);
        if (!text)
        {
            return;
        }
        try
        {
            auto const j = nlohmann::json::parse(*text);
            if (j.contains("resources") && j["resources"].is_object())
            {
                for (auto const& [id, entry] : j["resources"].items())
                {
                    if (util::isUuid(id) && entry.is_object() && entry.contains("active"))
                    {
                        _entries[id] = entry;
                    }
                }
            }
            log::info("connection_state_loaded", {{"path", _path}, {"resources", _entries.size()}});
        }
        catch (std::exception const& ex)
        {
            log::warn("connection_state_invalid", {{"path", _path}, {"error", ex.what()}});
            _entries.clear();
        }
    }

    std::optional<nlohmann::json> ConnectionState::active(util::Uuid const& resourceId) const
    {
        std::lock_guard const lock{_mutex};
        auto const it = _entries.find(resourceId.toString());
        if (it == _entries.end())
        {
            return std::nullopt;
        }
        return it->second.at("active");
    }

    bool ConnectionState::save(util::Uuid const& resourceId, std::string const& type, nlohmann::json const& active)
    {
        std::lock_guard const lock{_mutex};
        _entries[resourceId.toString()] = {{"type", type}, {"active", active}};
        return writeLocked();
    }

    void ConnectionState::erase(util::Uuid const& resourceId)
    {
        std::lock_guard const lock{_mutex};
        if (_entries.erase(resourceId.toString()) > 0)
        {
            writeLocked();
        }
    }

    std::size_t ConnectionState::size() const
    {
        std::lock_guard const lock{_mutex};
        return _entries.size();
    }

    bool ConnectionState::writeLocked()
    {
        nlohmann::ordered_json j;
        j["version"] = 1;
        j["resources"] = nlohmann::ordered_json::object();
        for (auto const& [id, entry] : _entries)
        {
            j["resources"][id] = entry;
        }
        try
        {
            std::filesystem::create_directories(std::filesystem::path(_path).parent_path());
            util::atomicWrite(_path, j.dump(2) + "\n", 0644);
            return true;
        }
        catch (std::exception const& ex)
        {
            log::error("connection_state_write_failed", {{"path", _path}, {"error", ex.what()}});
            return false;
        }
    }
}
