// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mxlgw::codec
{
    /// One SMPTE ST 291 ANC packet as carried by RFC 8331 (user data words are 8-bit values;
    /// bits 8-9 are parity, regenerated on serialisation — §6.3 accepted limitation).
    struct AncPacket
    {
        bool c = false;             // colour-difference (chroma) channel
        std::uint16_t line = 0;     // 11 bits
        std::uint16_t hOffset = 0;  // 12 bits
        bool s = false;             // StreamNum valid
        std::uint8_t streamNum = 0; // 7 bits
        std::uint8_t did = 0;
        std::uint8_t sdid = 0;
        std::vector<std::uint8_t> udw; // at most 255

        bool operator==(AncPacket const& o) const;
    };

    /// RFC 8331 F field: 0b00 progressive / not specified, 0b10 field 1, 0b11 field 2.
    enum class AncField : std::uint8_t
    {
        Unspecified = 0,
        Field1 = 2,
        Field2 = 3,
    };

    struct AncFrame
    {
        AncField field = AncField::Unspecified;
        std::vector<AncPacket> packets;
    };

    /// 10-bit word with even parity in b8 and !b8 in b9 (ST 291).
    std::uint16_t withParity(std::uint8_t value);
    bool parityOk(std::uint16_t word);

    /// ST 291 checksum over DID, SDID, DC and UDW 10-bit words (9-bit sum, b9 = !b8).
    std::uint16_t checksum(std::uint16_t did10, std::uint16_t sdid10, std::uint16_t dc10, std::vector<std::uint16_t> const& udw10);

    /// Serialises to the MXL `video/smpte291` grain body: the RFC 8331 payload starting at the
    /// Length field (§6.3, MXL docs/Architecture.md). Returns nullopt if it exceeds `capacity`.
    std::optional<std::vector<std::uint8_t>> serialiseGrain(AncFrame const& frame, std::size_t capacity = 4096);

    struct AncParseResult
    {
        std::optional<AncFrame> frame;
        std::string error; // set if the grain is malformed
        std::size_t parityErrors = 0;
        std::size_t checksumErrors = 0;
    };

    /// Parses a grain body (as written by serialiseGrain or any MXL writer). Never reads past `size`.
    AncParseResult parseGrain(std::uint8_t const* data, std::size_t size);

    /// Bytes an empty frame occupies (Length, ANC_Count, F, reserved).
    inline constexpr std::size_t ancHeaderBytes = 6;
}
