// SPDX-License-Identifier: MIT
#pragma once

#include <string>

#include "config/config.hpp"
#include "util/uuid.hpp"

namespace mxlgw::ids
{
    /// Stable NMOS identifiers (§7.3). Pure functions of the configuration.
    struct EssenceIds
    {
        util::Uuid sender;
        util::Uuid receiver;
        util::Uuid source;
        util::Uuid flow;
    };

    util::Uuid deviceId(util::Uuid const& nodeId);

    /// Flow id = UUIDv5(uid, "flow:" + canonical format): a format change mints a new flow UUID.
    util::Uuid flowId(util::Uuid const& essenceUid, std::string const& canonicalFormat);

    EssenceIds forVideo(config::VideoEssence const& e);
    EssenceIds forAudio(config::AudioEssence const& e);
    EssenceIds forAnc(config::AncEssence const& e);
}
