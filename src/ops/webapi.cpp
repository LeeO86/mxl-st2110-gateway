// SPDX-License-Identifier: MIT
#include "ops/webapi.hpp"

#include <ctime>

#include "config/group_factory.hpp"
#include "config/schema.hpp"
#include "ops/statusz.hpp"
#include "util/logging.hpp"

namespace mxlgw::ops
{
    namespace
    {
        using json = nlohmann::json;
        using ojson = nlohmann::ordered_json;

        json errorsJson(config::ValidationErrors const& errors)
        {
            json arr = json::array();
            for (auto const& e : errors)
            {
                arr.push_back({{"pointer", e.pointer}, {"message", e.message}});
            }
            return arr;
        }

        HttpResponse validationFailed(config::ValidationErrors const& errors)
        {
            return HttpResponse::error(400, "validation failed", errorsJson(errors));
        }

        std::optional<ojson> parseBody(HttpRequest const& req, HttpResponse& error)
        {
            try
            {
                return ojson::parse(req.body);
            }
            catch (std::exception const& ex)
            {
                error = HttpResponse::error(400, std::string("invalid JSON: ") + ex.what());
                return std::nullopt;
            }
        }

        std::string today()
        {
            auto const t = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&t, &tm);
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
            return buf;
        }

        std::string safeFileName(std::string s)
        {
            for (auto& c : s)
            {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
                {
                    c = '-';
                }
            }
            return s;
        }

        json configView(app::Services& services)
        {
            auto const snap = services.store().snapshot();
            json bindings = json::array();
            for (auto const& b : snap.overlay.bindings)
            {
                bindings.push_back({{"pointer", b.pointer}, {"variable", b.variable}});
            }
            return {{"file", snap.file},
                    {"effective", snap.overlay.effective},
                    {"provenance", snap.provenance},
                    {"environment", bindings},
                    {"etag", snap.etag},
                    {"path", services.store().path()},
                    {"changed_on_disk", services.store().changedOnDisk()},
                    {"restart_required", services.restartRequired()},
                    {"restart_reasons", services.restartReasons()}};
        }

        /// Writes `mutate` through the store, then applies groups live and flags restart-relevant changes.
        HttpResponse writeConfig(app::Services& services, config::UpdateResult const& result, json const& before)
        {
            if (result.conflict)
            {
                return HttpResponse::error(services.store().changedOnDisk() ? 409 : 412,
                                           services.store().changedOnDisk() ? "configuration changed on disk — reload (restart) or overwrite first"
                                                                            : "ETag does not match (If-Match)");
            }
            if (!result.errors.empty())
            {
                return validationFailed(result.errors);
            }
            auto const after = services.store().snapshot().overlay.effective;
            for (auto const& section : restartRelevantChanges(before, after))
            {
                services.markRestartRequired(section);
            }
            json body;
            body["restart_required"] = services.restartRequired();
            body["restart_reasons"] = services.restartReasons();
            body["groups"] = services.setupMode() ? json::array() : services.applyGroups();
            body["etag"] = services.store().snapshot().etag;
            auto res = HttpResponse::json(body);
            res.headers["ETag"] = services.store().snapshot().etag;
            return res;
        }

        std::optional<std::size_t> findGroup(ojson const& file, std::string const& uid)
        {
            if (!file.contains("groups") || !file["groups"].is_array())
            {
                return std::nullopt;
            }
            for (std::size_t i = 0; i < file["groups"].size(); ++i)
            {
                if (file["groups"][i].value("uid", std::string()) == uid)
                {
                    return i;
                }
            }
            return std::nullopt;
        }
    }

    std::vector<std::string> restartRelevantChanges(nlohmann::json const& before, nlohmann::json const& after)
    {
        std::vector<std::string> out;
        for (auto const* section : {"node", "nic", "ptp"})
        {
            if (before.value(section, json::object()) != after.value(section, json::object()))
            {
                out.emplace_back(section);
            }
        }
        auto mxlOf = [](json const& j)
        {
            auto m = j.value("mxl", json::object());
            return m;
        };
        if (mxlOf(before) != mxlOf(after))
        {
            out.emplace_back("mxl");
        }
        return out;
    }

    Router makeGatewayRouter(app::Services& services)
    {
        Router r;

        // ---- health (§10)
        r.add("GET", "/livez", [](HttpRequest const&) { return HttpResponse::json({{"status", "ok"}}); });
        r.add("GET", "/readyz",
              [&services](HttpRequest const&)
              {
                  auto const readiness = services.readiness();
                  return HttpResponse::json(readiness.toJson(), readiness.ready ? 200 : 503);
              });
        r.add("GET", "/statusz", [&services](HttpRequest const&) { return HttpResponse::text(renderStatusz(services.status())); });
        r.add("GET", "/metrics",
              [&services](HttpRequest const&) { return HttpResponse::text(services.metrics(), 200, "text/plain; version=0.0.4; charset=utf-8"); });

        // ---- admin UI (§11.1)
        r.addPrefix("GET", "/admin",
                    [&services](HttpRequest const& req)
                    {
                        if (req.path == "/admin")
                        {
                            auto res = HttpResponse::text("", 301);
                            res.headers["Location"] = "/admin/";
                            return res;
                        }
                        auto res = HttpResponse::text(std::string(services.adminHtml()), 200, "text/html; charset=utf-8");
                        res.headers["Cache-Control"] = "no-cache";
                        return res;
                    });

        // ---- REST API (§11.3)
        r.add("GET", "/api/status", [&services](HttpRequest const&) { return HttpResponse::json(services.status()); });
        r.add("GET", "/api/config",
              [&services](HttpRequest const&)
              {
                  auto res = HttpResponse::json(configView(services));
                  res.headers["ETag"] = services.store().snapshot().etag;
                  return res;
              });
        r.add("PUT", "/api/config",
              [&services](HttpRequest const& req)
              {
                  if (auto bad = checkMutatingRequest(req))
                  {
                      return *bad;
                  }
                  HttpResponse error;
                  auto body = parseBody(req, error);
                  if (!body)
                  {
                      return error;
                  }
                  auto const ifMatch = req.header("if-match");
                  if (!ifMatch)
                  {
                      return HttpResponse::error(428, "If-Match header required");
                  }
                  bool const force = req.queryParam("force") == "true";
                  if (force)
                  {
                      // "overwrite with UI state" after a change on disk (§9.2)
                      services.store().acceptDiskVersion();
                  }
                  auto const snap = services.store().snapshot();
                  return writeConfig(services, services.store().replace(*body, force ? std::optional<std::string>{snap.etag} : ifMatch),
                                     snap.overlay.effective);
              });
        r.add("GET", "/api/config/export",
              [&services](HttpRequest const&)
              {
                  auto res = HttpResponse::text(services.store().exportRaw(), 200, "application/json");
                  auto const label = services.store().snapshot().config.node.label;
                  res.headers["Content-Disposition"] = "attachment; filename=gateway-" + safeFileName(label) + "-" + today() + ".json";
                  return res;
              });
        r.add("POST", "/api/config/import",
              [&services](HttpRequest const& req)
              {
                  if (auto bad = checkMutatingRequest(req))
                  {
                      return *bad;
                  }
                  bool const keepIds = req.queryParam("keep_ids").value_or("true") != "false";
                  auto const result = services.store().importText(req.body, keepIds);
                  if (result.conflict)
                  {
                      return HttpResponse::error(409, "configuration changed on disk — reload (restart) or overwrite first");
                  }
                  if (!result.errors.empty())
                  {
                      return validationFailed(result.errors);
                  }
                  services.markRestartRequired("import");
                  // ?restart=true: restore and apply in one call (graceful exit 0, the orchestrator restarts the container).
                  bool const restart = req.queryParam("restart") == "true";
                  if (restart)
                  {
                      services.requestRestart();
                  }
                  return HttpResponse::json(
                      {{"restart_required", !restart}, {"restarting", restart}, {"keep_ids", keepIds}, {"etag", services.store().snapshot().etag}},
                      restart ? 202 : 200);
              });
        r.add("POST", "/api/config/validate",
              [&services](HttpRequest const& req)
              {
                  if (auto bad = checkMutatingRequest(req))
                  {
                      return *bad;
                  }
                  HttpResponse error;
                  auto body = parseBody(req, error);
                  if (!body)
                  {
                      return error;
                  }
                  auto const result = services.store().validateFile(*body);
                  return HttpResponse::json({{"valid", result.ok()}, {"errors", errorsJson(result.errors)}});
              });
        r.add("GET", "/api/schema",
              [](HttpRequest const&) { return HttpResponse::text(std::string(config::gatewaySchemaText()), 200, "application/schema+json"); });

        r.add("POST", "/api/groups",
              [&services](HttpRequest const& req)
              {
                  if (auto bad = checkMutatingRequest(req))
                  {
                      return *bad;
                  }
                  HttpResponse error;
                  auto body = parseBody(req, error);
                  if (!body)
                  {
                      return error;
                  }
                  ojson group;
                  if (body->contains("duplicate_of"))
                  {
                      auto const file = services.store().snapshot().file;
                      auto const idx = findGroup(file, body->value("duplicate_of", std::string()));
                      if (!idx)
                      {
                          return HttpResponse::error(404, "group to duplicate not found");
                      }
                      group = config::duplicateGroup(file["groups"][*idx]);
                  }
                  else
                  {
                      config::ValidationErrors errors;
                      auto const request = config::parseGroupRequest(json(*body), errors);
                      if (!errors.empty())
                      {
                          return validationFailed(errors);
                      }
                      group = config::makeGroup(request, json(*body));
                  }
                  auto const before = services.store().snapshot().overlay.effective;
                  auto const result = services.store().update(
                      [&](ojson& file)
                      {
                          if (!file.contains("groups") || !file["groups"].is_array())
                          {
                              file["groups"] = ojson::array();
                          }
                          file["groups"].push_back(group);
                      });
                  auto res = writeConfig(services, result, before);
                  if (res.status == 200)
                  {
                      auto j = json::parse(res.body);
                      j["group"] = group;
                      res = HttpResponse::json(j, 201);
                      res.headers["ETag"] = services.store().snapshot().etag;
                  }
                  return res;
              });
        r.add("PUT", "/api/groups/{uid}",
              [&services](HttpRequest const& req)
              {
                  if (auto bad = checkMutatingRequest(req))
                  {
                      return *bad;
                  }
                  HttpResponse error;
                  auto body = parseBody(req, error);
                  if (!body)
                  {
                      return error;
                  }
                  auto const uid = req.params.at("uid");
                  if (!findGroup(services.store().snapshot().file, uid))
                  {
                      return HttpResponse::error(404, "group not found");
                  }
                  (*body)["uid"] = uid; // the uid is immutable (§7.3)
                  auto const before = services.store().snapshot().overlay.effective;
                  auto const result = services.store().update(
                      [&](ojson& file)
                      {
                          if (auto const idx = findGroup(file, uid))
                          {
                              file["groups"][*idx] = *body;
                          }
                      });
                  return writeConfig(services, result, before);
              });
        r.add("DELETE", "/api/groups/{uid}",
              [&services](HttpRequest const& req)
              {
                  if (auto const origin = req.header("origin"); origin && !origin->empty())
                  {
                      HttpRequest copy = req;
                      copy.headers["content-type"] = "application/json";
                      if (auto bad = checkMutatingRequest(copy))
                      {
                          return *bad;
                      }
                  }
                  auto const uid = req.params.at("uid");
                  if (!findGroup(services.store().snapshot().file, uid))
                  {
                      return HttpResponse::error(404, "group not found");
                  }
                  auto const before = services.store().snapshot().overlay.effective;
                  auto const result = services.store().update(
                      [&](ojson& file)
                      {
                          if (auto const idx = findGroup(file, uid))
                          {
                              file["groups"].erase(*idx);
                          }
                      });
                  return writeConfig(services, result, before);
              });

        r.add("GET", "/api/nic", [&services](HttpRequest const&) { return HttpResponse::json(services.nic()); });
        r.add("GET", "/api/ptp", [&services](HttpRequest const&) { return HttpResponse::json(services.ptp()); });
        r.add("GET", "/api/domains", [&services](HttpRequest const&) { return HttpResponse::json(services.domains()); });
        r.add("GET", "/api/flows",
              [&services](HttpRequest const& req)
              {
                  auto const domain = req.queryParam("domain");
                  if (!domain || domain->empty())
                  {
                      return HttpResponse::error(400, "query parameter 'domain' (domain_def.json id) is required");
                  }
                  auto flows = services.flows(*domain);
                  if (!flows)
                  {
                      return HttpResponse::error(404, "domain not found");
                  }
                  return HttpResponse::json(*flows);
              });
        r.add("GET", "/api/nmos", [&services](HttpRequest const&) { return HttpResponse::json(services.nmos()); });
        r.add("GET", "/api/preflight", [&services](HttpRequest const&) { return HttpResponse::json(toJson(services.preflight())); });
        r.add("GET", "/api/logs",
              [](HttpRequest const& req)
              {
                  auto const n = req.queryParam("lines");
                  std::size_t lines = 500;
                  if (n)
                  {
                      try
                      {
                          lines = static_cast<std::size_t>(std::stoul(*n));
                      }
                      catch (std::exception const&)
                      {}
                  }
                  return HttpResponse::json(log::recent(lines));
              });
        r.add("POST", "/api/restart",
              [&services](HttpRequest const& req)
              {
                  if (auto const origin = req.header("origin"); origin && !origin->empty())
                  {
                      HttpRequest copy = req;
                      copy.headers["content-type"] = "application/json";
                      if (auto bad = checkMutatingRequest(copy))
                      {
                          return *bad;
                      }
                  }
                  services.requestRestart();
                  return HttpResponse::json({{"restarting", true}}, 202);
              });
        return r;
    }
}
