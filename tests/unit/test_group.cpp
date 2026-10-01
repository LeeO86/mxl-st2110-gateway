// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "group/essence_state.hpp"
#include "util/logging.hpp"

using namespace mxlgw;

TEST_CASE("essence state holder logs transitions")
{
    group::StateHolder s({{"essence", "CAM 1 V"}});
    CHECK(s.get().state == group::EssenceState::Idle);
    CHECK(s.set(group::EssenceState::WaitingForFlow, "domain_not_found"));
    CHECK_FALSE(s.set(group::EssenceState::WaitingForFlow, "domain_not_found"));
    auto const line = nlohmann::json::parse(log::recent(1).back());
    CHECK(line["event"] == "essence_state");
    CHECK(line["state"] == "waiting_for_flow");
    CHECK(line["from"] == "idle");
    CHECK(line["reason"] == "domain_not_found");
    CHECK(line["essence"] == "CAM 1 V");
    CHECK(s.set(group::EssenceState::NoSignal));
    CHECK(s.get().reason.empty());
    CHECK(s.set(group::EssenceState::Error, "format_mismatch"));
    CHECK(nlohmann::json::parse(log::recent(1).back())["level"] == "warn");
    for (auto const st : group::allEssenceStates)
    {
        CHECK(std::string(group::toName(st)).size() > 2);
    }
}
