// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "config/store.hpp"
#include "helpers.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using nlohmann::json;
using ojson = nlohmann::ordered_json;

TEST_CASE("missing file creates the minimal configuration with a node id")
{
    testutil::TempDir dir;
    config::ConfigStore store(dir.file("config/gateway.json"), testutil::envFrom({}));
    CHECK(store.load() == config::ConfigStore::LoadResult::CreatedMinimal);
    auto const s = store.snapshot();
    CHECK(s.config.unconfigured());
    REQUIRE(s.config.node.id);
    auto const onDisk = json::parse(*util::readFile(dir.file("config/gateway.json")));
    CHECK(onDisk["node"]["id"] == s.config.node.id->toString());
    CHECK(s.etag.size() > 2);
    CHECK_FALSE(store.changedOnDisk());

    // Second load keeps the id.
    config::ConfigStore again(dir.file("config/gateway.json"), testutil::envFrom({}));
    CHECK(again.load() == config::ConfigStore::LoadResult::Loaded);
    CHECK(again.snapshot().config.node.id == s.config.node.id);
}

TEST_CASE("invalid file is rejected and not rewritten")
{
    testutil::TempDir dir;
    auto const path = dir.file("gateway.json");
    testutil::writeFile(path, R"({"schema_version": 1, "groups": [{"label": "x"}]})");
    config::ConfigStore store(path, testutil::envFrom({}));
    CHECK_THROWS_AS(store.load(), config::ConfigError);
    CHECK(*util::readFile(path) == R"({"schema_version": 1, "groups": [{"label": "x"}]})");

    testutil::writeFile(path, "{not json");
    try
    {
        store.load();
        CHECK(false);
    }
    catch (config::ConfigError const& e)
    {
        CHECK_FALSE(e.errors().empty());
        CHECK(std::string(e.what()).find("invalid JSON") != std::string::npos);
    }
}

TEST_CASE("missing uids are generated and written back")
{
    testutil::TempDir dir;
    auto const path = dir.file("gateway.json");
    auto j = testutil::sampleConfig();
    j["groups"][0].erase("uid");
    j["groups"][0]["video"][0].erase("uid");
    j["node"].erase("id");
    testutil::writeFile(path, j.dump(2));
    config::ConfigStore store(path, testutil::envFrom({}));
    store.load();
    auto const disk = json::parse(*util::readFile(path));
    CHECK(util::isUuid(disk["groups"][0]["uid"].get<std::string>()));
    CHECK(util::isUuid(disk["groups"][0]["video"][0]["uid"].get<std::string>()));
    CHECK(util::isUuid(disk["node"]["id"].get<std::string>()));
}

TEST_CASE("replace with If-Match, .bak, conflict, env-bound keys")
{
    testutil::TempDir dir;
    auto const path = dir.file("gateway.json");
    testutil::writeFile(path, testutil::sampleConfig().dump(2));
    config::ConfigStore store(path, testutil::envFrom({{"MXLGW_NODE_LABEL", "ENV"}}));
    store.load();
    auto s = store.snapshot();
    CHECK(s.config.node.label == "ENV");

    auto edited = s.file;
    edited["node"]["description"] = "edited";
    CHECK(store.replace(edited, std::string("\"wrong\"")).conflict);
    CHECK(store.replace(edited, std::nullopt).conflict);
    auto const ok = store.replace(edited, s.etag);
    CHECK(ok.ok());
    CHECK(store.snapshot().config.node.description == "edited");
    CHECK(util::readFile(path + ".bak").has_value());
    CHECK(store.snapshot().etag != s.etag);

    // Changing an env-bound key through the API is refused.
    s = store.snapshot();
    auto envEdit = s.file;
    envEdit["node"]["label"] = "other";
    auto const refused = store.replace(envEdit, s.etag);
    REQUIRE_FALSE(refused.errors.empty());
    CHECK(refused.errors.front().pointer == "/node/label");

    // Invalid content is refused and the file stays untouched.
    auto bad = s.file;
    bad["groups"][0]["video"][0]["width"] = 1;
    auto const before = *util::readFile(path);
    CHECK_FALSE(store.replace(bad, s.etag).ok());
    CHECK(*util::readFile(path) == before);
}

TEST_CASE("changed on disk blocks updates until accepted")
{
    testutil::TempDir dir;
    auto const path = dir.file("gateway.json");
    testutil::writeFile(path, testutil::sampleConfig().dump(2));
    config::ConfigStore store(path, testutil::envFrom({}));
    store.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    testutil::writeFile(path, testutil::sampleConfig().dump(4));
    CHECK(store.changedOnDisk());
    auto const r = store.update([](ojson& f) { f["node"]["description"] = "x"; });
    CHECK(r.conflict);
    auto const forced = store.update([](ojson& f) { f["node"]["description"] = "x"; }, true);
    CHECK(forced.ok());
    CHECK_FALSE(store.changedOnDisk());
    store.acceptDiskVersion();
    CHECK_FALSE(store.changedOnDisk());
}

TEST_CASE("write back respects the environment")
{
    testutil::TempDir dir;
    auto const path = dir.file("gateway.json");
    testutil::writeFile(path, testutil::sampleConfig().dump(2));
    config::ConfigStore store(path, testutil::envFrom({{"MXLGW_MXL_DOMAIN_MAIN_ID", "22222222-2222-4222-8222-222222222222"}}));
    store.load();
    CHECK_FALSE(store.writeBack("/mxl/domains/0/id", "33333333-3333-4333-8333-333333333333"));
    CHECK(store.writeBack("/node/label", "WB"));
    CHECK_FALSE(store.writeBack("/node/label", "WB"));
    CHECK(store.snapshot().config.node.label == "WB");
}

TEST_CASE("import with and without keep_ids")
{
    testutil::TempDir dir;
    auto const path = dir.file("gateway.json");
    testutil::writeFile(path, testutil::sampleConfig().dump(2));
    config::ConfigStore store(path, testutil::envFrom({}));
    store.load();
    auto const original = store.snapshot();

    CHECK(store.importText(original.raw, true).ok());
    CHECK(store.snapshot().config.groups[0].uid == original.config.groups[0].uid);
    CHECK(store.snapshot().config.node.id == original.config.node.id);

    CHECK(store.importText(original.raw, false).ok());
    auto const cloned = store.snapshot();
    CHECK(cloned.config.groups[0].uid != original.config.groups[0].uid);
    CHECK(cloned.config.groups[0].video[0].uid != original.config.groups[0].video[0].uid);
    CHECK(cloned.config.node.id != original.config.node.id);

    CHECK_FALSE(store.importText("{", true).ok());
    CHECK(store.exportRaw() == cloned.raw);
}
