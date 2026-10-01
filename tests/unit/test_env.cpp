// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "config/config.hpp"
#include "config/env.hpp"
#include "helpers.hpp"

using namespace mxlgw;
using nlohmann::json;

TEST_CASE("environment overrides file, file overrides default")
{
    auto const file = testutil::sampleConfig();
    auto const env = testutil::envFrom({
        {"MXLGW_HTTP_PORT", "9000"}, // alias
        {"MXLGW_NODE_LABEL", "FROM-ENV"},
        {"MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl"}, // alias
        {"MXL_READ_OFFSET_MS", "40"},
        {"MXLGW_PTP_REQUIRE_LOCK", "yes"},
        {"MXLGW_NODE_MANAGEMENT_ADDRESSES", "10.0.0.1, 10.0.0.2"},
        {"MXLGW_NIC_PRIMARY_IP", "10.1.1.99"},
        {"MXLGW_MXL_DOMAIN_MAIN_LABEL", "Env domain"},
    });
    auto const overlay = config::applyEnvironment(file, env);
    CHECK(overlay.errors.empty());
    CHECK(overlay.effective["node"]["http_port"] == 9000);
    CHECK(overlay.effective["node"]["label"] == "FROM-ENV");
    CHECK(overlay.effective["mxl"]["scan_path"] == "/Volumes/mxl");
    CHECK(overlay.effective["mxl"]["default_read_offset_ns"] == 40'000'000);
    CHECK(overlay.effective["ptp"]["require_lock"] == true);
    CHECK(overlay.effective["node"]["management_addresses"] == json::array({"10.0.0.1", "10.0.0.2"}));
    CHECK(overlay.effective["nic"]["port_pairs"][0]["primary"]["ip"] == "10.1.1.99");
    CHECK(overlay.effective["nic"]["port_pairs"][0]["primary"]["name"] == "media-p"); // untouched
    CHECK(overlay.effective["mxl"]["domains"][0]["label"] == "Env domain");
    CHECK(overlay.variableFor("/node/http_port") == std::string("MXLGW_HTTP_PORT"));
    CHECK_FALSE(overlay.variableFor("/node/description"));

    CHECK(config::provenanceOf("/node/http_port", file, overlay) == "env:MXLGW_HTTP_PORT");
    CHECK(config::provenanceOf("/node/id", file, overlay) == "file");
    CHECK(config::provenanceOf("/node/description", file, overlay) == "default");
    auto const map = config::provenanceMap(file, overlay);
    CHECK(map.at("/mxl/domains/0/label") == "env:MXLGW_MXL_DOMAIN_MAIN_LABEL");
    CHECK(map.at("/mxl/domains/0/path") == "file");

    auto const parsed = config::parseAndValidate(overlay.effective);
    INFO(config::formatErrors(parsed.errors));
    REQUIRE(parsed.ok());
    CHECK(parsed.config->mxl.defaultReadOffset.ns == 40'000'000);
}

TEST_CASE("canonical variable wins over alias")
{
    auto const overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_HTTP_PORT", "1"}, {"MXLGW_NODE_HTTP_PORT", "2"}}));
    CHECK(overlay.effective["node"]["http_port"] == 2);
    CHECK(overlay.variableFor("/node/http_port") == std::string("MXLGW_NODE_HTTP_PORT"));
}

TEST_CASE("invalid environment values are configuration errors")
{
    auto const overlay =
        config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_NODE_HTTP_PORT", "eighty"}, {"MXLGW_PTP_REQUIRE_LOCK", "maybe"}}));
    REQUIRE(overlay.errors.size() == 2);
    CHECK(overlay.errors[0].message.find("MXLGW_NODE_HTTP_PORT") != std::string::npos);
}

TEST_CASE("hugepage socket accepts auto or an integer")
{
    auto overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_NIC_HUGEPAGE_SOCKET", "1"}}));
    CHECK(overlay.effective["nic"]["hugepage_socket"] == 1);
    CHECK(config::parseAndValidate(overlay.effective).config->nic.hugepageSocket == 1);
    overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_NIC_HUGEPAGE_SOCKET", "auto"}}));
    CHECK(overlay.effective["nic"]["hugepage_socket"] == "auto");
    overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_NIC_HUGEPAGE_SOCKET", "near"}}));
    REQUIRE(overlay.errors.size() == 1);
    CHECK(overlay.errors[0].pointer == "/nic/hugepage_socket");
}

TEST_CASE("environment can create the port pair (Kubernetes)")
{
    auto file = testutil::sampleConfig();
    file["nic"].erase("port_pairs");
    file["groups"] = json::array();
    auto const overlay = config::applyEnvironment(file, testutil::envFrom({
                                                            {"MXLGW_NIC_BACKEND", "dpdk"},
                                                            {"MXLGW_NIC_PRIMARY_PCI", "env:PCIDEVICE_INTEL_COM_E810_MEDIA_P"},
                                                            {"MXLGW_NIC_PRIMARY_IP", "10.1.1.21"},
                                                            {"MXLGW_NIC_PRIMARY_NETMASK", "255.255.255.0"},
                                                            {"MXLGW_NIC_REDUNDANT_PCI", "0000:31:00.1"},
                                                            {"MXLGW_NIC_REDUNDANT_IP", "10.2.1.21"},
                                                            {"MXLGW_NIC_REDUNDANT_NETMASK", "255.255.255.0"},
                                                        }));
    auto const& pair = overlay.effective["nic"]["port_pairs"][0];
    CHECK(pair["primary"]["name"] == "media-p");
    CHECK(pair["redundant"]["name"] == "media-r");
    auto const parsed = config::parseAndValidate(overlay.effective);
    INFO(config::formatErrors(parsed.errors));
    CHECK(parsed.ok());
}

TEST_CASE("documented variables")
{
    auto const docs = config::documentedVariables();
    bool found = false;
    for (auto const& d : docs)
    {
        found = found || (d.variable == "MXLGW_MXL_SCAN_PATH" && !d.aliases.empty() && d.aliases.front() == "MXL_DOMAIN_SCAN_PATH");
    }
    CHECK(found);
    CHECK(config::processEnvironment()("PATH").has_value());
    CHECK_FALSE(config::processEnvironment()("MXLGW_SURELY_NOT_SET_12345").has_value());
}
