// SPDX-License-Identifier: MIT
#include "group/flow_identity.hpp"

#include "mxlbridge/flowdef.hpp"

namespace mxlgw::group
{
    namespace
    {
        char const* roleOf(config::EssenceType type)
        {
            switch (type)
            {
                case config::EssenceType::Video: return "Video";
                case config::EssenceType::Audio: return "Audio";
                case config::EssenceType::Anc: return "Data";
            }
            return "Video";
        }

        config::EssenceCommon const& commonOf(config::Group const& g, config::EssenceType type, std::size_t index)
        {
            switch (type)
            {
                case config::EssenceType::Video: return g.video.at(index);
                case config::EssenceType::Audio: return g.audio.at(index);
                case config::EssenceType::Anc: return g.anc.at(index);
            }
            return g.video.at(index);
        }
    }

    std::string groupHint(config::Group const& group, config::EssenceType type, std::size_t index)
    {
        return group.label + ":" + roleOf(type) + " " + std::to_string(index + 1);
    }

    ids::EssenceIds essenceIds(config::Group const& group, config::EssenceType type, std::size_t index)
    {
        switch (type)
        {
            case config::EssenceType::Video: return ids::forVideo(group.video.at(index));
            case config::EssenceType::Audio: return ids::forAudio(group.audio.at(index));
            case config::EssenceType::Anc: return ids::forAnc(group.anc.at(index));
        }
        return {};
    }

    nlohmann::json flowDefinition(util::Uuid const& nodeId, config::Group const& group, config::EssenceType type, std::size_t index, std::string const& version)
    {
        auto const& common = commonOf(group, type, index);
        auto const ids = essenceIds(group, type, index);
        mxlbridge::FlowIdentity identity;
        identity.flowId = ids.flow;
        identity.sourceId = ids.source;
        identity.deviceId = ids::deviceId(nodeId);
        identity.label = common.label;
        identity.description = group.label + " " + roleOf(type) + " " + std::to_string(index + 1) + " (MXL)";
        identity.groupHint = groupHint(group, type, index);
        identity.version = version;
        switch (type)
        {
            case config::EssenceType::Video: return mxlbridge::videoFlowDef(identity, group.video.at(index).format);
            case config::EssenceType::Audio: return mxlbridge::audioFlowDef(identity, group.audio.at(index).format);
            case config::EssenceType::Anc: return mxlbridge::ancFlowDef(identity, group.anc.at(index).format);
        }
        return {};
    }
}
