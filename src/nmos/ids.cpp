// SPDX-License-Identifier: MIT
#include "nmos/ids.hpp"

namespace mxlgw::ids
{
    namespace
    {
        EssenceIds make(util::Uuid const& uid, std::string const& canonical)
        {
            EssenceIds ids;
            ids.sender = util::uuidV5(uid, "sender");
            ids.receiver = util::uuidV5(uid, "receiver");
            ids.source = util::uuidV5(uid, "source");
            ids.flow = flowId(uid, canonical);
            return ids;
        }
    }

    util::Uuid deviceId(util::Uuid const& nodeId)
    {
        return util::uuidV5(nodeId, "device");
    }

    util::Uuid flowId(util::Uuid const& essenceUid, std::string const& canonicalFormat)
    {
        return util::uuidV5(essenceUid, "flow:" + canonicalFormat);
    }

    EssenceIds forVideo(config::VideoEssence const& e)
    {
        return make(e.idNamespace, e.format.canonical());
    }

    EssenceIds forAudio(config::AudioEssence const& e)
    {
        return make(e.idNamespace, e.format.canonical());
    }

    EssenceIds forAnc(config::AncEssence const& e)
    {
        return make(e.idNamespace, e.format.canonical());
    }
}
