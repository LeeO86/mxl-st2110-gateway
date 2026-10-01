// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mxlgw::util
{
    std::vector<std::string> split(std::string_view text, char separator, bool skipEmpty = true);
    std::string trim(std::string_view text);
    std::string toLower(std::string_view text);
    /// "media-p" / "Studio 1" -> "MEDIA_P" / "STUDIO_1" (environment variable names).
    std::string toUpperSnake(std::string_view text);
    bool startsWith(std::string_view text, std::string_view prefix);
    bool endsWith(std::string_view text, std::string_view suffix);

    std::optional<std::int64_t> parseInt(std::string_view text);
    std::optional<bool> parseBool(std::string_view text);

    struct Rational
    {
        std::int64_t num = 0;
        std::int64_t den = 1;

        bool operator==(Rational const& o) const { return num == o.num && den == o.den; }
        bool operator!=(Rational const& o) const { return !(*this == o); }
        double value() const { return den != 0 ? static_cast<double>(num) / static_cast<double>(den) : 0.0; }
        std::string toString() const { return std::to_string(num) + "/" + std::to_string(den); }
    };

    /// "50/1", "30000/1001" or "25".
    std::optional<Rational> parseRational(std::string_view text);
}
