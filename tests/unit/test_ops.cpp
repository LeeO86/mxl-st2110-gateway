// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "ops/health.hpp"
#include "ops/http.hpp"
#include "ops/metrics.hpp"
#include "ops/statusz.hpp"

using namespace mxlgw::ops;

TEST_CASE("router: exact, params, prefix, 405, passthrough")
{
    Router r;
    r.add("GET", "/api/status", [](HttpRequest const&) { return HttpResponse::text("status"); });
    r.add("PUT", "/api/groups/{uid}", [](HttpRequest const& q) { return HttpResponse::text(q.params.at("uid")); });
    r.addPrefix("*", "/admin", [](HttpRequest const& q) { return HttpResponse::text("ui:" + q.path); });

    HttpRequest q;
    q.method = "GET";
    q.path = "/api/status";
    CHECK(r.dispatch(q)->body == "status");
    q.method = "HEAD";
    CHECK(r.dispatch(q)->body == "status");
    q.method = "POST";
    CHECK(r.dispatch(q)->status == 405);
    q.method = "PUT";
    q.path = "/api/groups/abc";
    CHECK(r.dispatch(q)->body == "abc");
    q.method = "GET";
    q.path = "/admin/";
    CHECK(r.dispatch(q)->body == "ui:/admin/");
    q.path = "/admin/assets/x.js";
    CHECK(r.dispatch(q).has_value());
    q.path = "/x-nmos/node/v1.3/";
    CHECK_FALSE(r.dispatch(q).has_value());
    auto const mounts = r.mounts();
    CHECK(mounts.size() == 2);
}

TEST_CASE("request helpers and mutating-request checks")
{
    HttpRequest q;
    q.query = "domain=abc%2Fd&keep_ids=false&flag";
    q.headers["content-type"] = "application/json; charset=utf-8";
    q.headers["host"] = "gw:8080";
    CHECK(q.queryParam("domain") == std::string("abc/d"));
    CHECK(q.queryParam("keep_ids") == std::string("false"));
    CHECK(q.queryParam("flag") == std::string());
    CHECK_FALSE(q.queryParam("nope"));
    CHECK(q.header("Content-Type"));
    CHECK_FALSE(checkMutatingRequest(q));
    q.headers["origin"] = "http://gw:8080";
    CHECK_FALSE(checkMutatingRequest(q));
    q.headers["origin"] = "http://evil:8080";
    CHECK(checkMutatingRequest(q)->status == 403);
    q.headers["content-type"] = "text/plain";
    CHECK(checkMutatingRequest(q)->status == 415);
    CHECK(percentDecode("a+b%20c%zz") == "a b c%zz");
    auto const e = HttpResponse::error(400, "bad", {{"x", 1}});
    CHECK(nlohmann::json::parse(e.body)["details"]["x"] == 1);
}

TEST_CASE("metrics exposition format")
{
    MetricsRegistry reg;
    auto const id = reg.add(
        [](MetricsWriter& w)
        {
            w.gauge("mxlgw_ready", "Readiness", {}, 1);
            w.counter("mxlgw_rx_frames_total", "Frames", {{"group", "CAM \"1\""}, {"result", "complete"}}, 42);
            w.counter("mxlgw_rx_frames_total", "Frames", {{"group", "CAM \"1\""}, {"result", "incomplete"}}, 0);
            w.gauge("mxlgw_clock_mtl_minus_host_tai_ns", "Offset", {}, -12.5);
        });
    auto const text = reg.scrape();
    CHECK(text.find("# TYPE mxlgw_ready gauge\nmxlgw_ready 1\n") != std::string::npos);
    CHECK(text.find("mxlgw_rx_frames_total{group=\"CAM \\\"1\\\"\",result=\"complete\"} 42") != std::string::npos);
    CHECK(text.find("# HELP mxlgw_rx_frames_total Frames") != std::string::npos);
    CHECK(text.find("-12.5") != std::string::npos);
    // TYPE appears once per family.
    CHECK(text.find("# TYPE mxlgw_rx_frames_total") == text.rfind("# TYPE mxlgw_rx_frames_total"));
    reg.remove(id);
    CHECK(reg.scrape().empty());
    CHECK(escapeLabelValue("a\\b\n") == "a\\\\b\\n");
}

TEST_CASE("readiness reasons (§10)")
{
    ReadinessInputs in;
    in.mediaUp = true;
    in.ptpLocked = true;
    in.clockOffsetNs = 500;
    in.nmosRegistered = true;
    CHECK(evaluateReadiness(in).ready);

    in.ptpLocked = false;
    auto r = evaluateReadiness(in);
    CHECK_FALSE(r.ready);
    CHECK(r.reasons == std::vector<std::string>{"ptp_unlocked"});
    in.requireLock = false;
    r = evaluateReadiness(in);
    CHECK(r.ready);
    CHECK(r.warnings == std::vector<std::string>{"ptp_unlocked"});

    in.clockOffsetNs = 2'000'000;
    CHECK(evaluateReadiness(in).reasons == std::vector<std::string>{"clock_mismatch"});
    in.clockOffsetNs = -2'000'000;
    CHECK_FALSE(evaluateReadiness(in).ready);
    in.clockOffsetNs.reset();

    // G7: registration counts only for a configured registry, per node.
    in.nmosRegistered = false;
    CHECK(evaluateReadiness(in).ready);
    in.registryConfigured = true;
    CHECK(evaluateReadiness(in).reasons == std::vector<std::string>{"nmos_not_registered"});
    in.nmosRegistered = true;
    in.st2110RegistryConfigured = true;
    CHECK(evaluateReadiness(in).reasons == std::vector<std::string>{"st2110_nmos_not_registered"});
    in.st2110Registered = true;
    CHECK(evaluateReadiness(in).ready);
    in.shuttingDown = true;
    CHECK(evaluateReadiness(in).reasons == std::vector<std::string>{"shutting_down"});
    in.shuttingDown = false;

    ReadinessInputs setup;
    setup.setupMode = true;
    r = evaluateReadiness(setup);
    CHECK(r.reasons == std::vector<std::string>{"unconfigured"});

    ReadinessInputs test;
    test.mediaUp = true;
    test.testBackend = true;
    test.ptpLocked = false;
    test.nmosRegistered = true;
    test.domainsOk = false;
    test.configValid = false;
    test.extraReasons = {"x"};
    r = evaluateReadiness(test);
    CHECK(r.warnings.size() == 2);
    CHECK(r.reasons.size() == 3);
    CHECK(r.toJson()["ready"] == false);
    test = ReadinessInputs{};
    CHECK(evaluateReadiness(test).reasons.front() == "media_backend_down");
}

TEST_CASE("statusz rendering")
{
    auto const st = nlohmann::json::parse(R"({
      "versions": {"gateway": "1.2.3"},
      "node": {"label": "GW", "id": "x"},
      "readiness": {"ready": false, "reasons": ["ptp_unlocked"]},
      "setup_mode": false,
      "nic": {"backend": "mock"},
      "ptp": {"mode": "external", "selected_port": null, "mtl_minus_host_tai_ns": 3},
      "domains": [{"kind": "configured", "id": "d", "path": "/p", "flow_count": 2}],
      "groups": [{"label": "CAM 1", "direction": "ingest", "enabled": true,
                  "essences": [{"type": "video", "label": "V", "state": "waiting_for_flow", "reason": "domain_not_found"}]}]
    })");
    auto const text = renderStatusz(st);
    CHECK(text.find("mxl-st2110-gateway 1.2.3") != std::string::npos);
    CHECK(text.find("ptp_unlocked") != std::string::npos);
    CHECK(text.find("waiting_for_flow (domain_not_found)") != std::string::npos);
    CHECK(text.find("configured d /p flows=2") != std::string::npos);
    CHECK_FALSE(renderStatusz(nlohmann::json::object()).empty());
}
