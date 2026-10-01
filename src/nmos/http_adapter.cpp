// SPDX-License-Identifier: MIT
#include "nmos/http_adapter.hpp"

#include <algorithm>
#include <cctype>

#include "cpprest/http_utils.h"
#include "nmos/api_utils.h"
#include "nmos/slog.h"
#include "util/logging.hpp"

namespace mxlgw::nmosnode
{
    namespace
    {
        class Gate final : public slog::base_gate
        {
        public:
            bool pertinent(slog::severity level) const override
            {
                return level >= slog::severities::warning || (level >= slog::severities::info && log::enabled(log::Level::Debug));
            }
            void log(slog::log_message const& message) const override
            {
                // DNS-SD browsing repeats the same message every second while no registry is found.
                if (!_repeats.allow(message.str()))
                {
                    return;
                }
                auto const level = message.level();
                auto const lvl = level >= slog::severities::error     ? log::Level::Error
                                 : level >= slog::severities::warning ? log::Level::Warn
                                                                      : log::Level::Debug;
                log::write(lvl, "external_log", {{"component", "nmos"}, {"message", message.str()}});
            }

        private:
            mutable log::RateLimiter _repeats{std::chrono::seconds(60)};
        };

        ops::HttpRequest toRequest(web::http::http_request& req)
        {
            ops::HttpRequest r;
            r.method = req.method();
            auto const uri = req.request_uri();
            r.path = ops::percentDecode(uri.path());
            r.query = uri.query();
            for (auto const& h : req.headers())
            {
                auto name = h.first;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                r.headers[name] = h.second;
            }
            if (r.method != "GET" && r.method != "HEAD")
            {
                try
                {
                    r.body = req.extract_string(true).get();
                }
                catch (std::exception const&)
                {
                    r.body.clear();
                }
            }
            return r;
        }
    }

    slog::base_gate& nmosGate()
    {
        static Gate gate;
        return gate;
    }

    void mountGatewayRoutes(web::http::experimental::listener::api_router& api, ops::Router const& routes, nmos::settings const& settings,
                            slog::base_gate& gate, bool replaceFinallyHandler)
    {
        using namespace web::http::experimental::listener::api_router_using_declarations;
        if (replaceFinallyHandler)
        {
            api.pop_back();
        }
        auto handler = [&routes](http_request req, http_response res, utility::string_t const&, route_parameters const&) -> pplx::task<bool>
        {
            auto request = toRequest(req);
            auto response = routes.dispatch(request);
            if (!response)
            {
                return pplx::task_from_result(true); // not ours: nmos-cpp's finally handler answers 404
            }
            res.set_status_code(static_cast<web::http::status_code>(response->status));
            for (auto const& [name, value] : response->headers)
            {
                res.headers()[name] = value;
            }
            if (request.method == "HEAD")
            {
                res.headers().set_content_type(response->contentType);
            }
            else
            {
                res.set_body(response->body, response->contentType);
            }
            return pplx::task_from_result(true);
        };
        for (auto const& prefix : routes.mounts())
        {
            api.mount(prefix, handler);
        }
        if (replaceFinallyHandler)
        {
            nmos::add_api_finally_handler(api, nmos::experimental::get_hsts(settings), gate);
        }
    }
}
