// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mxlgw::ops
{
    /// Transport-independent HTTP request/response (the cpprest adapter translates, §10).
    struct HttpRequest
    {
        std::string method;
        std::string path;                           // without query, percent-decoded
        std::string query;                          // raw query string
        std::map<std::string, std::string> headers; // lower-case names
        std::string body;
        std::map<std::string, std::string> params; // filled by the router from {name} segments

        std::optional<std::string> header(std::string const& name) const;
        std::optional<std::string> queryParam(std::string const& name) const;
    };

    struct HttpResponse
    {
        int status = 200;
        std::string contentType = "application/json";
        std::map<std::string, std::string> headers;
        std::string body;

        static HttpResponse json(nlohmann::json const& body, int status = 200);
        static HttpResponse text(std::string body, int status = 200, std::string contentType = "text/plain; charset=utf-8");
        static HttpResponse error(int status, std::string const& message, nlohmann::json details = nullptr);
    };

    using Handler = std::function<HttpResponse(HttpRequest const&)>;

    /// Minimal router: exact paths with optional {param} segments, plus prefix routes.
    class Router
    {
    public:
        void add(std::string method, std::string pattern, Handler handler);
        void addPrefix(std::string method, std::string prefix, Handler handler);

        /// nullopt if no route matches the path at all (the caller passes the request on).
        std::optional<HttpResponse> dispatch(HttpRequest request) const;

        /// Top-level path prefixes served by this router ("/admin", "/api", ...).
        std::vector<std::string> mounts() const;

    private:
        struct Route
        {
            std::string method; // "*" = any
            std::vector<std::string> segments;
            bool prefix = false;
            Handler handler;
        };
        std::vector<Route> _routes;
    };

    /// Mutating /api requests: JSON content type and same-origin (§10).
    std::optional<HttpResponse> checkMutatingRequest(HttpRequest const& request);

    std::string percentDecode(std::string const& text);
}
