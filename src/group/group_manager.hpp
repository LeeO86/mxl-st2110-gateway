// SPDX-License-Identifier: MIT
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "config/config.hpp"
#include "group/domain_resolver.hpp"
#include "group/egress_group.hpp"
#include "group/ingest_essence.hpp"
#include "group/pipeline_types.hpp"
#include "mtl/backend.hpp"
#include "mxlbridge/instance.hpp"

namespace mxlgw::group
{
    /// A configured domain after bootstrap (§8.3).
    struct DomainRuntime
    {
        std::string name;
        std::string path;
        util::Uuid id;
        std::string label;
        std::shared_ptr<mxlbridge::Instance> instance;
    };

    /// Owns all groups and routes IS-05 targets to their essences (control thread only, except
    /// snapshot()). Group edits tear down and rebuild only the affected group (§9.3); the last target
    /// of every essence is kept and re-applied to the rebuilt objects.
    class GroupManager
    {
    public:
        GroupManager(util::Uuid nodeId, media::MediaBackend& backend, DomainResolver& resolver, std::vector<DomainRuntime> domains, std::string appCpus);
        ~GroupManager();

        struct ApplyResult
        {
            std::vector<util::Uuid> created;
            std::vector<util::Uuid> rebuilt;
            std::vector<util::Uuid> removed;
            std::vector<util::Uuid> updatedLive;
        };
        ApplyResult apply(config::Config const& config);

        void setRtpReceiver(util::Uuid const& essenceUid, RtpTarget const& target);
        void setMxlSender(util::Uuid const& essenceUid, MxlSenderTarget const& target);
        void setMxlReceiver(util::Uuid const& essenceUid, MxlReceiverTarget const& target);
        void setRtpSender(util::Uuid const& essenceUid, RtpTarget const& target);

        std::vector<GroupSnapshot> snapshot() const;
        DomainRuntime const* domainByName(std::string const& name) const;
        std::vector<DomainRuntime> const& domains() const { return _domains; }

    private:
        struct GroupEntry
        {
            config::Group config;
            std::string signature; // JSON without read offsets: equal => no rebuild
            std::vector<std::unique_ptr<IngestEssence>> ingest;
            std::unique_ptr<EgressGroup> egress;
        };
        void build(GroupEntry& entry, config::Config const& config);
        void reapplyTargets(GroupEntry& entry);
        GroupEntry* groupOfEssence(util::Uuid const& essenceUid);
        IngestEssence* ingestEssence(util::Uuid const& essenceUid);

        util::Uuid _nodeId;
        media::MediaBackend& _backend;
        DomainResolver& _resolver;
        std::vector<DomainRuntime> _domains;
        std::string _appCpus;
        mutable std::mutex _mutex;
        std::map<util::Uuid, std::unique_ptr<GroupEntry>> _groups;
        std::map<util::Uuid, util::Uuid> _essenceToGroup;
        std::map<util::Uuid, RtpTarget> _rtpReceivers;
        std::map<util::Uuid, MxlSenderTarget> _mxlSenders;
        std::map<util::Uuid, MxlReceiverTarget> _mxlReceivers;
        std::map<util::Uuid, RtpTarget> _rtpSenders;
    };

    /// Group JSON without the live-editable read offsets (§9.3).
    std::string groupSignature(config::Group const& group);
}
