// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "config/config.hpp"
#include "helpers.hpp"
#include "nmos/ids.hpp"

using namespace mxlgw;

TEST_CASE("stable identifiers (§7.3)")
{
    auto const a = config::parseAndValidate(testutil::sampleConfig());
    auto j = testutil::sampleConfig();
    j["groups"][0]["label"] = "RENAMED";
    j["groups"][0]["video"][0]["label"] = "RENAMED V";
    auto const renamed = config::parseAndValidate(j);
    REQUIRE(a.ok());
    REQUIRE(renamed.ok());

    auto const idsA = ids::forVideo(a.config->groups[0].video[0]);
    auto const idsB = ids::forVideo(renamed.config->groups[0].video[0]);
    CHECK(idsA.sender == idsB.sender);
    CHECK(idsA.receiver == idsB.receiver);
    CHECK(idsA.source == idsB.source);
    CHECK(idsA.flow == idsB.flow);
    CHECK(idsA.sender != idsA.receiver);
    CHECK(idsA.flow != idsA.source);

    // Format change: new Flow id only.
    j = testutil::sampleConfig();
    j["groups"][0]["video"][0]["rate"] = "25/1";
    auto const changed = config::parseAndValidate(j);
    REQUIRE(changed.ok());
    auto const idsC = ids::forVideo(changed.config->groups[0].video[0]);
    CHECK(idsC.flow != idsA.flow);
    CHECK(idsC.sender == idsA.sender);
    CHECK(idsC.source == idsA.source);

    // Delete + recreate essence (new uid): all new.
    j = testutil::sampleConfig();
    j["groups"][0]["video"][0]["uid"] = "00000000-0000-4000-8000-000000000999";
    auto const recreated = config::parseAndValidate(j);
    REQUIRE(recreated.ok());
    auto const idsD = ids::forVideo(recreated.config->groups[0].video[0]);
    CHECK(idsD.sender != idsA.sender);
    CHECK(idsD.flow != idsA.flow);

    // Audio flow id ignores the 2110-only bit depth / packet time.
    j = testutil::sampleConfig();
    j["groups"][0]["audio"][0]["bit_depth"] = 16;
    auto const l16 = config::parseAndValidate(j);
    REQUIRE(l16.ok());
    CHECK(ids::forAudio(l16.config->groups[0].audio[0]).flow == ids::forAudio(a.config->groups[0].audio[0]).flow);
    CHECK(ids::forAnc(a.config->groups[0].anc[0]).flow != ids::forAudio(a.config->groups[0].audio[0]).flow);

    auto const node = *a.config->node.id;
    CHECK(ids::deviceId(node) == ids::deviceId(node));
    CHECK(ids::deviceId(node) != node);
}
