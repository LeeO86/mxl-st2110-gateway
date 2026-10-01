// SPDX-License-Identifier: MIT
#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "config/env.hpp"
#include "util/fs.hpp"
#include "util/uuid.hpp"

namespace testutil
{
    /// Temporary directory, preferring /dev/shm (tmpfs) so MXL-style checks work.
    class TempDir
    {
    public:
        explicit TempDir(bool preferTmpfs = true)
        {
            std::string base = "/tmp";
            auto const* custom = std::getenv("MXLGW_TEST_TMPFS"); // large tmpfs for MXL video flows (Docker build: RUN --mount=type=tmpfs)
            if (preferTmpfs && custom != nullptr && mxlgw::util::inspectFs(custom).tmpfs)
            {
                base = custom;
            }
            else if (preferTmpfs && mxlgw::util::inspectFs("/dev/shm").tmpfs)
            {
                base = "/dev/shm";
            }
            std::string tmpl = base + "/mxlgw-test-XXXXXX";
            char* dir = ::mkdtemp(tmpl.data());
            _path = dir != nullptr ? std::string(dir) : std::string();
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(_path, ec);
        }
        TempDir(TempDir const&) = delete;
        TempDir& operator=(TempDir const&) = delete;

        std::string const& path() const { return _path; }
        std::string file(std::string const& name) const { return _path + "/" + name; }
        bool tmpfs() const { return mxlgw::util::inspectFs(_path).tmpfs; }

    private:
        std::string _path;
    };

    inline void writeFile(std::string const& path, std::string const& content)
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    inline mxlgw::config::EnvLookup envFrom(std::map<std::string, std::string> vars)
    {
        return [vars](std::string const& name) -> std::optional<std::string>
        {
            auto const it = vars.find(name);
            if (it == vars.end())
            {
                return std::nullopt;
            }
            return it->second;
        };
    }

    inline std::string uid(int n)
    {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "00000000-0000-4000-8000-%012d", n);
        return buf;
    }

    /// A valid configuration with one ingest and one egress group (mock backend).
    inline nlohmann::json sampleConfig(std::string const& domainPath = "/Volumes/mxl/main")
    {
        auto j = nlohmann::json::parse(R"({
          "schema_version": 1,
          "node": {"id": "11111111-1111-4111-8111-111111111111", "label": "GW", "http_port": 18080, "registry": {"mode": "dns-sd"}},
          "nic": {"backend": "mock", "port_pairs": [{"name": "media",
                  "primary": {"name": "media-p", "ip": "10.1.1.21", "netmask": "255.255.255.0"},
                  "redundant": {"name": "media-r", "ip": "10.2.1.21", "netmask": "255.255.255.0"}}]},
          "ptp": {"mode": "external", "require_lock": false},
          "mxl": {"scan_path": null, "domains": [{"name": "main", "path": "DOMAIN"}]},
          "groups": [
            {"uid": "00000000-0000-4000-8000-000000000100", "label": "CAM 1", "direction": "ingest", "domain": "main", "redundancy": true,
             "video": [{"uid": "00000000-0000-4000-8000-000000000101", "label": "CAM 1 V", "width": 1920, "height": 1080, "rate": "50/1",
                        "defaults": {"legs": [{"multicast": "239.1.1.1", "port": 20000}, {"multicast": "239.2.1.1", "port": 20000}]}}],
             "audio": [{"uid": "00000000-0000-4000-8000-000000000102", "label": "CAM 1 A", "channels": 8, "bit_depth": 24, "ptime_us": 1000, "block_us": 1000,
                        "defaults": {"legs": [{"multicast": "239.1.1.2", "port": 20000}]}}],
             "anc": [{"uid": "00000000-0000-4000-8000-000000000103", "label": "CAM 1 ANC",
                        "defaults": {"legs": [{"multicast": "239.1.1.3", "port": 20000}]}}]},
            {"uid": "00000000-0000-4000-8000-000000000200", "label": "PGM", "direction": "egress", "domain": "main", "redundancy": false,
             "video": [{"uid": "00000000-0000-4000-8000-000000000201", "label": "PGM V", "width": 1920, "height": 1080, "rate": "50/1",
                        "defaults": {"legs": [{"multicast": "239.10.0.1", "port": 20000}]}}],
             "audio": [{"uid": "00000000-0000-4000-8000-000000000202", "label": "PGM A", "channels": 2, "bit_depth": 24,
                        "defaults": {"legs": [{"multicast": "239.10.0.2", "port": 20000}]}}]}
          ]
        })");
        j["mxl"]["domains"][0]["path"] = domainPath;
        return j;
    }
}
