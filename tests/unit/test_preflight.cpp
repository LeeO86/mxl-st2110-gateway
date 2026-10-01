// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <filesystem>

#include "helpers.hpp"
#include "ops/preflight.hpp"

using namespace mxlgw;

namespace
{
    ops::CheckResult const* find(std::vector<ops::CheckResult> const& r, std::string const& id)
    {
        for (auto const& c : r)
        {
            if (c.id == id)
            {
                return &c;
            }
        }
        return nullptr;
    }

    config::Config parse(nlohmann::json const& j)
    {
        auto r = config::parseAndValidate(j);
        INFO(config::formatErrors(r.errors));
        REQUIRE(r.ok());
        return *r.config;
    }
}

TEST_CASE("preflight: dpdk backend against a fake sysfs")
{
    testutil::TempDir root(false);
    auto const sys = root.file("sys");
    auto const dev = root.file("dev");
    auto const proc = root.file("proc");
    // PCI device bound to vfio-pci with an IOMMU group.
    auto const pci = sys + "/bus/pci/devices/0000:31:00.0";
    std::filesystem::create_directories(pci);
    std::filesystem::create_directories(sys + "/bus/pci/drivers/vfio-pci");
    std::filesystem::create_directories(sys + "/kernel/iommu_groups/42");
    std::filesystem::create_symlink(sys + "/bus/pci/drivers/vfio-pci", pci + "/driver");
    std::filesystem::create_symlink(sys + "/kernel/iommu_groups/42", pci + "/iommu_group");
    testutil::writeFile(pci + "/vendor", "0x8086\n");
    testutil::writeFile(dev + "/vfio/vfio", "");
    testutil::writeFile(dev + "/vfio/42", "");
    testutil::writeFile(proc + "/meminfo", "Hugepagesize:    2048 kB\nHugePages_Free:     4096\n");
    testutil::writeFile(proc + "/self/status", "CapEff:\t0000000000804000\n"); // IPC_LOCK + SYS_NICE

    auto j = testutil::sampleConfig(root.file("mxl/main"));
    j["nic"]["backend"] = "dpdk";
    j["nic"]["port_pairs"][0]["primary"]["pci"] = "0000:31:00.0";
    j["nic"]["port_pairs"][0]["redundant"]["pci"] = "0000:31:00.7"; // missing
    j["nic"]["lcores"] = "4000";
    auto const c = parse(j);
    ops::PreflightEnv env;
    env.sysRoot = sys;
    env.devRoot = dev;
    env.procRoot = proc;
    env.firmwareRoot = root.file("fw");
    env.checkPortInUse = false;
    auto const r = ops::runPreflight(c, env);
    CHECK(find(r, "pci-MEDIA_P")->level == ops::CheckLevel::Ok);
    CHECK(find(r, "pci-MEDIA_R")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "ddp")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "hugepages-free")->level == ops::CheckLevel::Ok);
    CHECK(find(r, "cap-ipc-lock") == nullptr);
    CHECK(find(r, "domain-MAIN")->level == ops::CheckLevel::Fail); // not on tmpfs
    CHECK(find(r, "lcores")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "tai-offset") != nullptr);
    CHECK(ops::hasFailures(r));
    CHECK(ops::toJson(r).is_array());
    CHECK(find(r, "pci-MEDIA_P")->anchor == "#preflight-pci-MEDIA_P");
}

TEST_CASE("preflight: wrong driver, no IOMMU, missing caps, phc2sys")
{
    testutil::TempDir root(false);
    auto const sys = root.file("sys");
    auto const pci = sys + "/bus/pci/devices/0000:31:00.0";
    std::filesystem::create_directories(pci);
    std::filesystem::create_directories(sys + "/bus/pci/drivers/ice");
    std::filesystem::create_symlink(sys + "/bus/pci/drivers/ice", pci + "/driver");
    testutil::writeFile(root.file("proc/self/status"), "CapEff:\t0000000000000000\n");
    testutil::writeFile(root.file("proc/meminfo"), "Hugepagesize: 2048 kB\nHugePages_Free: 1\n");
    auto j = testutil::sampleConfig(root.file("mxl/main"));
    j["nic"]["backend"] = "dpdk";
    j["nic"]["port_pairs"][0]["primary"]["pci"] = "0000:31:00.0";
    j["nic"]["port_pairs"][0]["redundant"]["pci"] = "env:PCIDEVICE_X";
    j["ptp"]["mode"] = "builtin_phc2sys";
    auto const c = parse(j);
    ops::PreflightEnv env;
    env.sysRoot = sys;
    env.devRoot = root.file("dev");
    env.procRoot = root.file("proc");
    env.checkPortInUse = false;
    auto const r = ops::runPreflight(c, env);
    CHECK(find(r, "pci-MEDIA_P")->message.find("'ice'") != std::string::npos);
    CHECK(find(r, "vfio")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "cap-ipc-lock")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "cap-sys-time")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "hugepages-free")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "pci-MEDIA_R") == nullptr); // env: resolved at runtime
}

TEST_CASE("preflight: kernel and mock backends, ports, domains on tmpfs")
{
    testutil::TempDir shm;
    testutil::TempDir root(false);
    testutil::writeFile(root.file("proc/sys/net/core/rmem_max"), "212992\n");
    std::filesystem::create_directories(root.file("sys/class/net/veth0"));
    auto j = testutil::sampleConfig(shm.file("main"));
    j["nic"]["backend"] = "kernel";
    j["nic"]["port_pairs"][0]["primary"]["ifname"] = "veth0";
    j["nic"]["port_pairs"][0]["redundant"]["ifname"] = "veth9";
    j["node"]["http_port"] = 8095;
    j["mxl"]["scan_path"] = shm.path();
    auto const c = parse(j);
    ops::PreflightEnv env;
    env.sysRoot = root.file("sys");
    env.procRoot = root.file("proc");
    env.devRoot = root.file("dev");
    env.checkPortInUse = false;
    auto const r = ops::runPreflight(c, env);
    CHECK(find(r, "ifname-MEDIA_P")->level == ops::CheckLevel::Ok);
    CHECK(find(r, "ifname-MEDIA_R")->level == ops::CheckLevel::Fail);
    CHECK(find(r, "rmem-max")->level == ops::CheckLevel::Warn);
    CHECK(find(r, "test-backend")->level == ops::CheckLevel::Warn);
    CHECK(find(r, "http-port")->level == ops::CheckLevel::Warn);
    CHECK(find(r, "scan-path") != nullptr);
    if (shm.tmpfs())
    {
        CHECK(find(r, "domain-MAIN")->level == ops::CheckLevel::Ok);
    }

    auto m = testutil::sampleConfig("/Volumes/mxl/mirror-x/");
    m["mxl"]["domains"][0]["path"] = shm.file("mirror-x");
    auto mr = config::parseAndValidate(m);
    // (semantic validation already rejects mirror paths; preflight reports them too)
    CHECK_FALSE(mr.ok());

    auto mock = parse(testutil::sampleConfig(shm.file("main")));
    mock.node.httpPort = 8080;
    auto const mrr = ops::runPreflight(mock, env);
    CHECK(find(mrr, "test-backend")->message.find("mock") != std::string::npos);
    CHECK(find(mrr, "http-port")->level == ops::CheckLevel::Info);
    CHECK(std::string(ops::toName(ops::CheckLevel::Info)) == "info");
    CHECK(ops::effectiveCapabilities("/nonexistent") == 0);
}
