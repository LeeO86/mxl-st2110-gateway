// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "config/schema.hpp"
#include "util/uuid.hpp"

namespace mxlgw::config
{
    /// Body of POST /api/groups (§11.2 "Create group" dialog).
    struct GroupRequest
    {
        std::string label;
        std::string direction = "ingest";
        std::string domain;
        bool redundancy = false;
        int video = 1;
        int audio = 1;
        int anc = 0;
    };

    /// Parses the request; `errors` gets per-field messages (JSON pointers into the body).
    GroupRequest parseGroupRequest(nlohmann::json const& body, ValidationErrors& errors);

    /// A new group with `n` essences per type and the node-wide default profile (1080p50, 8 ch L24 1 ms,
    /// ANC); every group and essence gets a fresh uid. Full essence lists in `body` take precedence.
    nlohmann::ordered_json makeGroup(GroupRequest const& request, nlohmann::json const& body = nlohmann::json::object(),
                                     std::function<util::Uuid()> newUid = util::uuidV4);

    /// Copy of a group with new uids and the label suffixed with " copy"; egress essences lose their
    /// default legs because two egress legs must not share a destination.
    nlohmann::ordered_json duplicateGroup(nlohmann::ordered_json group, std::function<util::Uuid()> newUid = util::uuidV4);
}
