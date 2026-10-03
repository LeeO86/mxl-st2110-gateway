// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "config/config.hpp"
#include "config/env.hpp"
#include "helpers.hpp"
#include "util/fs.hpp"

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
    CHECK(overlay.variableFor("/mxl/scan_path") == std::string("MXL_DOMAIN_SCAN_PATH"));
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

TEST_CASE("aliases: equal values are accepted, different values are a configuration error")
{
    auto overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_HTTP_PORT", "9001"}, {"NMOS_PORT", "9001"}}));
    CHECK(overlay.errors.empty());
    CHECK(overlay.effective["node"]["http_port"] == 9001);
    CHECK(overlay.variableFor("/node/http_port") == std::string("NMOS_PORT"));

    overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXLGW_HTTP_PORT", "1"}, {"MXLGW_NODE_HTTP_PORT", "2"}}));
    REQUIRE(overlay.errors.size() == 1);
    CHECK(overlay.errors[0].pointer == "/node/http_port");
    CHECK(overlay.errors[0].message.find("MXLGW_NODE_HTTP_PORT") != std::string::npos);
    CHECK(overlay.errors[0].message.find("MXLGW_HTTP_PORT") != std::string::npos);
    CHECK(overlay.effective["node"]["http_port"] == 18080); // nothing applied
    CHECK_FALSE(overlay.variableFor("/node/http_port"));

    // Different units, same value.
    overlay =
        config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"MXL_READ_OFFSET_MS", "40"}, {"MXLGW_MXL_DEFAULT_READ_OFFSET_NS", "40000000"}}));
    CHECK(overlay.errors.empty());
    CHECK(overlay.effective["mxl"]["default_read_offset_ns"] == 40'000'000);
}

TEST_CASE("platform standard variables (G1-G8)")
{
    auto file = testutil::sampleConfig();
    auto const env = testutil::envFrom({
        {"NMOS_SEED", "prod1-gw"},
        {"NMOS_LABEL", "PROD1 GW"},
        {"NMOS_TAGS", R"({"urn:x-platform:production": ["prod1"], "urn:x-platform:function": ["gw"]})"},
        {"NMOS_PORT", "3212"},
        {"WEB_PORT", "8085"},
        {"NMOS_HOST_ADDRESS", "10.0.0.7"},
        {"NMOS_REGISTRY_ADDRESS", "10.0.0.2"},
        {"NMOS_REGISTRY_PORT", "4000"},
        {"NMOS_DNS_SD", "false"},
        {"SHUTDOWN_TIMEOUT_S", "20"},
        {"MXL_CLEANUP_ON_EXIT", "true"},
        {"MXL_DOMAIN_SCAN_PATH", "/Volumes/mxl"},
        {"MXL_OUTPUT_DOMAIN_DIR", "/Volumes/mxl/prod1-gw"},
        {"MXL_OUTPUT_DOMAIN_ID", "a1a1a1a1-0000-4000-8000-00000000a001"},
        {"MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS", "500000000"},
    });
    auto const overlay = config::applyEnvironment(file, env);
    INFO(config::formatErrors(overlay.errors));
    REQUIRE(overlay.errors.empty());
    auto const parsed = config::parseAndValidate(overlay.effective);
    INFO(config::formatErrors(parsed.errors));
    REQUIRE(parsed.ok());
    auto const& c = *parsed.config;
    CHECK(c.node.seed == std::string("prod1-gw"));
    CHECK(c.node.label == "PROD1 GW");
    CHECK(c.node.tags.at("urn:x-platform:production") == std::vector<std::string>{"prod1"});
    CHECK(c.node.httpPort == 3212);
    CHECK(c.node.effectiveWebPort() == 8085);
    CHECK(c.node.st2110HttpPort() == 3213);
    CHECK(c.node.hostAddress == std::string("10.0.0.7"));
    CHECK_FALSE(c.node.registry.dnsSd);
    CHECK(c.node.registry.address == "10.0.0.2");
    CHECK(c.node.registry.port == 4000);
    CHECK(c.node.registry.effectiveQueryAddress() == "10.0.0.2");
    CHECK(c.node.registry.effectiveQueryPort() == 4001);
    CHECK(c.node.registry.configured());
    CHECK_FALSE(c.node.st2110.registry.configured());
    CHECK(c.node.shutdownTimeoutS == 20);
    CHECK(c.mxl.cleanupOnExit);
    CHECK(c.mxl.domains.at(0).name == "main");
    CHECK(c.mxl.domains.at(0).path == "/Volumes/mxl/prod1-gw");
    CHECK(c.mxl.domains.at(0).id->toString() == "a1a1a1a1-0000-4000-8000-00000000a001");
    CHECK(c.mxl.domains.at(0).historyDurationNs == 500'000'000);
    CHECK(overlay.variableFor("/mxl/domains/0/path") == std::string("MXL_OUTPUT_DOMAIN_DIR"));
    CHECK(config::provenanceMap(file, overlay).at("/node/http_port") == "env:NMOS_PORT");
}

TEST_CASE("legacy names stay valid as aliases")
{
    auto const overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({
                                                                                {"MXLGW_NODE_LABEL", "OLD"},
                                                                                {"MXLGW_NODE_PUBLIC_ADDRESS", "10.0.0.9"},
                                                                                {"MXLGW_MXL_SCAN_PATH", "/mnt/mxl"},
                                                                                {"MXLGW_NODE_REGISTRY_ADDRESS", "registry.mxl.svc"},
                                                                                {"MXLGW_NODE_REGISTRY_PORT", "8235"},
                                                                            }));
    REQUIRE(overlay.errors.empty());
    auto const parsed = config::parseAndValidate(overlay.effective);
    INFO(config::formatErrors(parsed.errors));
    REQUIRE(parsed.ok());
    CHECK(parsed.config->node.label == "OLD");
    CHECK(parsed.config->node.hostAddress == std::string("10.0.0.9"));
    CHECK(parsed.config->mxl.scanPath == std::string("/mnt/mxl"));
    CHECK(parsed.config->node.registry.address == "registry.mxl.svc");
    CHECK(parsed.config->node.registry.port == 8235);
    CHECK(overlay.variableFor("/node/host_address") == std::string("MXLGW_NODE_PUBLIC_ADDRESS"));
}

TEST_CASE("unknown variables are ignored")
{
    auto const overlay =
        config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"NMOS_UNKNOWN", "x"}, {"MXLGW_NOT_A_SETTING", "y"}, {"WEB", "z"}}));
    CHECK(overlay.errors.empty());
    CHECK(overlay.bindings.empty());
}

TEST_CASE("NMOS_TAGS must be a JSON object of string arrays")
{
    auto overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"NMOS_TAGS", "{broken"}}));
    REQUIRE(overlay.errors.size() == 1);
    CHECK(overlay.errors[0].message.find("NMOS_TAGS") != std::string::npos);
    overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"NMOS_TAGS", "[1, 2]"}}));
    REQUIRE(overlay.errors.size() == 1);
    overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"NMOS_TAGS", R"({"a": "not-an-array"})"}}));
    REQUIRE(overlay.errors.empty());
    CHECK_FALSE(config::parseAndValidate(overlay.effective).ok());
    overlay = config::applyEnvironment(testutil::sampleConfig(), testutil::envFrom({{"NMOS_TAGS", ""}}));
    REQUIRE(overlay.errors.empty());
    CHECK(config::parseAndValidate(overlay.effective).config->node.tags.empty());
}

TEST_CASE("output domain variables")
{
    SUBCASE("file without domains: the output domain is created as main")
    {
        auto file = testutil::sampleConfig();
        file["mxl"].erase("domains");
        file["groups"] = json::array();
        auto const overlay = config::applyEnvironment(file, testutil::envFrom({{"MXL_OUTPUT_DOMAIN_DIR", "/Volumes/mxl/gw"}}));
        REQUIRE(overlay.errors.empty());
        auto const parsed = config::parseAndValidate(overlay.effective);
        INFO(config::formatErrors(parsed.errors));
        REQUIRE(parsed.ok());
        REQUIRE(parsed.config->mxl.domains.size() == 1);
        CHECK(parsed.config->mxl.domains[0].name == "main");
        CHECK(parsed.config->mxl.domains[0].path == "/Volumes/mxl/gw");
        CHECK(config::provenanceMap(file, overlay).at("/mxl/domains/0/path") == "env:MXL_OUTPUT_DOMAIN_DIR");
    }
    SUBCASE("MXL_OUTPUT_DOMAIN_DIR and the per-domain variable must agree")
    {
        auto overlay = config::applyEnvironment(
            testutil::sampleConfig(), testutil::envFrom({{"MXL_OUTPUT_DOMAIN_DIR", "/Volumes/mxl/a"}, {"MXLGW_MXL_DOMAIN_MAIN_PATH", "/Volumes/mxl/b"}}));
        REQUIRE(overlay.errors.size() == 1);
        CHECK(overlay.errors[0].pointer == "/mxl/domains/0/path");
        overlay = config::applyEnvironment(testutil::sampleConfig(),
                                           testutil::envFrom({{"MXL_OUTPUT_DOMAIN_DIR", "/Volumes/mxl/a"}, {"MXLGW_MXL_DOMAIN_MAIN_PATH", "/Volumes/mxl/a"}}));
        CHECK(overlay.errors.empty());
    }
    SUBCASE("only the id: path still required")
    {
        auto file = testutil::sampleConfig();
        file["mxl"].erase("domains");
        file["groups"] = json::array();
        auto const overlay = config::applyEnvironment(file, testutil::envFrom({{"MXL_OUTPUT_DOMAIN_ID", "a1a1a1a1-0000-4000-8000-00000000a001"}}));
        CHECK_FALSE(config::parseAndValidate(overlay.effective).ok());
    }
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
        found = found || (d.variable == "MXL_DOMAIN_SCAN_PATH" && !d.aliases.empty() && d.aliases.front() == "MXLGW_MXL_SCAN_PATH");
    }
    CHECK(found);
    CHECK(config::processEnvironment()("PATH").has_value());
    CHECK_FALSE(config::processEnvironment()("MXLGW_SURELY_NOT_SET_12345").has_value());
}

TEST_CASE("configuration reference lists every environment variable")
{
    auto const doc = util::readFile(std::string(MXLGW_SOURCE_DIR) + "/docs/configuration.md");
    REQUIRE(doc);
    for (auto const& d : config::documentedVariables())
    {
        auto const pattern = d.variable.find('<');
        auto const name = pattern == std::string::npos ? "`" + d.variable + "`" : d.variable.substr(0, pattern);
        CHECK_MESSAGE(doc->find(name) != std::string::npos, d.variable << " is missing in docs/configuration.md (run tools/gen_config_docs.py)");
        for (auto const& alias : d.aliases)
        {
            CHECK_MESSAGE(doc->find("`" + alias + "`") != std::string::npos, alias);
        }
    }
}
