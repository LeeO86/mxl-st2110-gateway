// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "timing/ptp.hpp"

using namespace mxlgw::timing;

namespace
{
    AnnounceInfo gm(std::uint8_t last, std::uint8_t priority1 = 128, std::uint8_t clockClass = 6)
    {
        AnnounceInfo a;
        a.grandmaster = {0x08, 0x00, 0x11, 0xff, 0xfe, 0x21, 0xe1, last};
        a.priority1 = priority1;
        a.clockClass = clockClass;
        a.clockAccuracy = 0x21;
        a.offsetScaledLogVariance = 0x4e5d;
        a.sender.clock = a.grandmaster;
        a.sender.port = 1;
        return a;
    }

    constexpr std::int64_t sec = 1'000'000'000;
}

TEST_CASE("clock identity formatting")
{
    CHECK(formatClockIdentity(gm(0xb0).grandmaster) == "08-00-11-ff-fe-21-e1-b0");
    CHECK(gm(1).sender.toString() == "08-00-11-ff-fe-21-e1-01/1");
}

TEST_CASE("data set comparison order")
{
    CHECK(compareAnnounce(gm(1), gm(1)) == 0);
    CHECK(compareAnnounce(gm(1, 100), gm(2, 128)) < 0);       // priority1
    CHECK(compareAnnounce(gm(1, 128, 6), gm(2, 128, 7)) < 0); // clockClass
    auto a = gm(1);
    auto b = gm(2);
    a.clockAccuracy = 0x20;
    CHECK(compareAnnounce(a, b) < 0);
    a = gm(1);
    a.offsetScaledLogVariance = 0x1000;
    CHECK(compareAnnounce(a, b) < 0);
    a = gm(1);
    a.priority2 = 10;
    CHECK(compareAnnounce(a, b) < 0);
    CHECK(compareAnnounce(gm(1), gm(2)) < 0); // grandmaster identity tie-break
    CHECK(compareAnnounce(gm(2), gm(1)) > 0);
    // Same GM: fewer steps removed, then sender identity.
    a = gm(1);
    b = gm(1);
    b.stepsRemoved = 2;
    CHECK(compareAnnounce(a, b) < 0);
    b = gm(1);
    b.sender.port = 2;
    CHECK(compareAnnounce(a, b) < 0);
}

TEST_CASE("dual-port selection, timeout and failover")
{
    DualPortSelector sel;
    CHECK_FALSE(sel.selected());
    sel.onAnnounce(0, gm(5), 0, sec);
    CHECK(sel.selected() == 0);
    sel.onAnnounce(1, gm(5), 0, sec); // same parent quality on R: keep P (no flapping)
    CHECK(sel.selected() == 0);
    sel.onAnnounce(1, gm(1, 100), sec, sec); // better GM seen on R
    CHECK(sel.selected() == 1);
    CHECK(sel.selectionChanges() == 2);
    CHECK(sel.grandmasterChanges() == 1);

    // R's parent goes silent for 3 intervals: fall back to P (which keeps announcing).
    sel.onAnnounce(0, gm(5), 3 * sec, sec);
    CHECK_FALSE(sel.tick(4 * sec));
    CHECK(sel.tick(4 * sec + sec + 1));
    CHECK(sel.selected() == 0);
    CHECK_FALSE(sel.parent(1));
    CHECK(sel.parent(0));
    CHECK_FALSE(sel.parent(7));

    // Parent change on the same port (GM failover behind a transparent clock).
    sel.onAnnounce(0, gm(9), 6 * sec, sec); // worse GM, but from a different sender: ignored while parent alive
    CHECK(sel.parent(0)->grandmaster == gm(5).grandmaster);
    CHECK(sel.tick(20 * sec)); // everything expired
    CHECK_FALSE(sel.selected());
    sel.onAnnounce(0, gm(9), 21 * sec, sec);
    CHECK(sel.selected() == 0);
    CHECK(sel.parent(0)->grandmaster == gm(9).grandmaster);
    sel.onAnnounce(5, gm(1), 0, sec); // invalid port ignored
}
