// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "codec/anc8331.hpp"

using namespace mxlgw::codec;

namespace
{
    AncPacket timecode()
    {
        AncPacket p;
        p.line = 9;
        p.hOffset = 0;
        p.did = 0x60;
        p.sdid = 0x60;
        p.udw = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
        return p;
    }
}

TEST_CASE("parity and checksum")
{
    CHECK(withParity(0x00) == 0x200); // even number of ones -> b8 = 0, b9 = 1
    CHECK(withParity(0x01) == 0x101);
    CHECK(withParity(0x60) == 0x260);
    CHECK(withParity(0x61) == 0x161);
    CHECK(parityOk(withParity(0xA5)));
    CHECK_FALSE(parityOk(0x0A5));
    // checksum: 9-bit sum, b9 = !b8
    auto const cs = checksum(withParity(0x41), withParity(0x05), withParity(1), {withParity(0x08)});
    CHECK((cs & 0x1FF) == ((withParity(0x41) & 0x1FF) + (withParity(0x05) & 0x1FF) + (withParity(1) & 0x1FF) + (withParity(0x08) & 0x1FF)) % 512);
    CHECK(((cs >> 9) & 1) == (((cs >> 8) & 1) ^ 1));
}

TEST_CASE("empty frame keeps cadence with ANC_Count = 0")
{
    auto const g = serialiseGrain(AncFrame{});
    REQUIRE(g);
    CHECK(g->size() == ancHeaderBytes);
    CHECK((*g)[0] == 0);
    CHECK((*g)[1] == 0);
    CHECK((*g)[2] == 0);
    auto const r = parseGrain(g->data(), g->size());
    REQUIRE(r.frame);
    CHECK(r.frame->packets.empty());
}

TEST_CASE("golden vector: AFD (DID 0x41 / SDID 0x05)")
{
    AncFrame f;
    f.field = AncField::Field1;
    AncPacket p;
    p.c = false;
    p.line = 11;
    p.hOffset = 0;
    p.did = 0x41;
    p.sdid = 0x05;
    p.udw = {0x08, 0, 0, 0, 0, 0, 0, 0};
    f.packets.push_back(p);
    auto const g = serialiseGrain(f);
    REQUIRE(g);
    // Header: Length = 4 (packet header) + ceil((3 + 8 + 1) * 10 / 8) = 4 + 15 -> word aligned 20 bytes.
    CHECK((((*g)[0] << 8) | (*g)[1]) == 20);
    CHECK((*g)[2] == 1);
    CHECK(((*g)[3] >> 6) == 2);
    // C=0, line 11 (11 bits), hoffset 0 (12 bits), S=0, stream 0 -> 0x00 0xB0 0x00 0x00.
    CHECK((*g)[6] == 0x00);
    CHECK((*g)[7] == 0xB0);
    CHECK((*g)[8] == 0x00);
    CHECK((*g)[9] == 0x00);
    // DID 0x141 (parity b8=0? 0x41 has 2 ones -> b8=0, b9=1 -> 0x241) first 8 bits: 0x241 >> 2 = 0x90.
    CHECK((*g)[10] == 0x90);
    auto const r = parseGrain(g->data(), g->size());
    REQUIRE(r.frame);
    CHECK(r.parityErrors == 0);
    CHECK(r.checksumErrors == 0);
    CHECK(r.frame->field == AncField::Field1);
    REQUIRE(r.frame->packets.size() == 1);
    CHECK(r.frame->packets[0] == p);
}

TEST_CASE("round trip: timecode, CEA-708, several packets, field 2")
{
    AncFrame f;
    f.field = AncField::Field2;
    f.packets.push_back(timecode());
    AncPacket cea;
    cea.c = false;
    cea.line = 10;
    cea.hOffset = 0xABC;
    cea.s = true;
    cea.streamNum = 0x5A;
    cea.did = 0x61;
    cea.sdid = 0x01;
    for (int i = 0; i < 73; ++i)
    {
        cea.udw.push_back(static_cast<std::uint8_t>(i * 3));
    }
    f.packets.push_back(cea);
    AncPacket chroma = timecode();
    chroma.c = true;
    chroma.line = 0x7FF;
    f.packets.push_back(chroma);
    auto const g = serialiseGrain(f);
    REQUIRE(g);
    CHECK(g->size() % 4 == 2); // 6 header bytes + 32-bit aligned packets
    auto const r = parseGrain(g->data(), g->size());
    REQUIRE(r.frame);
    CHECK(r.error.empty());
    CHECK(r.frame->field == AncField::Field2);
    REQUIRE(r.frame->packets.size() == 3);
    CHECK(r.frame->packets[0] == f.packets[0]);
    CHECK(r.frame->packets[1] == f.packets[1]);
    CHECK(r.frame->packets[2] == f.packets[2]);
    // Fits into a padded 4096-byte grain buffer too.
    std::vector<std::uint8_t> grain(4096, 0);
    std::copy(g->begin(), g->end(), grain.begin());
    CHECK(parseGrain(grain.data(), grain.size()).frame->packets.size() == 3);
}

TEST_CASE("rejects truncated and oversized grains, counts corruption")
{
    auto const g = serialiseGrain(AncFrame{AncField::Unspecified, {timecode()}});
    REQUIRE(g);
    CHECK_FALSE(parseGrain(g->data(), 3).frame);
    CHECK_FALSE(parseGrain(nullptr, 0).frame);
    CHECK_FALSE(parseGrain(g->data(), g->size() - 1).frame); // length field exceeds buffer
    auto broken = *g;
    broken[2] = 2; // claims 2 packets
    CHECK_FALSE(parseGrain(broken.data(), broken.size()).frame);
    auto corrupt = *g;
    corrupt[14] ^= 0x01; // flip a user data bit
    auto const r = parseGrain(corrupt.data(), corrupt.size());
    REQUIRE(r.frame);
    CHECK(r.parityErrors + r.checksumErrors > 0);

    AncFrame big;
    AncPacket p;
    p.udw.assign(255, 0x55);
    for (int i = 0; i < 20; ++i)
    {
        big.packets.push_back(p);
    }
    CHECK_FALSE(serialiseGrain(big)); // > 4096 bytes
    AncPacket tooLong;
    tooLong.udw.assign(256, 0);
    CHECK_FALSE(serialiseGrain(AncFrame{AncField::Unspecified, {tooLong}}));
}
