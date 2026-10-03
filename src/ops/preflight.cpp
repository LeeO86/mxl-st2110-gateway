// SPDX-License-Identifier: MIT
#include "ops/preflight.hpp"

#include <algorithm>
#include <cctype>

#include <filesystem>
#include <set>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <unistd.h>

#include "mxlbridge/domaindef.hpp"
#include "mxlbridge/domainscan.hpp"
#include "util/cpuset.hpp"
#include "util/fs.hpp"
#include "util/strings.hpp"

namespace mxlgw::ops
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr int capIpcLock = 14;
        constexpr int capSysNice = 23;
        constexpr int capSysTime = 25;

        /// README anchor of a check: per-port/per-domain ids ("pci-MEDIA_P", "domain-MAIN") share their family's anchor.
        std::string anchorOf(std::string const& id)
        {
            auto const dash = id.rfind('-');
            if (dash != std::string::npos)
            {
                auto const suffix = id.substr(dash + 1);
                bool const upperSnake =
                    !suffix.empty() &&
                    std::all_of(suffix.begin(), suffix.end(), [](char ch)
                                { return std::isupper(static_cast<unsigned char>(ch)) || std::isdigit(static_cast<unsigned char>(ch)) || ch == '_'; });
                if (upperSnake)
                {
                    return "#preflight-" + id.substr(0, dash);
                }
            }
            return "#preflight-" + id;
        }

        void push(std::vector<CheckResult>& out, std::string id, CheckLevel level, std::string message)
        {
            auto anchor = anchorOf(id);
            out.push_back({std::move(id), level, std::move(message), std::move(anchor)});
        }

        std::string readTrim(std::string const& path)
        {
            auto const t = util::readFile(path);
            return t ? util::trim(*t) : std::string();
        }

        std::uint64_t meminfoKb(std::string const& procRoot, std::string const& key)
        {
            auto const text = util::readFile(procRoot + "/meminfo");
            if (!text)
            {
                return 0;
            }
            for (auto const& line : util::split(*text, '\n'))
            {
                if (util::startsWith(line, key + ":"))
                {
                    auto const parts = util::split(line.substr(key.size() + 1), ' ');
                    if (!parts.empty())
                    {
                        return static_cast<std::uint64_t>(util::parseInt(parts.front()).value_or(0));
                    }
                }
            }
            return 0;
        }

        std::uint64_t estimateHugepageBytes(config::Config const& c)
        {
            // Rough per-session estimate: 3 frame buffers per video session plus fixed mempools.
            std::uint64_t bytes = 512ull << 20; // MTL/DPDK base pools
            for (auto const& g : c.groups)
            {
                if (!g.enabled)
                {
                    continue;
                }
                for (auto const& v : g.video)
                {
                    bytes += 3ull * v.format.width * v.format.height * 5 / 2 + (64ull << 20);
                }
                bytes += (g.audio.size() + g.anc.size()) * (8ull << 20);
            }
            return bytes;
        }

        bool portInUse(int port)
        {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
            {
                return false;
            }
            int one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
            addr.sin_port = htons(static_cast<std::uint16_t>(port));
            bool const busy = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0;
            ::close(fd);
            return busy;
        }

        void checkPci(std::vector<CheckResult>& out, PreflightEnv const& env, config::NicPort const& port)
        {
            if (port.pci.empty() || util::startsWith(port.pci, "env:"))
            {
                return; // resolved at runtime from the device plugin variable
            }
            auto const id = "pci-" + util::toUpperSnake(port.name);
            auto const dev = env.sysRoot + "/bus/pci/devices/" + port.pci;
            std::error_code ec;
            if (!fs::exists(dev, ec))
            {
                push(out, id, CheckLevel::Fail, "PCI device " + port.pci + " (" + port.name + ") does not exist");
                return;
            }
            auto const driverLink = fs::read_symlink(dev + "/driver", ec);
            auto const driver = ec ? std::string() : driverLink.filename().string();
            if (driver != "vfio-pci")
            {
                push(out, id, CheckLevel::Fail,
                     "PCI device " + port.pci + " is bound to '" + (driver.empty() ? std::string("no driver") : driver) +
                         "', not vfio-pci (README: driverctl set-override " + port.pci + " vfio-pci)");
                return;
            }
            auto const group = fs::read_symlink(dev + "/iommu_group", ec);
            if (ec)
            {
                push(out, id, CheckLevel::Fail, "PCI device " + port.pci + " has no IOMMU group (enable VT-d / intel_iommu=on)");
                return;
            }
            auto const groupNode = env.devRoot + "/vfio/" + group.filename().string();
            if (!fs::exists(groupNode, ec))
            {
                push(out, id, CheckLevel::Fail, "VFIO group node " + groupNode + " is not mapped into the container");
                return;
            }
            auto const vendor = readTrim(dev + "/vendor");
            bool const vf = fs::exists(dev + "/physfn", ec);
            if (vendor != "0x8086")
            {
                push(out, id, CheckLevel::Warn, "PCI device " + port.pci + " is not an Intel device (vendor " + vendor + "); E810/E830 expected");
                return;
            }
            push(out, id, CheckLevel::Ok, "PCI device " + port.pci + " bound to vfio-pci (" + (vf ? "VF, degraded PTP" : "PF") + ")");
        }
    }

    char const* toName(CheckLevel level)
    {
        switch (level)
        {
            case CheckLevel::Ok: return "ok";
            case CheckLevel::Info: return "info";
            case CheckLevel::Warn: return "warn";
            case CheckLevel::Fail: return "fail";
        }
        return "ok";
    }

    int kernelTaiOffset()
    {
        struct timex tx
        {};
        ::adjtimex(&tx);
        return tx.tai;
    }

    unsigned long long effectiveCapabilities(std::string const& procRoot)
    {
        auto const text = util::readFile(procRoot + "/self/status");
        if (!text)
        {
            return 0;
        }
        for (auto const& line : util::split(*text, '\n'))
        {
            if (util::startsWith(line, "CapEff:"))
            {
                return std::stoull(util::trim(line.substr(7)), nullptr, 16);
            }
        }
        return 0;
    }

    std::vector<CheckResult> runPreflight(config::Config const& c, PreflightEnv const& env)
    {
        std::vector<CheckResult> out;
        bool const media = !c.unconfigured();
        bool const realNic = media && c.nic.backend != config::Backend::Mock;

        if (realNic)
        {
            std::error_code ec;
            auto const hugeFs = util::inspectFs(env.devRoot + "/hugepages");
            auto const pageKb = meminfoKb(env.procRoot, "Hugepagesize");
            auto const freePages = meminfoKb(env.procRoot, "HugePages_Free");
            auto const freeBytes = freePages * pageKb * 1024;
            auto const need = estimateHugepageBytes(c);
            if (!fs::exists(env.devRoot + "/hugepages", ec) || hugeFs.typeName != "0x958458f6")
            {
                if (hugeFs.typeName != "0x958458f6")
                {
                    push(out, "hugepages", fs::exists(env.devRoot + "/hugepages", ec) ? CheckLevel::Warn : CheckLevel::Fail,
                         env.devRoot + "/hugepages is not a hugetlbfs mount (" + hugeFs.typeName + ")");
                }
            }
            if (freeBytes < need)
            {
                // The estimate is sized for E810 sessions; the test-only kernel backend needs far less.
                push(out, "hugepages-free", c.nic.backend == config::Backend::Kernel ? CheckLevel::Warn : CheckLevel::Fail,
                     "free hugepages " + std::to_string(freeBytes >> 20) + " MiB < estimated " + std::to_string(need >> 20) +
                         " MiB for the configured sessions");
            }
            else
            {
                push(out, "hugepages-free", CheckLevel::Ok, std::to_string(freeBytes >> 20) + " MiB hugepages free");
            }
        }

        if (media && c.nic.backend == config::Backend::Dpdk)
        {
            std::error_code ec;
            if (!fs::exists(env.devRoot + "/vfio/vfio", ec))
            {
                push(out, "vfio", CheckLevel::Fail, env.devRoot + "/vfio/vfio is missing (map /dev/vfio into the container)");
            }
            for (auto const& pp : c.nic.portPairs)
            {
                checkPci(out, env, pp.primary);
                if (pp.redundant)
                {
                    checkPci(out, env, *pp.redundant);
                }
            }
            bool const ddp = fs::exists(env.firmwareRoot + "/updates/intel/ice/ddp/ice.pkg", ec) || fs::exists(env.firmwareRoot + "/intel/ice/ddp/ice.pkg", ec);
            push(out, "ddp", ddp ? CheckLevel::Ok : CheckLevel::Fail, ddp ? "E810 DDP package present" : "E810 DDP package (ice.pkg) missing in the container");
        }

        if (media && c.nic.backend == config::Backend::Kernel)
        {
            std::error_code ec;
            for (auto const& pp : c.nic.portPairs)
            {
                for (auto const* port : {&pp.primary, pp.redundant ? &*pp.redundant : nullptr})
                {
                    if (port == nullptr)
                    {
                        continue;
                    }
                    bool const exists = fs::exists(env.sysRoot + "/class/net/" + port->ifname, ec);
                    push(out, "ifname-" + util::toUpperSnake(port->name), exists ? CheckLevel::Ok : CheckLevel::Fail,
                         "kernel interface '" + port->ifname + "' " + (exists ? "exists" : "does not exist"));
                }
            }
            auto const rmem = util::parseInt(readTrim(env.procRoot + "/sys/net/core/rmem_max")).value_or(0);
            if (rmem < 4194304)
            {
                push(out, "rmem-max", CheckLevel::Warn, "net.core.rmem_max = " + std::to_string(rmem) + " < 4194304 (MTL kernel backend loses packets)");
            }
            push(out, "test-backend", CheckLevel::Warn, "nic.backend = kernel is test-only (no pacing, no HW PTP)");
        }
        if (media && c.nic.backend == config::Backend::Mock)
        {
            push(out, "test-backend", CheckLevel::Warn, "nic.backend = mock is test-only (no network I/O)");
        }

        // Capabilities.
        if (realNic)
        {
            auto const caps = effectiveCapabilities(env.procRoot);
            auto has = [&](int bit) { return (caps >> bit) & 1ull; };
            if (!has(capIpcLock))
            {
                push(out, "cap-ipc-lock", c.nic.backend == config::Backend::Dpdk ? CheckLevel::Fail : CheckLevel::Warn,
                     "CAP_IPC_LOCK missing (cap_add: IPC_LOCK)");
            }
            if (!has(capSysNice))
            {
                push(out, "cap-sys-nice", CheckLevel::Warn, "CAP_SYS_NICE missing: no NUMA policy / real-time priorities");
            }
            if (c.ptp.mode == config::PtpMode::BuiltinPhc2sys && !has(capSysTime))
            {
                push(out, "cap-sys-time", CheckLevel::Fail, "ptp.mode = builtin_phc2sys needs CAP_SYS_TIME");
            }
        }

        // MXL domains.
        for (auto const& d : c.mxl.domains)
        {
            auto const id = "domain-" + util::toUpperSnake(d.name);
            auto const info = util::inspectFs(d.path);
            if (mxlbridge::isMirrorBasename(d.path))
            {
                push(out, id, CheckLevel::Fail, "domain '" + d.name + "' path " + d.path + " is a mirror domain");
            }
            else if (!info.tmpfs || info.overlay)
            {
                push(out, id, CheckLevel::Fail, "domain '" + d.name + "' path " + d.path + " is not on tmpfs/ramfs (" + info.typeName + ")");
            }
            else
            {
                push(out, id, CheckLevel::Ok, "domain '" + d.name + "' on " + info.typeName + ", " + std::to_string(info.freeBytes >> 20) + " MiB free");
            }
        }
        if (c.mxl.scanPath)
        {
            std::error_code ec;
            if (!fs::is_directory(*c.mxl.scanPath, ec))
            {
                push(out, "scan-path", CheckLevel::Warn, "mxl.scan_path " + *c.mxl.scanPath + " does not exist: no domain discovery");
            }
            else
            {
                std::vector<mxlbridge::ConfiguredDomainRef> refs;
                for (auto const& d : c.mxl.domains)
                {
                    refs.push_back({d.name, d.path});
                }
                auto const scan = mxlbridge::scanDomains(c.mxl.scanPath, refs);
                std::size_t mirrors = 0;
                for (auto const& d : scan.domains)
                {
                    mirrors += d.kind == mxlbridge::DomainKind::Mirror ? 1 : 0;
                }
                push(out, "scan-path", scan.conflicts.empty() ? CheckLevel::Info : CheckLevel::Warn,
                     std::to_string(scan.domains.size()) + " domain(s) under " + *c.mxl.scanPath + " (" + std::to_string(mirrors) + " mirror), " +
                         std::to_string(scan.conflicts.size()) + " conflict(s), " + std::to_string(scan.skipped.size()) + " skipped");
            }
        }

        // Kernel TAI offset (§5.1, §5.2).
        if (media)
        {
            int const tai = kernelTaiOffset();
            if (c.ptp.mode == config::PtpMode::BuiltinPhc2sys)
            {
                push(out, "tai-offset", tai == 0 ? CheckLevel::Ok : CheckLevel::Fail,
                     tai == 0 ? std::string("kernel TAI offset 0 (builtin_phc2sys)")
                              : "ptp.mode = builtin_phc2sys requires kernel TAI offset 0, found " + std::to_string(tai) + " s");
            }
            else
            {
                push(out, "tai-offset", tai != 0 ? CheckLevel::Ok : CheckLevel::Warn,
                     tai != 0 ? "kernel TAI offset " + std::to_string(tai) + " s"
                              : std::string("kernel TAI offset is 0: CLOCK_TAI equals UTC; MXL grain indices will be off by the leap seconds and replicated "
                                            "flows misaligned between hosts (§5.1)"));
            }
        }

        // Ports (§15.4). A port in use is only a warning here: the listener itself then exits 75 (§14.4).
        static std::set<int> const siblings = {8095, 3212, 3213, 3232, 3233};
        std::vector<std::pair<std::string, int>> ports{{"node.http_port", c.node.httpPort}};
        if (c.node.effectiveWebPort() != c.node.httpPort)
        {
            ports.emplace_back("node.web_port", c.node.effectiveWebPort());
        }
        if (c.node.st2110.enabled)
        {
            ports.emplace_back("node.st2110.http_port", c.node.st2110HttpPort());
        }
        std::string inUse;
        for (auto const& [name, port] : ports)
        {
            if (env.checkPortInUse && portInUse(port))
            {
                inUse += (inUse.empty() ? "" : ", ") + name + " " + std::to_string(port);
            }
        }
        if (!inUse.empty())
        {
            push(out, "http-port", CheckLevel::Warn, inUse + " already in use: the gateway will exit 75");
        }
        else if (siblings.count(c.node.httpPort) != 0 || (c.node.httpPort >= 23500 && c.node.httpPort <= 23599))
        {
            push(out, "http-port", CheckLevel::Warn,
                 "node.http_port " + std::to_string(c.node.httpPort) + " is a default port of mxl-decklink / mxl-fabrics-agent");
        }
        else if (c.node.httpPort == 8080)
        {
            push(out, "http-port", CheckLevel::Info, "node.http_port 8080 is also mxl-decklink's default; remap when both share host networking");
        }

        // lcores.
        if (!c.nic.lcores.empty())
        {
            auto const lcores = util::parseCpuList(c.nic.lcores);
            auto const present = util::presentCpus();
            if (lcores && !present.empty())
            {
                for (auto const cpu : *lcores)
                {
                    if (present.count(cpu) == 0)
                    {
                        push(out, "lcores", CheckLevel::Fail, "nic.lcores contains CPU " + std::to_string(cpu) + " which does not exist");
                        break;
                    }
                }
            }
        }
        return out;
    }

    bool hasFailures(std::vector<CheckResult> const& results)
    {
        for (auto const& r : results)
        {
            if (r.level == CheckLevel::Fail)
            {
                return true;
            }
        }
        return false;
    }

    nlohmann::json toJson(std::vector<CheckResult> const& results)
    {
        auto arr = nlohmann::json::array();
        for (auto const& r : results)
        {
            arr.push_back({{"id", r.id}, {"level", toName(r.level)}, {"message", r.message}, {"anchor", r.anchor}});
        }
        return arr;
    }
}
