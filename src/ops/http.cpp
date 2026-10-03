// SPDX-License-Identifier: MIT
#include "ops/http.hpp"

#include <cctype>
#include <cstdlib>

#include "util/strings.hpp"

namespace mxlgw::ops
{
    namespace
    {
        std::vector<std::string> segmentsOf(std::string const& path)
        {
            return util::split(path, '/');
        }

        std::string hostOfOrigin(std::string const& origin)
        {
            auto const scheme = origin.find("://");
            auto start = scheme == std::string::npos ? 0 : scheme + 3;
            auto const end = origin.find('/', start);
            return util::toLower(origin.substr(start, end == std::string::npos ? std::string::npos : end - start));
        }
    }

    std::optional<std::string> HttpRequest::header(std::string const& name) const
    {
        auto const it = headers.find(util::toLower(name));
        if (it == headers.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::string> HttpRequest::queryParam(std::string const& name) const
    {
        for (auto const& pair : util::split(query, '&'))
        {
            auto const eq = pair.find('=');
            auto const key = percentDecode(pair.substr(0, eq));
            if (key == name)
            {
                return eq == std::string::npos ? std::string() : percentDecode(pair.substr(eq + 1));
            }
        }
        return std::nullopt;
    }

    HttpResponse HttpResponse::json(nlohmann::json const& body, int status)
    {
        HttpResponse r;
        r.status = status;
        r.contentType = "application/json";
        r.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        return r;
    }

    HttpResponse HttpResponse::text(std::string body, int status, std::string contentType)
    {
        HttpResponse r;
        r.status = status;
        r.contentType = std::move(contentType);
        r.body = std::move(body);
        return r;
    }

    HttpResponse HttpResponse::error(int status, std::string const& message, nlohmann::json details)
    {
        nlohmann::json body{{"code", status}, {"error", message}};
        if (!details.is_null())
        {
            body["details"] = std::move(details);
        }
        return json(body, status);
    }

    void Router::add(std::string method, std::string pattern, Handler handler)
    {
        _routes.push_back({std::move(method), segmentsOf(pattern), false, std::move(handler)});
    }

    void Router::addPrefix(std::string method, std::string prefix, Handler handler)
    {
        _routes.push_back({std::move(method), segmentsOf(prefix), true, std::move(handler)});
    }

    std::optional<HttpResponse> Router::dispatch(HttpRequest request) const
    {
        // G10: /api/v1/... is the versioned name of every /api/... route (v1 = this stable contract).
        if (request.path.rfind("/api/v1/", 0) == 0)
        {
            request.path = "/api" + request.path.substr(7);
        }
        auto const segs = segmentsOf(request.path);
        bool pathMatched = false;
        for (auto const& route : _routes)
        {
            if (route.prefix ? segs.size() < route.segments.size() : segs.size() != route.segments.size())
            {
                continue;
            }
            std::map<std::string, std::string> params;
            bool ok = true;
            for (std::size_t i = 0; i < route.segments.size() && ok; ++i)
            {
                auto const& s = route.segments[i];
                if (s.size() > 2 && s.front() == '{' && s.back() == '}')
                {
                    params[s.substr(1, s.size() - 2)] = segs[i];
                }
                else
                {
                    ok = s == segs[i];
                }
            }
            if (!ok)
            {
                continue;
            }
            pathMatched = true;
            auto const method = request.method == "HEAD" ? std::string("GET") : request.method;
            if (route.method != "*" && route.method != method)
            {
                continue;
            }
            request.params = std::move(params);
            return route.handler(request);
        }
        if (pathMatched)
        {
            return HttpResponse::error(405, "method not allowed");
        }
        return std::nullopt;
    }

    std::vector<std::string> Router::mounts() const
    {
        std::vector<std::string> out;
        for (auto const& r : _routes)
        {
            if (r.segments.empty())
            {
                continue;
            }
            auto const m = "/" + r.segments.front();
            bool seen = false;
            for (auto const& o : out)
            {
                seen = seen || o == m;
            }
            if (!seen)
            {
                out.push_back(m);
            }
        }
        return out;
    }

    std::optional<HttpResponse> checkMutatingRequest(HttpRequest const& request)
    {
        auto const ct = util::toLower(request.header("content-type").value_or(""));
        if (ct.rfind("application/json", 0) != 0)
        {
            return HttpResponse::error(415, "Content-Type must be application/json");
        }
        if (auto const origin = request.header("origin"); origin && !origin->empty() && *origin != "null")
        {
            auto const host = util::toLower(request.header("host").value_or(""));
            if (hostOfOrigin(*origin) != host)
            {
                return HttpResponse::error(403, "cross-origin request rejected");
            }
        }
        return std::nullopt;
    }

    std::string percentDecode(std::string const& text)
    {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '+')
            {
                out += ' ';
            }
            else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                     std::isxdigit(static_cast<unsigned char>(text[i + 2])))
            {
                out += static_cast<char>(std::strtol(text.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            }
            else
            {
                out += text[i];
            }
        }
        return out;
    }
}
