// SPDX-License-Identifier: MIT
#include "util/strings.hpp"

#include <cctype>
#include <charconv>

namespace mxlgw::util
{
    std::vector<std::string> split(std::string_view text, char separator, bool skipEmpty)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= text.size())
        {
            auto const end = text.find(separator, start);
            auto const piece = trim(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (!piece.empty() || !skipEmpty)
            {
                out.push_back(piece);
            }
            if (end == std::string_view::npos)
            {
                break;
            }
            start = end + 1;
        }
        return out;
    }

    std::string trim(std::string_view text)
    {
        auto begin = text.begin();
        auto end = text.end();
        while (begin != end && std::isspace(static_cast<unsigned char>(*begin)))
        {
            ++begin;
        }
        while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))))
        {
            --end;
        }
        return {begin, end};
    }

    std::string toLower(std::string_view text)
    {
        std::string out{text};
        for (auto& c : out)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    }

    std::string toUpperSnake(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        bool lastUnderscore = false;
        for (char const c : text)
        {
            if (std::isalnum(static_cast<unsigned char>(c)))
            {
                out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                lastUnderscore = false;
            }
            else if (!out.empty() && !lastUnderscore)
            {
                out.push_back('_');
                lastUnderscore = true;
            }
        }
        while (!out.empty() && out.back() == '_')
        {
            out.pop_back();
        }
        return out;
    }

    bool startsWith(std::string_view text, std::string_view prefix)
    {
        return text.substr(0, prefix.size()) == prefix;
    }

    bool endsWith(std::string_view text, std::string_view suffix)
    {
        return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
    }

    std::optional<std::int64_t> parseInt(std::string_view text)
    {
        auto const t = trim(text);
        if (t.empty())
        {
            return std::nullopt;
        }
        std::int64_t value = 0;
        auto const* first = t.data();
        auto const* last = t.data() + t.size();
        if (*first == '+')
        {
            ++first;
        }
        auto const [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last)
        {
            return std::nullopt;
        }
        return value;
    }

    std::optional<bool> parseBool(std::string_view text)
    {
        auto const t = toLower(trim(text));
        if (t == "true" || t == "1" || t == "yes" || t == "on")
        {
            return true;
        }
        if (t == "false" || t == "0" || t == "no" || t == "off")
        {
            return false;
        }
        return std::nullopt;
    }

    std::optional<Rational> parseRational(std::string_view text)
    {
        auto const slash = text.find('/');
        if (slash == std::string_view::npos)
        {
            auto const n = parseInt(text);
            if (!n || *n <= 0)
            {
                return std::nullopt;
            }
            return Rational{*n, 1};
        }
        auto const n = parseInt(text.substr(0, slash));
        auto const d = parseInt(text.substr(slash + 1));
        if (!n || !d || *n <= 0 || *d <= 0)
        {
            return std::nullopt;
        }
        return Rational{*n, *d};
    }
}
