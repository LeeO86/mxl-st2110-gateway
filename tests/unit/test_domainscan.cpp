// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <filesystem>

#include "helpers.hpp"
#include "mxlbridge/domaindef.hpp"
#include "mxlbridge/domainscan.hpp"

using namespace mxlgw;
using namespace mxlgw::mxlbridge;

namespace
{
    std::string const idA = "aaaaaaaa-0000-4000-8000-00000000000a";
    std::string const idB = "bbbbbbbb-0000-4000-8000-00000000000b";

    void makeDomain(std::string const& path, std::string const& id, std::string const& extra = "")
    {
        std::filesystem::create_directories(path);
        testutil::writeFile(path + "/domain_def.json", R"({"id": ")" + id + R"(", "label": "L", "description": "D", "tags": {})" + extra + "}");
    }
}

TEST_CASE("domain_def.json parsing ignores unknown fields and detects the mirror marker")
{
    std::string error;
    auto def = parseDomainDef(R"({"id": ")" + idA + R"(", "label": "A", "description": "x", "tags": {"k": ["v"]}, "future": 1})", error);
    REQUIRE(def);
    CHECK_FALSE(def->mirror);
    def = parseDomainDef(
        R"({"id": ")" + idA +
            R"(", "label": "A", "description": "x", "tags": {}, "x-mxl-fabrics-agent": {"mirror": true, "source_host_id": "a", "owner_host_id": "b"}})",
        error);
    REQUIRE(def);
    CHECK(def->mirror);
    CHECK(def->sourceHostId == "a");
    CHECK(def->ownerHostId == "b");
    CHECK_FALSE(parseDomainDef("{", error));
    CHECK_FALSE(parseDomainDef("[]", error));
    CHECK_FALSE(parseDomainDef(R"({"id": "x", "label": "A", "description": "x", "tags": {}})", error));
    // Only the id is required: mxl-test-player, mxl-color-corrector and fabrics-agent mirrors write no description or tags.
    def = parseDomainDef(R"({"id": ")" + idA + R"(", "label": "MXL Test Player"})", error);
    REQUIRE(def);
    CHECK(def->label == "MXL Test Player");
    CHECK(def->description.empty());
    CHECK(def->tags == nlohmann::json::object());
    def = parseDomainDef(R"({"id": ")" + idA + R"(", "x-mxl-fabrics-agent": {"mirror": true}})", error);
    REQUIRE(def);
    CHECK(def->label.empty());
    CHECK(def->mirror);
    CHECK_FALSE(parseDomainDef(R"({"label": "A", "description": "x", "tags": {}})", error));
    CHECK(error == "missing required field 'id'");
    CHECK_FALSE(parseDomainDef(R"({"id": ")" + idA + R"(", "label": 1, "description": "x", "tags": {}})", error));
    CHECK_FALSE(parseDomainDef(R"({"id": ")" + idA + R"(", "description": 1})", error));
    CHECK_FALSE(parseDomainDef(R"({"id": ")" + idA + R"(", "label": "A", "description": "x", "tags": {"k": "v"}})", error));
    CHECK_FALSE(parseDomainDef(R"({"id": ")" + idA + R"(", "label": "A", "description": "x", "tags": {"k": [1]}})", error));
    CHECK_FALSE(parseDomainDef(R"({"id": ")" + idA + R"(", "label": "A", "description": "x", "tags": []})", error));
    CHECK(parseDomainDef(renderDomainDef(*util::parseUuid(idB), "B", "Bd"), error)->id.toString() == idB);
    CHECK(isMirrorBasename("/Volumes/mxl/mirror-123"));
    CHECK(isMirrorBasename("/Volumes/mxl/mirror-123/"));
    CHECK_FALSE(isMirrorBasename("/Volumes/mxl/main"));
}

TEST_CASE("scan: identity from domain_def.json, classification, conflicts, no negative caching")
{
    testutil::TempDir root;
    if (!root.tmpfs())
    {
        MESSAGE("skipped: no tmpfs available for the scan test");
        return;
    }
    makeDomain(root.file("main"), idA);
    // A mirror under an unrelated directory name: identity is the id, never the name.
    makeDomain(root.file("mirror-" + idB), idB, R"(, "x-mxl-fabrics-agent": {"mirror": true, "source_host_id": "host-a"})");
    makeDomain(root.file("broken"), idA.substr(0, 10)); // invalid id -> skipped
    std::filesystem::create_directories(root.file("not-a-domain"));
    std::filesystem::create_directories(root.file("main/" + idA + ".mxl-flow"));
    std::filesystem::create_directories(root.file("main/garbage.mxl-flow"));

    auto const scan = scanDomains(root.path(), {{"main", root.file("main")}});
    REQUIRE(scan.domains.size() == 2);
    auto const* main = scan.findById(*util::parseUuid(idA));
    REQUIRE(main);
    CHECK(main->kind == DomainKind::Configured);
    CHECK(main->configuredName == "main");
    CHECK(main->flowCount == 1);
    CHECK(main->tmpfs);
    auto const* mirror = scan.findById(*util::parseUuid(idB));
    REQUIRE(mirror);
    CHECK(mirror->kind == DomainKind::Mirror);
    CHECK(mirror->sourceHostId == "host-a");
    CHECK(scan.skipped.size() == 1);
    CHECK(std::string(toName(DomainKind::Mirror)) == "mirror");

    // Duplicate id outside the configured domain: conflict, the configured one wins.
    makeDomain(root.file("copy"), idA);
    auto const dup = scanDomains(root.path(), {{"main", root.file("main")}});
    CHECK(dup.conflicts.size() == 1);
    CHECK(dup.findById(*util::parseUuid(idA))->kind == DomainKind::Configured);
    // Without a configured winner both are conflicts.
    auto const noWinner = scanDomains(root.path(), {});
    CHECK(noWinner.conflicts.size() == 2);
    CHECK_FALSE(noWinner.findById(*util::parseUuid(idA)));
    std::filesystem::remove_all(root.file("copy"));

    DomainDirectory dir(root.path(), {{"main", root.file("main")}});
    dir.rescan();
    auto const late = *util::parseUuid("cccccccc-0000-4000-8000-00000000000c");
    CHECK_FALSE(dir.findById(late));
    makeDomain(root.file("late"), late.toString());
    CHECK(dir.findById(late)); // not cached as missing
    CHECK(dir.last().findById(late));

    // findFlow prefers the preferred/configured domain, then mirrors.
    auto const flow = *util::parseUuid("dddddddd-0000-4000-8000-00000000000d");
    CHECK_FALSE(dir.findFlow(flow, std::nullopt));
    std::filesystem::create_directories(root.file("mirror-" + idB + "/" + flow.toString() + ".mxl-flow"));
    auto const found = dir.findFlow(flow, *util::parseUuid(idA));
    REQUIRE(found);
    CHECK(found->id == *util::parseUuid(idB));
    std::filesystem::create_directories(root.file("main/" + flow.toString() + ".mxl-flow"));
    CHECK(dir.findFlow(flow, std::nullopt)->id == *util::parseUuid(idA));

    auto const flows = listFlowDirs(root.file("main"));
    CHECK(flows.size() == 2);
    CHECK(flowDirExists(root.file("main"), flow));
    CHECK(dir.scanPath() == root.path());

    // Scan path missing: configured domains still accessible.
    auto const noScan = scanDomains(std::string("/nonexistent/path"), {{"main", root.file("main")}});
    CHECK(noScan.domains.size() == 1);
}

TEST_CASE("non-tmpfs candidates are skipped")
{
    testutil::TempDir disk(false);
    if (disk.tmpfs())
    {
        MESSAGE("skipped: /tmp is tmpfs here");
        return;
    }
    makeDomain(disk.file("d"), idA);
    auto const scan = scanDomains(disk.path(), {});
    CHECK(scan.domains.empty());
    REQUIRE(scan.skipped.size() == 1);
    CHECK(scan.skipped[0].reason.find("not on tmpfs") != std::string::npos);
}
