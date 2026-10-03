// SPDX-License-Identifier: MIT
// REST API (§11.3) against a fake app::Services backed by a real ConfigStore.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <thread>

#include "app/services.hpp"
#include "config/store.hpp"
#include "helpers.hpp"
#include "ops/webapi.hpp"
#include "util/fs.hpp"

using namespace mxlgw;
using json = nlohmann::json;

namespace
{
    class FakeServices final : public app::Services
    {
    public:
        explicit FakeServices(std::string path, std::map<std::string, std::string> env = {})
            : _store(std::move(path), testutil::envFrom(std::move(env)))
        {
            _store.load();
        }

        config::ConfigStore& store() override { return _store; }
        bool setupMode() const override { return _store.snapshot().config.unconfigured(); }
        bool restartRequired() const override { return !_reasons.empty(); }
        std::vector<std::string> restartReasons() const override { return {_reasons.begin(), _reasons.end()}; }
        void markRestartRequired(std::string const& reason) override { _reasons.insert(reason); }
        json applyGroups() override
        {
            ++applied;
            return {{"created", json::array()}};
        }
        json status() override { return {{"node", {{"label", _store.snapshot().config.node.label}}}}; }
        json nic() override { return json::object(); }
        json ptp() override { return json::object(); }
        json domains() override { return json::object(); }
        std::optional<json> flows(std::string const& domainId) override
        {
            if (domainId == "7e3a8c52-1d2f-4f0a-9b8e-5c6d7e8f9a01")
            {
                return json::array();
            }
            return std::nullopt;
        }
        json nmos() override { return json::object(); }
        std::vector<ops::CheckResult> preflight() override { return {}; }
        ops::Readiness readiness() override
        {
            ops::Readiness r;
            r.ready = false;
            r.reasons.push_back("ptp_unlocked");
            return r;
        }
        std::string metrics() override { return "mxl_st2110_gateway_ready 0\n"; }
        std::string_view adminHtml() const override { return "<!DOCTYPE html><title>ui</title>"; }
        void requestRestart() override { ++restarts; }

        int applied = 0;
        int restarts = 0;

    private:
        config::ConfigStore _store;
        std::set<std::string> _reasons;
    };

    struct Api
    {
        explicit Api(std::map<std::string, std::string> env = {})
            : path(dir.file("gateway.json"))
        {
            testutil::writeFile(path, testutil::sampleConfig().dump(2));
            services = std::make_unique<FakeServices>(path, std::move(env));
            router = ops::makeGatewayRouter(*services);
        }

        ops::HttpResponse call(std::string method, std::string target, std::string body = {}, std::map<std::string, std::string> headers = {})
        {
            ops::HttpRequest q;
            q.method = std::move(method);
            auto const qm = target.find('?');
            q.path = target.substr(0, qm);
            q.query = qm == std::string::npos ? std::string() : target.substr(qm + 1);
            q.body = std::move(body);
            q.headers = {{"host", "gw:8080"}};
            if (!q.body.empty())
            {
                q.headers["content-type"] = "application/json";
            }
            for (auto const& [k, v] : headers)
            {
                q.headers[k] = v;
            }
            auto r = router.dispatch(q);
            REQUIRE(r.has_value());
            return *r;
        }

        static json body(ops::HttpResponse const& r) { return json::parse(r.body); }
        std::string etag() const { return services->store().snapshot().etag; }
        json file() const { return json::parse(services->store().snapshot().raw); }

        testutil::TempDir dir{false};
        std::string path;
        std::unique_ptr<FakeServices> services;
        ops::Router router;
    };
}

TEST_CASE("health endpoints and the embedded UI")
{
    Api api;
    CHECK(api.call("GET", "/livez").status == 200);
    auto const ready = api.call("GET", "/readyz");
    CHECK(ready.status == 503);
    CHECK(Api::body(ready)["reasons"][0] == "ptp_unlocked");
    CHECK(api.call("GET", "/metrics").contentType.find("version=0.0.4") != std::string::npos);
    CHECK(api.call("GET", "/statusz").contentType.find("text/plain") == 0);

    auto const redirect = api.call("GET", "/admin");
    CHECK(redirect.status == 301);
    CHECK(redirect.headers.at("Location") == "/admin/");
    auto const ui = api.call("GET", "/admin/");
    CHECK(ui.status == 200);
    CHECK(ui.contentType.find("text/html") == 0);
    CHECK(ui.body.find("<!DOCTYPE html>") == 0);
    CHECK(api.call("GET", "/admin/groups").status == 200); // SPA deep link

    ops::HttpRequest nmos;
    nmos.method = "GET";
    nmos.path = "/x-nmos/node/v1.3/self";
    CHECK_FALSE(api.router.dispatch(nmos).has_value()); // left to nmos-cpp
}

TEST_CASE("PUT /api/config requires If-Match and returns ETags (§11.3)")
{
    Api api;
    auto const get = api.call("GET", "/api/config");
    CHECK(get.status == 200);
    auto const view = Api::body(get);
    CHECK(get.headers.at("ETag") == view["etag"].get<std::string>());
    CHECK(view["provenance"].is_object());
    CHECK(view["changed_on_disk"] == false);

    auto edited = view["file"];
    edited["groups"][0]["label"] = "CAM 1 renamed";
    CHECK(api.call("PUT", "/api/config", edited.dump()).status == 428);
    CHECK(api.call("PUT", "/api/config", edited.dump(), {{"if-match", "\"stale\""}}).status == 412);

    auto const ok = api.call("PUT", "/api/config", edited.dump(), {{"if-match", view["etag"].get<std::string>()}});
    REQUIRE(ok.status == 200);
    CHECK(ok.headers.at("ETag") != view["etag"].get<std::string>());
    CHECK(Api::body(ok)["restart_required"] == false);
    CHECK(api.services->applied == 1);
    CHECK(api.file()["groups"][0]["label"] == "CAM 1 renamed");

    // The previous ETag is stale now.
    CHECK(api.call("PUT", "/api/config", edited.dump(), {{"if-match", view["etag"].get<std::string>()}}).status == 412);
}

TEST_CASE("validation errors carry JSON pointers; restart-relevant sections are flagged")
{
    Api api;
    auto file = api.file();
    file["groups"][0]["video"][0]["width"] = 1280;
    auto const bad = api.call("PUT", "/api/config", file.dump(), {{"if-match", api.etag()}});
    CHECK(bad.status == 400);
    auto const details = Api::body(bad)["details"];
    REQUIRE(details.is_array());
    bool found = false;
    for (auto const& d : details)
    {
        found = found || d["pointer"].get<std::string>().rfind("/groups/0/video/0", 0) == 0;
    }
    CHECK(found);
    CHECK(api.call("PUT", "/api/config", "{ nope", {{"if-match", api.etag()}}).status == 400);

    file = api.file();
    file["ptp"]["warn_offset_ns"] = 20000;
    auto const ok = api.call("PUT", "/api/config", file.dump(), {{"if-match", api.etag()}});
    REQUIRE(ok.status == 200);
    auto const reasons = Api::body(ok)["restart_reasons"];
    CHECK(std::find(reasons.begin(), reasons.end(), "ptp") != reasons.end());
    CHECK(Api::body(api.call("GET", "/api/config"))["restart_required"] == true);
}

TEST_CASE("environment-set keys are rejected through the API (§9.1)")
{
    Api api({{"MXLGW_NODE_LABEL", "FROM-ENV"}});
    auto const view = Api::body(api.call("GET", "/api/config"));
    CHECK(view["effective"]["node"]["label"] == "FROM-ENV");
    CHECK(view["provenance"]["/node/label"] == "env:MXLGW_NODE_LABEL");
    auto file = view["file"];
    file["node"]["label"] = "OTHER";
    auto const r = api.call("PUT", "/api/config", file.dump(), {{"if-match", view["etag"].get<std::string>()}});
    CHECK(r.status == 400);
    CHECK(Api::body(r)["details"][0]["pointer"] == "/node/label");
}

TEST_CASE("group create, edit, duplicate and delete apply live (§9.3)")
{
    Api api;
    auto const created =
        api.call("POST", "/api/groups",
                 json{{"label", "CAM 2"}, {"direction", "ingest"}, {"domain", "main"}, {"counts", {{"video", 1}, {"audio", 2}, {"anc", 1}}}}.dump());
    REQUIRE(created.status == 201);
    auto const group = Api::body(created)["group"];
    auto const uid = group["uid"].get<std::string>();
    CHECK(group["audio"].size() == 2);
    CHECK(api.file()["groups"].size() == 3);
    CHECK(api.services->applied == 1);

    auto const invalid = api.call("POST", "/api/groups", json{{"direction", "egress"}, {"counts", {{"video", 99}}}}.dump());
    CHECK(invalid.status == 400);
    CHECK(Api::body(invalid)["details"].size() == 3); // label, domain, counts/video

    auto const dup = api.call("POST", "/api/groups", json{{"label", "CAM 1"}, {"domain", "main"}}.dump());
    CHECK(dup.status == 400); // duplicate group label

    auto edited = group;
    edited["label"] = "CAM 2 edited";
    edited["uid"] = "00000000-0000-4000-8000-999999999999"; // ignored: the uid is immutable
    auto const put = api.call("PUT", "/api/groups/" + uid, edited.dump());
    REQUIRE(put.status == 200);
    CHECK(api.file()["groups"][2]["label"] == "CAM 2 edited");
    CHECK(api.file()["groups"][2]["uid"] == uid);
    CHECK(api.call("PUT", "/api/groups/00000000-0000-4000-8000-424242424242", edited.dump()).status == 404);

    auto const pgmUid = api.file()["groups"][1]["uid"].get<std::string>();
    auto const copy = api.call("POST", "/api/groups", json{{"duplicate_of", pgmUid}}.dump());
    REQUIRE(copy.status == 201);
    CHECK(Api::body(copy)["group"]["label"] == "PGM copy");
    CHECK(api.call("POST", "/api/groups", json{{"duplicate_of", "nope"}}.dump()).status == 404);

    CHECK(api.call("DELETE", "/api/groups/" + uid).status == 200);
    CHECK(api.call("DELETE", "/api/groups/" + uid).status == 404);
    CHECK(api.file()["groups"].size() == 3);
    CHECK(api.services->applied == 4);
}

TEST_CASE("mutating requests: content type and same-origin (§10)")
{
    Api api;
    auto const body = json{{"label", "X"}, {"domain", "main"}}.dump();
    CHECK(api.call("POST", "/api/groups", body, {{"origin", "http://evil.example"}}).status == 403);
    CHECK(api.call("POST", "/api/groups", body, {{"content-type", "text/plain"}}).status == 415);
    CHECK(api.call("POST", "/api/groups", body, {{"origin", "http://gw:8080"}}).status == 201);
    auto const uid = api.file()["groups"][2]["uid"].get<std::string>();
    CHECK(api.call("DELETE", "/api/groups/" + uid, {}, {{"origin", "http://evil.example"}}).status == 403);
    CHECK(api.call("POST", "/api/restart", {}, {{"origin", "http://evil.example"}}).status == 403);
    CHECK(api.services->restarts == 0);
    CHECK(api.call("POST", "/api/restart").status == 202);
    CHECK(api.services->restarts == 1);
}

TEST_CASE("export and import round trip with and without keep_ids (§9.4)")
{
    Api api;
    auto const exported = api.call("GET", "/api/config/export");
    CHECK(exported.status == 200);
    CHECK(exported.body == *util::readFile(api.path));
    CHECK(exported.headers.at("Content-Disposition").find("attachment; filename=gateway-GW-") == 0);

    auto const validated = Api::body(api.call("POST", "/api/config/validate", exported.body));
    CHECK(validated["valid"] == true);
    auto broken = json::parse(exported.body);
    broken["nic"]["port_pairs"][0]["primary"]["ip"] = "not-an-ip";
    auto const report = Api::body(api.call("POST", "/api/config/validate", broken.dump()));
    CHECK(report["valid"] == false);
    CHECK_FALSE(report["errors"].empty());

    auto const original = api.file();
    auto const kept = api.call("POST", "/api/config/import?keep_ids=true", exported.body);
    REQUIRE(kept.status == 200);
    CHECK(Api::body(kept)["restart_required"] == true);
    CHECK(api.file()["node"]["id"] == original["node"]["id"]);
    CHECK(api.file()["groups"][0]["uid"] == original["groups"][0]["uid"]);
    CHECK(api.services->applied == 0); // import never applies live

    auto const cloned = api.call("POST", "/api/config/import?keep_ids=false", exported.body);
    REQUIRE(cloned.status == 200);
    CHECK(api.file()["groups"][0]["uid"] != original["groups"][0]["uid"]);
    CHECK(api.file()["node"]["id"] != original["node"]["id"]);

    CHECK(api.call("POST", "/api/config/import", broken.dump()).status == 400);
    CHECK(api.call("GET", "/api/schema").contentType == "application/schema+json");
}

TEST_CASE("a hand edit blocks UI saves until overwritten (§9.2)")
{
    Api api;
    auto const etag = api.etag();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto hand = api.file();
    hand["node"]["description"] = "edited by hand";
    testutil::writeFile(api.path, hand.dump(4));
    CHECK(Api::body(api.call("GET", "/api/config"))["changed_on_disk"] == true);

    CHECK(api.call("POST", "/api/groups", json{{"label", "X"}, {"domain", "main"}}.dump()).status == 409);
    auto ui = Api::body(api.call("GET", "/api/config"))["file"];
    ui["node"]["description"] = "from the UI";
    CHECK(api.call("PUT", "/api/config", ui.dump(), {{"if-match", etag}}).status == 409);
    CHECK(api.call("POST", "/api/config/import", ui.dump()).status == 409);

    auto const forced = api.call("PUT", "/api/config?force=true", ui.dump(), {{"if-match", etag}});
    REQUIRE(forced.status == 200);
    CHECK(api.file()["node"]["description"] == "from the UI");
    CHECK(Api::body(api.call("GET", "/api/config"))["changed_on_disk"] == false);
}

TEST_CASE("tab endpoints")
{
    Api api;
    CHECK(api.call("GET", "/api/flows").status == 400);
    CHECK(api.call("GET", "/api/flows?domain=00000000-0000-4000-8000-000000000001").status == 404);
    CHECK(api.call("GET", "/api/flows?domain=7e3a8c52-1d2f-4f0a-9b8e-5c6d7e8f9a01").status == 200);
    for (auto const* p : {"/api/status", "/api/nic", "/api/ptp", "/api/domains", "/api/nmos", "/api/preflight", "/api/logs?lines=5"})
    {
        CHECK_MESSAGE(api.call("GET", p).status == 200, p);
    }
    CHECK(api.call("DELETE", "/api/status").status == 405);
}

TEST_CASE("restart-relevant sections")
{
    auto const base = testutil::sampleConfig();
    auto changed = base;
    CHECK(ops::restartRelevantChanges(base, changed).empty());
    changed["groups"].clear();
    CHECK(ops::restartRelevantChanges(base, changed).empty()); // groups apply live
    changed["node"]["label"] = "new";
    changed["mxl"]["scan_path"] = "/Volumes/mxl";
    changed["nic"]["lcores"] = "4-5";
    auto const sections = ops::restartRelevantChanges(base, changed);
    CHECK(sections == std::vector<std::string>{"node", "nic", "mxl"});
}
