// SPDX-License-Identifier: MIT
#include "mxlbridge/domainscan.hpp"

#include <filesystem>
#include <map>

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace mxlgw::mxlbridge
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string normalise(std::string const& path)
        {
            std::error_code ec;
            auto const canonical = fs::weakly_canonical(fs::path(path), ec);
            auto out = (ec ? fs::path(path) : canonical).lexically_normal().string();
            while (out.size() > 1 && out.back() == '/')
            {
                out.pop_back();
            }
            return out;
        }

        std::optional<DomainEntry> inspect(std::string const& path, SkippedDomain& skipped)
        {
            auto const defText = util::readFile(domainDefPath(path));
            if (!defText)
            {
                skipped = {path, "no domain_def.json"};
                return std::nullopt;
            }
            auto const fsInfo = util::inspectFs(path);
            if (!fsInfo.tmpfs)
            {
                skipped = {path, "not on tmpfs/ramfs (" + fsInfo.typeName + ")"};
                return std::nullopt;
            }
            std::string error;
            auto const def = parseDomainDef(*defText, error);
            if (!def)
            {
                skipped = {path, "invalid domain_def.json: " + error};
                return std::nullopt;
            }
            DomainEntry e;
            e.path = path;
            e.id = def->id;
            e.label = def->label;
            e.description = def->description;
            e.kind = (def->mirror || isMirrorBasename(path)) ? DomainKind::Mirror : DomainKind::Discovered;
            e.sourceHostId = def->sourceHostId;
            e.ownerHostId = def->ownerHostId;
            e.fsType = fsInfo.typeName;
            e.tmpfs = fsInfo.tmpfs;
            e.totalBytes = fsInfo.totalBytes;
            e.freeBytes = fsInfo.freeBytes;
            e.flowCount = listFlowDirs(path, false).size();
            return e;
        }
    }

    char const* toName(DomainKind kind)
    {
        switch (kind)
        {
            case DomainKind::Configured: return "configured";
            case DomainKind::Discovered: return "discovered";
            case DomainKind::Mirror: return "mirror";
            case DomainKind::Conflict: return "conflict";
        }
        return "discovered";
    }

    DomainEntry const* ScanResult::findById(util::Uuid const& id) const
    {
        for (auto const& d : domains)
        {
            if (d.id == id)
            {
                return &d;
            }
        }
        return nullptr;
    }

    std::string flowDirPath(std::string const& domainPath, util::Uuid const& flowId)
    {
        return (fs::path(domainPath) / (flowId.toString() + ".mxl-flow")).string();
    }

    bool flowDirExists(std::string const& domainPath, util::Uuid const& flowId)
    {
        std::error_code ec;
        return fs::is_directory(flowDirPath(domainPath, flowId), ec);
    }

    std::vector<FlowDirEntry> listFlowDirs(std::string const& domainPath, bool readDefs)
    {
        std::vector<FlowDirEntry> out;
        std::error_code ec;
        for (fs::directory_iterator it(domainPath, ec), end; !ec && it != end; it.increment(ec))
        {
            auto const name = it->path().filename().string();
            if (!util::endsWith(name, ".mxl-flow") || !it->is_directory(ec))
            {
                continue;
            }
            auto const id = util::parseUuid(name.substr(0, name.size() - std::string(".mxl-flow").size()));
            if (!id)
            {
                continue;
            }
            FlowDirEntry entry;
            entry.id = *id;
            entry.path = it->path().string();
            if (readDefs)
            {
                if (auto const text = util::readFile((it->path() / "flow_def.json").string()))
                {
                    try
                    {
                        entry.flowDef = nlohmann::json::parse(*text);
                    }
                    catch (...)
                    {}
                }
            }
            out.push_back(std::move(entry));
        }
        return out;
    }

    ScanResult scanDomains(std::optional<std::string> const& scanPath, std::vector<ConfiguredDomainRef> const& configured)
    {
        ScanResult result;
        std::map<std::string, std::string> configuredByPath; // normalised path -> name
        for (auto const& c : configured)
        {
            configuredByPath[normalise(c.path)] = c.name;
        }

        std::vector<DomainEntry> candidates;
        std::map<std::string, bool> seenPaths;
        auto consider = [&](std::string const& path)
        {
            auto const norm = normalise(path);
            if (seenPaths.count(norm) != 0)
            {
                return;
            }
            seenPaths[norm] = true;
            SkippedDomain skipped;
            auto entry = inspect(norm, skipped);
            if (!entry)
            {
                // A configured domain without domain_def.json yet is not "skipped": bootstrap creates it.
                if (configuredByPath.count(norm) == 0 && skipped.reason != "no domain_def.json")
                {
                    result.skipped.push_back(skipped);
                }
                return;
            }
            if (auto const it = configuredByPath.find(norm); it != configuredByPath.end())
            {
                entry->kind = DomainKind::Configured;
                entry->configuredName = it->second;
            }
            candidates.push_back(std::move(*entry));
        };

        for (auto const& c : configured)
        {
            consider(c.path);
        }
        if (scanPath)
        {
            std::error_code ec;
            if (fs::is_directory(*scanPath, ec))
            {
                consider(*scanPath);
                for (fs::directory_iterator it(*scanPath, ec), end; !ec && it != end; it.increment(ec))
                {
                    if (it->is_directory(ec))
                    {
                        consider(it->path().string());
                    }
                }
            }
        }

        // Duplicate ids: a configured domain wins; otherwise every holder is a conflict (§8.5).
        std::map<util::Uuid, std::vector<std::size_t>> byId;
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            byId[candidates[i].id].push_back(i);
        }
        for (auto const& [id, indexes] : byId)
        {
            if (indexes.size() == 1)
            {
                result.domains.push_back(candidates[indexes.front()]);
                continue;
            }
            std::optional<std::size_t> winner;
            for (auto const i : indexes)
            {
                if (candidates[i].kind == DomainKind::Configured)
                {
                    winner = i;
                    break;
                }
            }
            for (auto const i : indexes)
            {
                if (winner && i == *winner)
                {
                    result.domains.push_back(candidates[i]);
                }
                else
                {
                    auto c = candidates[i];
                    c.kind = DomainKind::Conflict;
                    result.conflicts.push_back(c);
                }
            }
        }
        return result;
    }

    DomainDirectory::DomainDirectory(std::optional<std::string> scanPath, std::vector<ConfiguredDomainRef> configured)
        : _scanPath(std::move(scanPath))
        , _configured(std::move(configured))
    {}

    ScanResult DomainDirectory::rescan()
    {
        auto result = scanDomains(_scanPath, _configured);
        std::lock_guard const lock{_mutex};
        _last = result;
        return result;
    }

    ScanResult DomainDirectory::last() const
    {
        std::lock_guard const lock{_mutex};
        return _last;
    }

    std::optional<DomainEntry> DomainDirectory::findById(util::Uuid const& id)
    {
        {
            std::lock_guard const lock{_mutex};
            if (auto const* d = _last.findById(id))
            {
                std::error_code ec;
                if (fs::exists(domainDefPath(d->path), ec))
                {
                    return *d;
                }
            }
        }
        auto const fresh = rescan();
        if (auto const* d = fresh.findById(id))
        {
            return *d;
        }
        return std::nullopt;
    }

    std::optional<DomainEntry> DomainDirectory::findFlow(util::Uuid const& flowId, std::optional<util::Uuid> const& preferred)
    {
        auto const fresh = rescan();
        if (preferred)
        {
            if (auto const* d = fresh.findById(*preferred); d && flowDirExists(d->path, flowId))
            {
                return *d;
            }
        }
        // Prefer configured, then discovered, then mirror domains.
        for (auto const kind : {DomainKind::Configured, DomainKind::Discovered, DomainKind::Mirror})
        {
            for (auto const& d : fresh.domains)
            {
                if (d.kind == kind && flowDirExists(d.path, flowId))
                {
                    return d;
                }
            }
        }
        return std::nullopt;
    }
}
