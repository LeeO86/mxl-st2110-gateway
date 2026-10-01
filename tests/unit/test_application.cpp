// SPDX-License-Identifier: MIT
// The whole gateway process (config store, domain bootstrap, mock media backend, groups, web API)
// without the NMOS/HTTP listener: Options::withNmos = false, requests go straight to the router.
#include <doctest/doctest.h>

#include <chrono>
#include <thread>

#include "app/application.hpp"
#include "helpers.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using json = nlohmann::json;

namespace
{
    ops::HttpResponse call(app::Application& gw, std::string method, std::string target, std::string body = {})
    {
        ops::HttpRequest q;
        q.method = std::move(method);
        auto const qm = target.find('?');
        q.path = target.substr(0, qm);
        q.query = qm == std::string::npos ? std::string() : target.substr(qm + 1);
        q.headers = {{"host", "localhost:18080"}};
        if (!body.empty())
        {
            q.headers["content-type"] = "application/json";
        }
        q.body = std::move(body);
        auto r = gw.router().dispatch(q);
        REQUIRE(r.has_value());
        return *r;
    }

    json body(ops::HttpResponse const& r)
    {
        return json::parse(r.body);
    }

    app::Options options(std::string const& configPath, std::map<std::string, std::string> env = {})
    {
        app::Options o;
        o.configPath = configPath;
        o.env = testutil::envFrom(std::move(env));
        o.withNmos = false;
        return o;
    }

    std::size_t groupCount(app::Application& gw)
    {
        return gw.groups() != nullptr ? gw.groups()->snapshot().size() : 0;
    }
}

TEST_CASE("setup mode without a configuration file (§9.1)")
{
    testutil::TempDir dir(false);
    app::Application gw(options(dir.file("config/gateway.json")));
    REQUIRE(gw.start() == app::exitOk);
    CHECK(gw.setupMode());
    CHECK(util::readFile(dir.file("config/gateway.json")).has_value());
    CHECK(call(gw, "GET", "/livez").status == 200);
    auto const ready = call(gw, "GET", "/readyz");
    CHECK(ready.status == 503);
    CHECK(body(ready)["reasons"][0] == "unconfigured");
    CHECK(call(gw, "GET", "/admin/").contentType.find("text/html") == 0);
    CHECK(body(call(gw, "GET", "/api/status"))["setup_mode"] == true);
    CHECK(gw.groups() == nullptr);
    gw.shutdown();
}

TEST_CASE("invalid configuration and a non-tmpfs domain exit with 78 (§9.2, §8.3)")
{
    testutil::TempDir dir(false);
    auto const path = dir.file("gateway.json");
    testutil::writeFile(path, R"({"schema_version": 1, "nic": {"backend": "warp"}})");
    {
        app::Application gw(options(path));
        CHECK(gw.start() == app::exitConfig);
        gw.shutdown();
    }

    if (dir.tmpfs())
    {
        MESSAGE("skipped: the temporary directory is a tmpfs");
        return;
    }
    auto cfg = testutil::sampleConfig(dir.file("plain/main"));
    cfg["groups"] = json::array();
    testutil::writeFile(path, cfg.dump(2));
    app::Application gw(options(path));
    CHECK(gw.start() == app::exitConfig);
    gw.shutdown();
    CHECK_FALSE(util::readFile(dir.file("plain/main/domain_def.json")).has_value());
}

TEST_CASE("running gateway: bootstrap, live groups, domains and metrics")
{
    testutil::TempDir root;
    if (!root.tmpfs())
    {
        MESSAGE("skipped: no tmpfs for the MXL domain (set MXLGW_TEST_TMPFS)");
        return;
    }
    testutil::TempDir configDir(false);
    auto const path = configDir.file("gateway.json");
    auto cfg = testutil::sampleConfig(root.file("main"));
    cfg["mxl"]["scan_path"] = root.path();
    cfg["mxl"]["domains"][0]["history_duration_ns"] = 100'000'000;
    cfg["node"]["log_level"] = "warn";
    testutil::writeFile(path, cfg.dump(2));

    app::Application gw(options(path));
    REQUIRE(gw.start() == app::exitOk);
    CHECK_FALSE(gw.setupMode());

    // §8.3: domain_def.json created, its id written back into the configuration (owner decision C6).
    auto const def = json::parse(*util::readFile(root.file("main/domain_def.json")));
    auto const domainId = def["id"].get<std::string>();
    auto const written = json::parse(*util::readFile(path));
    CHECK(written["mxl"]["domains"][0]["id"] == domainId);

    CHECK(groupCount(gw) == 2);
    auto const status = body(call(gw, "GET", "/api/status"));
    CHECK(status["setup_mode"] == false);
    CHECK(status["groups"].size() == 2);
    CHECK(status["media"]["backend"] == "mock");

    auto const domains = body(call(gw, "GET", "/api/domains"));
    CHECK(domains["configured"][0]["id"] == domainId);
    CHECK(domains["configured"][0]["tmpfs"] == true);
    CHECK(call(gw, "GET", "/api/flows?domain=" + domainId).status == 200);
    CHECK(call(gw, "GET", "/api/flows?domain=00000000-0000-4000-8000-000000000001").status == 404);

    auto const ready = call(gw, "GET", "/readyz");
    INFO(ready.body);
    CHECK(ready.status == 200);

    // Live group create and delete (§9.3).
    auto const created =
        call(gw, "POST", "/api/groups", json{{"label", "PGM 2"}, {"direction", "egress"}, {"domain", "main"}, {"counts", {{"video", 1}, {"audio", 1}}}}.dump());
    REQUIRE(created.status == 201);
    CHECK(groupCount(gw) == 3);
    auto const uid = body(created)["group"]["uid"].get<std::string>();
    CHECK(body(call(gw, "GET", "/api/status"))["groups"].size() == 3);

    auto const metrics = call(gw, "GET", "/metrics").body;
    CHECK(metrics.find("mxlgw_build_info{") != std::string::npos);
    CHECK(metrics.find("mxlgw_essence_state{") != std::string::npos);
    CHECK(metrics.find("mxlgw_mxl_domain_flows{domain=\"main\"}") != std::string::npos);
    CHECK(metrics.find("group=\"PGM 2\"") != std::string::npos);

    REQUIRE(call(gw, "DELETE", "/api/groups/" + uid).status == 200);
    CHECK(groupCount(gw) == 2);

    // A restart-relevant change is persisted but not applied (§9.3).
    auto view = body(call(gw, "GET", "/api/config"));
    auto file = view["file"];
    file["ptp"]["warn_offset_ns"] = 5000;
    ops::HttpRequest put;
    put.method = "PUT";
    put.path = "/api/config";
    put.headers = {{"host", "localhost:18080"}, {"content-type", "application/json"}, {"if-match", view["etag"].get<std::string>()}};
    put.body = file.dump();
    auto const saved = gw.router().dispatch(put);
    REQUIRE(saved);
    CHECK(saved->status == 200);
    CHECK(body(call(gw, "GET", "/api/status"))["restart_required"] == true);
    CHECK(call(gw, "GET", "/metrics").body.find("mxlgw_restart_required 1") != std::string::npos);

    // A hand edit shows up as changed on disk.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto hand = json::parse(*util::readFile(path));
    hand["node"]["description"] = "hand edit";
    testutil::writeFile(path, hand.dump(4));
    CHECK(body(call(gw, "GET", "/api/status"))["config_changed_on_disk"] == true);

    CHECK(call(gw, "POST", "/api/restart").status == 202);
    CHECK(gw.stopping());
    gw.shutdown();

    // §8.4: nothing of ours is left in the domain after a clean stop.
    CHECK(util::readFile(root.file("main/domain_def.json")).has_value());
}
