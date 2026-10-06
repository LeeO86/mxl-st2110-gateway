// SPDX-License-Identifier: MIT
#include "group/group_manager.hpp"

#include <algorithm>

#include "util/logging.hpp"

namespace mxlgw::group
{
    namespace
    {
        /// The read offset of every essence of an egress group in ns, by uid (§5.7).
        std::map<util::Uuid, std::int64_t> readOffsetsNs(config::Config const& config, config::Group const& g)
        {
            std::map<util::Uuid, std::int64_t> out;
            for (auto const& e : g.video)
            {
                out[e.uid] = config.effectiveReadOffset(e).toNs(e.format.grainDurationNs());
            }
            for (auto const& e : g.audio)
            {
                out[e.uid] = config.effectiveReadOffset(e).toNs(g.cadenceNs());
            }
            for (auto const& e : g.anc)
            {
                out[e.uid] = config.effectiveReadOffset(e).toNs(e.format.grainDurationNs());
            }
            return out;
        }

        std::int64_t largest(std::map<util::Uuid, std::int64_t> const& offsets)
        {
            std::int64_t m = 0;
            for (auto const& entry : offsets)
            {
                m = std::max(m, entry.second);
            }
            return m;
        }
    }

    std::string groupSignature(config::Group const& group)
    {
        auto j = config::toJson(group);
        for (auto const* list : {"video", "audio", "anc"})
        {
            if (j.contains(list))
            {
                for (auto& e : j[list])
                {
                    e.erase("read_offset_grains");
                    e.erase("read_offset_ns");
                }
            }
        }
        // Network defaults are applied through targets, not by rebuilding the group (§9.3).
        for (auto const* list : {"video", "audio", "anc"})
        {
            if (j.contains(list))
            {
                for (auto& e : j[list])
                {
                    e.erase("defaults");
                }
            }
        }
        return j.dump();
    }

    GroupManager::GroupManager(util::Uuid nodeId, media::MediaBackend& backend, DomainResolver& resolver, std::vector<DomainRuntime> domains,
                               std::string appCpus)
        : _nodeId(nodeId)
        , _backend(backend)
        , _resolver(resolver)
        , _domains(std::move(domains))
        , _appCpus(std::move(appCpus))
    {}

    GroupManager::~GroupManager()
    {
        std::lock_guard const lock{_mutex};
        _groups.clear();
    }

    DomainRuntime const* GroupManager::domainByName(std::string const& name) const
    {
        for (auto const& d : _domains)
        {
            if (d.name == name)
            {
                return &d;
            }
        }
        return nullptr;
    }

    void GroupManager::build(GroupEntry& entry, config::Config const& config)
    {
        auto const& g = entry.config;
        auto const* domain = domainByName(g.domain);
        if (domain == nullptr || !domain->instance)
        {
            log::error("group_domain_missing", {{"group", g.label}, {"domain", g.domain}});
            return;
        }
        if (g.direction == config::Direction::Ingest)
        {
            auto add = [&](config::EssenceType type, std::size_t count)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    IngestSpec spec{_nodeId, g, type, i, _appCpus};
                    entry.ingest.push_back(std::make_unique<IngestEssence>(spec, _backend, domain->instance));
                }
            };
            add(config::EssenceType::Video, g.video.size());
            add(config::EssenceType::Audio, g.audio.size());
            add(config::EssenceType::Anc, g.anc.size());
        }
        else
        {
            EgressSpec spec;
            spec.nodeId = _nodeId;
            spec.group = g;
            spec.appCpus = _appCpus;
            spec.owner = domain->instance;
            spec.readOffsetNs = readOffsetsNs(config, g);
            entry.egress = std::make_unique<EgressGroup>(spec, _backend, _resolver);
        }
        for (auto const& e : g.video)
        {
            _essenceToGroup[e.uid] = g.uid;
        }
        for (auto const& e : g.audio)
        {
            _essenceToGroup[e.uid] = g.uid;
        }
        for (auto const& e : g.anc)
        {
            _essenceToGroup[e.uid] = g.uid;
        }
    }

    void GroupManager::reapplyTargets(GroupEntry& entry)
    {
        for (auto& ie : entry.ingest)
        {
            if (auto const it = _rtpReceivers.find(ie->uid()); it != _rtpReceivers.end())
            {
                ie->setReceiver(it->second);
            }
            if (auto const it = _mxlSenders.find(ie->uid()); it != _mxlSenders.end())
            {
                ie->setSender(it->second);
            }
        }
        if (entry.egress)
        {
            for (auto const& [uid, group] : _essenceToGroup)
            {
                if (group != entry.config.uid)
                {
                    continue;
                }
                if (auto const it = _mxlReceivers.find(uid); it != _mxlReceivers.end())
                {
                    entry.egress->setReceiver(uid, it->second);
                }
                if (auto const it = _rtpSenders.find(uid); it != _rtpSenders.end())
                {
                    entry.egress->setSender(uid, it->second);
                }
            }
        }
    }

    GroupManager::ApplyResult GroupManager::apply(config::Config const& config)
    {
        std::lock_guard const lock{_mutex};
        ApplyResult result;
        std::map<util::Uuid, config::Group const*> wanted;
        for (auto const& g : config.groups)
        {
            if (g.enabled)
            {
                wanted[g.uid] = &g;
            }
        }
        for (auto it = _groups.begin(); it != _groups.end();)
        {
            if (wanted.find(it->first) == wanted.end())
            {
                log::info("group_removed", {{"group", it->second->config.label}, {"uid", it->first.toString()}});
                result.removed.push_back(it->first);
                for (auto e = _essenceToGroup.begin(); e != _essenceToGroup.end();)
                {
                    e = e->second == it->first ? _essenceToGroup.erase(e) : std::next(e);
                }
                it = _groups.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto const& [uid, g] : wanted)
        {
            auto const signature = groupSignature(*g);
            auto it = _groups.find(uid);
            auto const offsets = readOffsetsNs(config, *g);
            // A read offset that changes the default output delay needs a rebuild: the media sessions
            // size their buffers from the output delay (§5.7).
            bool const delayChanged =
                it != _groups.end() && it->second->egress && it->second->egress->outputDelayNs() != g->effectiveOutputDelayNs(largest(offsets));
            if (it != _groups.end() && it->second->signature == signature && !delayChanged)
            {
                // Only read offsets or network defaults changed: applied live (§9.3).
                if (it->second->egress)
                {
                    for (auto const& [essenceUid, ns] : offsets)
                    {
                        it->second->egress->setReadOffset(essenceUid, ns);
                    }
                }
                it->second->config = *g;
                result.updatedLive.push_back(uid);
                continue;
            }
            bool const rebuild = it != _groups.end();
            if (rebuild)
            {
                for (auto e = _essenceToGroup.begin(); e != _essenceToGroup.end();)
                {
                    e = e->second == uid ? _essenceToGroup.erase(e) : std::next(e);
                }
                _groups.erase(it);
            }
            auto entry = std::make_unique<GroupEntry>();
            entry->config = *g;
            entry->signature = signature;
            build(*entry, config);
            reapplyTargets(*entry);
            log::info(rebuild ? "group_rebuilt" : "group_created", {{"group", g->label}, {"uid", uid.toString()}, {"direction", config::toName(g->direction)}});
            (rebuild ? result.rebuilt : result.created).push_back(uid);
            _groups.emplace(uid, std::move(entry));
        }
        return result;
    }

    GroupManager::GroupEntry* GroupManager::groupOfEssence(util::Uuid const& essenceUid)
    {
        auto const it = _essenceToGroup.find(essenceUid);
        if (it == _essenceToGroup.end())
        {
            return nullptr;
        }
        auto const g = _groups.find(it->second);
        return g == _groups.end() ? nullptr : g->second.get();
    }

    IngestEssence* GroupManager::ingestEssence(util::Uuid const& essenceUid)
    {
        if (auto* g = groupOfEssence(essenceUid))
        {
            for (auto& e : g->ingest)
            {
                if (e->uid() == essenceUid)
                {
                    return e.get();
                }
            }
        }
        return nullptr;
    }

    void GroupManager::setRtpReceiver(util::Uuid const& essenceUid, RtpTarget const& target)
    {
        std::lock_guard const lock{_mutex};
        _rtpReceivers[essenceUid] = target;
        if (auto* e = ingestEssence(essenceUid))
        {
            e->setReceiver(target);
        }
    }

    void GroupManager::setMxlSender(util::Uuid const& essenceUid, MxlSenderTarget const& target)
    {
        std::lock_guard const lock{_mutex};
        _mxlSenders[essenceUid] = target;
        if (auto* e = ingestEssence(essenceUid))
        {
            e->setSender(target);
        }
    }

    void GroupManager::setMxlReceiver(util::Uuid const& essenceUid, MxlReceiverTarget const& target)
    {
        std::lock_guard const lock{_mutex};
        _mxlReceivers[essenceUid] = target;
        if (auto* g = groupOfEssence(essenceUid); g != nullptr && g->egress)
        {
            g->egress->setReceiver(essenceUid, target);
        }
    }

    void GroupManager::setRtpSender(util::Uuid const& essenceUid, RtpTarget const& target)
    {
        std::lock_guard const lock{_mutex};
        _rtpSenders[essenceUid] = target;
        if (auto* g = groupOfEssence(essenceUid); g != nullptr && g->egress)
        {
            g->egress->setSender(essenceUid, target);
        }
    }

    std::vector<GroupSnapshot> GroupManager::snapshot() const
    {
        std::lock_guard const lock{_mutex};
        std::vector<GroupSnapshot> out;
        for (auto const& [uid, entry] : _groups)
        {
            if (entry->egress)
            {
                out.push_back(entry->egress->snapshot());
                continue;
            }
            GroupSnapshot g;
            g.uid = uid;
            g.label = entry->config.label;
            g.direction = entry->config.direction;
            g.enabled = entry->config.enabled;
            g.domain = entry->config.domain;
            for (auto const& e : entry->ingest)
            {
                g.essences.push_back(e->snapshot());
            }
            out.push_back(std::move(g));
        }
        return out;
    }
}
