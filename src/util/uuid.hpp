// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mxlgw::util
{
    struct Uuid
    {
        std::array<std::uint8_t, 16> bytes{};

        bool isNil() const;
        std::string toString() const;

        bool operator==(Uuid const& other) const { return bytes == other.bytes; }
        bool operator!=(Uuid const& other) const { return bytes != other.bytes; }
        bool operator<(Uuid const& other) const { return bytes < other.bytes; }
    };

    /// Parses the canonical 8-4-4-4-12 form (case-insensitive).
    std::optional<Uuid> parseUuid(std::string_view text);

    bool isUuid(std::string_view text);

    /// RFC 4122 version 4 (random).
    Uuid uuidV4();

    /// RFC 4122 version 5 (SHA-1 of namespace + name).
    Uuid uuidV5(Uuid const& ns, std::string_view name);
}
