// SPDX-License-Identifier: MIT
#include "mxlbridge/bootstrap.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

#include "mxlbridge/domaindef.hpp"
#include "mxlbridge/domainscan.hpp"
#include "util/fs.hpp"
#include "util/logging.hpp"

namespace mxlgw::mxlbridge
{
    namespace
    {
        namespace fs = std::filesystem;

        void createDirectories(fs::path const& path)
        {
            // Create missing components one by one so each new directory gets mode 0775 (§14.2).
            std::vector<fs::path> missing;
            for (auto p = path; !p.empty() && !fs::exists(p); p = p.parent_path())
            {
                missing.push_back(p);
                if (p == p.parent_path())
                {
                    break;
                }
            }
            for (auto it = missing.rbegin(); it != missing.rend(); ++it)
            {
                std::error_code ec;
                fs::create_directory(*it, ec);
                if (ec && !fs::is_directory(*it))
                {
                    throw BootstrapError("mxl_domain_create_failed", "cannot create MXL domain directory " + it->string() + ": " + ec.message(),
                                         {{"path", it->string()}, {"error", ec.message()}});
                }
                ::chmod(it->c_str(), 0775);
            }
        }
    }

    BootstrapResult bootstrapDomain(config::Domain const& domain, BootstrapOptions const& options)
    {
        BootstrapResult result;
        result.name = domain.name;
        result.path = domain.path;

        // 1. Mount check: tmpfs/ramfs and not the container's root filesystem.
        auto const info = util::inspectFs(domain.path);
        result.fsType = info.typeName;
        auto const rootDevice = util::deviceOf(options.rootPath);
        bool const onRoot = rootDevice && info.deviceId == *rootDevice && options.rootPath != domain.path;
        if (!info.tmpfs || info.overlay || onRoot)
        {
            throw BootstrapError("mxl_domain_not_tmpfs",
                                 "MXL domain " + domain.path + " is not on tmpfs/ramfs (filesystem: " + info.typeName + (onRoot ? ", container root" : "") +
                                     ")",
                                 {{"domain", domain.name}, {"path", domain.path}, {"fs_type", info.typeName}, {"container_root", onRoot}});
        }

        // 2. Mirror check (basename or marker in an existing domain_def.json).
        if (isMirrorBasename(domain.path))
        {
            throw BootstrapError("mxl_domain_is_mirror", "configured MXL domain " + domain.path + " is a mirror domain (basename mirror-*)",
                                 {{"domain", domain.name}, {"path", domain.path}, {"reason", "basename"}});
        }
        auto const defPath = domainDefPath(domain.path);
        auto const existingText = util::readFile(defPath);
        std::optional<DomainDef> existing;
        if (existingText)
        {
            std::string error;
            existing = parseDomainDef(*existingText, error);
            if (!existing)
            {
                throw BootstrapError("mxl_domain_def_invalid", "existing " + defPath + " is invalid: " + error,
                                     {{"domain", domain.name}, {"path", defPath}, {"error", error}});
            }
            if (existing->mirror)
            {
                throw BootstrapError("mxl_domain_is_mirror", "configured MXL domain " + domain.path + " carries the x-mxl-fabrics-agent marker",
                                     {{"domain", domain.name}, {"path", domain.path}, {"reason", "marker"}});
            }
        }

        // 3. Directory.
        if (!fs::is_directory(domain.path))
        {
            createDirectories(domain.path);
            result.directoryCreated = true;
            log::info("mxl_domain_directory_created", {{"domain", domain.name}, {"path", domain.path}});
        }

        // 4. domain_def.json: adopt an existing one, never rewrite it.
        if (existing)
        {
            result.id = existing->id;
            result.label = existing->label;
            result.description = existing->description;
            if (domain.id && *domain.id != existing->id)
            {
                log::warn("domain_id_mismatch", {{"domain", domain.name},
                                                 {"path", domain.path},
                                                 {"config_id", domain.id->toString()},
                                                 {"domain_def_id", existing->id.toString()},
                                                 {"from_environment", options.idFromEnvironment}});
                result.warnings.emplace_back("domain_id_mismatch");
            }
            if (!options.idFromEnvironment && (!domain.id || *domain.id != existing->id))
            {
                result.writeBackId = existing->id;
            }
        }
        else
        {
            result.id = domain.id ? *domain.id : util::uuidV4();
            result.label = domain.label.empty() ? domain.name : domain.label;
            result.description = domain.description.empty() ? result.label : domain.description;
            try
            {
                util::atomicWrite(defPath, renderDomainDef(result.id, result.label, result.description), 0664);
            }
            catch (std::exception const& ex)
            {
                throw BootstrapError("mxl_domain_create_failed", "cannot write " + defPath + ": " + ex.what(),
                                     {{"domain", domain.name}, {"path", defPath}, {"error", ex.what()}});
            }
            result.domainDefCreated = true;
            if (!domain.id && !options.idFromEnvironment)
            {
                result.writeBackId = result.id;
            }
            log::info("mxl_domain_def_created", {{"domain", domain.name}, {"path", defPath}, {"id", result.id.toString()}});
        }

        // 5. options.json: only if missing and configured; never overwritten.
        if (domain.historyDurationNs)
        {
            auto const optPath = optionsPath(domain.path);
            if (auto const text = util::readFile(optPath))
            {
                std::int64_t present = -1;
                try
                {
                    auto const j = nlohmann::json::parse(*text);
                    if (j.contains(historyDurationOption) && j[historyDurationOption].is_number_integer())
                    {
                        present = j[historyDurationOption].get<std::int64_t>();
                    }
                }
                catch (std::exception const&)
                {}
                if (present != *domain.historyDurationNs)
                {
                    log::warn("mxl_domain_options_mismatch",
                              {{"domain", domain.name}, {"path", optPath}, {"configured_ns", *domain.historyDurationNs}, {"present_ns", present}});
                    result.warnings.emplace_back("mxl_domain_options_mismatch");
                }
            }
            else
            {
                nlohmann::ordered_json j;
                j[historyDurationOption] = *domain.historyDurationNs;
                util::atomicWrite(optPath, j.dump(4) + "\n", 0664);
                result.optionsCreated = true;
            }
        }
        return result;
    }

    bool flowInUse(std::string const& domainPath, util::Uuid const& flowId)
    {
        auto const data = fs::path(flowDirPath(domainPath, flowId)) / "data";
        int fd = ::open(data.c_str(), O_RDONLY | O_CLOEXEC | O_NOATIME);
        if (fd < 0)
        {
            fd = ::open(data.c_str(), O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0)
        {
            return false;
        }
        bool const active = ::flock(fd, LOCK_EX | LOCK_NB) < 0;
        ::close(fd);
        return active;
    }

    std::vector<util::Uuid> removeStaleFlows(std::string const& domainPath, std::vector<util::Uuid> const& flowIds)
    {
        std::vector<util::Uuid> removed;
        for (auto const& id : flowIds)
        {
            auto const dir = flowDirPath(domainPath, id);
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
            {
                continue;
            }
            if (!fs::exists(fs::path(dir) / "data") || flowInUse(domainPath, id))
            {
                continue;
            }
            fs::remove_all(dir, ec);
            if (!ec)
            {
                removed.push_back(id);
                log::info("mxl_flow_removed_stale", {{"path", dir}, {"flow_id", id.toString()}});
            }
            else
            {
                log::warn("mxl_flow_remove_failed", {{"path", dir}, {"error", ec.message()}});
            }
        }
        return removed;
    }
}
