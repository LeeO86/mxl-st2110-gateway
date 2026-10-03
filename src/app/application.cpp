// SPDX-License-Identifier: MIT
#include "app/application.hpp"

#include <filesystem>
#include <iostream>

#include "app/targets.hpp"
#include "mtl/backend_factory.hpp"
#include "mxlbridge/domaindef.hpp"
#include "mxlbridge/flowdef.hpp"
#include "mxlbridge/instance.hpp"
#include "mxlbridge/reader.hpp"
#include "nmos/ids.hpp"
#include "ops/health.hpp"
#include "ops/preflight.hpp"
#include "ops/webapi.hpp"
#include "ops/webui_embedded.hpp"
#include "timing/ptp.hpp"
#include "util/fs.hpp"
#include "util/logging.hpp"
#include "util/threading.hpp"
#include "version.hpp"

namespace mxlgw::app
{
    namespace
    {
        using json = nlohmann::json;

        std::string stateFileFor(std::string const& configPath)
        {
            return (std::filesystem::path(configPath).parent_path() / "state" / "connections.json").string();
        }

        json essenceJson(group::EssenceSnapshot const& e)
        {
            json j;
            j["uid"] = e.uid.toString();
            j["label"] = e.label;
            j["type"] = config::toName(e.type);
            j["direction"] = config::toName(e.direction);
            j["state"] = group::toName(e.state.state);
            j["reason"] = e.state.reason;
            j["flow_id"] = e.flowId.isNil() ? json() : json(e.flowId.toString());
            if (e.direction == config::Direction::Ingest)
            {
                j["receiver_active"] = e.receiverActive;
                j["sender_active"] = e.senderActive;
                j["frames"] = {{"complete", e.rx.framesComplete}, {"incomplete", e.rx.framesIncomplete}, {"dropped", e.rx.framesDropped + e.framesDropped}};
                j["packets"] = e.rx.packets;
                j["legs"] = json::array(
                    {{{"packets", e.rx.legs[0].packets}, {"lost", e.rx.legs[0].lost}}, {{"packets", e.rx.legs[1].packets}, {"lost", e.rx.legs[1].lost}}});
                j["grains_written"] = e.grainsWritten;
                j["samples_written"] = e.samplesWritten;
                j["write_errors"] = e.writeErrors;
                j["writer_resyncs"] = e.writerResyncs;
                j["origin_age_ns"] = e.originAgeNs ? json(*e.originAgeNs) : json();
            }
            else
            {
                j["mxl_receiver_active"] = e.mxlReceiverActive;
                j["rtp_sender_active"] = e.rtpSenderActive;
                j["frames_sent"] = e.tx.framesComplete;
                j["late_frames"] = e.tx.framesLate + e.txDropped;
                j["packets"] = e.tx.packets;
                j["grains_read"] = e.grainsRead;
                j["read_timeouts"] = e.readTimeouts;
                j["late_reads"] = e.lateReads;
                j["grains_invalid"] = e.grainsInvalid;
                j["flow_not_found"] = e.flowNotFound;
                j["read_lag_grains"] = e.readLagGrains ? json(*e.readLagGrains) : json();
                j["lead_ns"] = e.leadNs ? json(*e.leadNs) : json();
                j["read_offset_ns"] = e.readOffsetNs;
                if (e.reader)
                {
                    j["resolved_domain"] = {{"id", e.reader->domainId.toString()},
                                            {"path", e.reader->domainPath},
                                            {"kind", e.reader->domainKind},
                                            {"flow_id", e.reader->flowId.toString()}};
                }
                else
                {
                    j["resolved_domain"] = nullptr;
                }
            }
            return j;
        }

        json domainEntryJson(mxlbridge::DomainEntry const& d)
        {
            json j{{"id", d.id.toString()},
                   {"path", d.path},
                   {"label", d.label},
                   {"description", d.description},
                   {"kind", mxlbridge::toName(d.kind)},
                   {"fs_type", d.fsType},
                   {"tmpfs", d.tmpfs},
                   {"total_bytes", d.totalBytes},
                   {"free_bytes", d.freeBytes},
                   {"flows", d.flowCount}};
            if (!d.configuredName.empty())
            {
                j["name"] = d.configuredName;
            }
            if (d.kind == mxlbridge::DomainKind::Mirror)
            {
                j["source_host_id"] = d.sourceHostId;
                j["owner_host_id"] = d.ownerHostId;
            }
            return j;
        }
    }

    Application::Application(Options options)
        : _options(std::move(options))
    {}

    Application::~Application()
    {
        shutdown();
    }

    bool Application::restartRequired() const
    {
        std::lock_guard const lock{_restartMutex};
        return !_restartReasons.empty();
    }

    std::vector<std::string> Application::restartReasons() const
    {
        std::lock_guard const lock{_restartMutex};
        return {_restartReasons.begin(), _restartReasons.end()};
    }

    void Application::markRestartRequired(std::string const& reason)
    {
        std::lock_guard const lock{_restartMutex};
        if (_restartReasons.insert(reason).second)
        {
            log::info("restart_required", {{"reason", reason}});
        }
    }

    int Application::bootstrapDomains(config::Config const& cfg)
    {
        auto const snap = _store->snapshot();
        for (std::size_t i = 0; i < cfg.mxl.domains.size(); ++i)
        {
            auto const& d = cfg.mxl.domains[i];
            auto const pointer = "/mxl/domains/" + std::to_string(i) + "/id";
            mxlbridge::BootstrapOptions opts;
            opts.idFromEnvironment = snap.overlay.variableFor(pointer).has_value();
            try
            {
                auto result = mxlbridge::bootstrapDomain(d, opts);
                if (result.writeBackId)
                {
                    // Owner decision C6: the adopted/generated id is written back so a tmpfs wipe re-creates it.
                    _store->writeBack(pointer, result.writeBackId->toString());
                }
                auto instance = _instances.pin(result.path);
                // Owner decision Q9: only our own stale flows, unless gc_on_start.
                std::vector<util::Uuid> own;
                for (auto const& g : cfg.groups)
                {
                    if (g.direction != config::Direction::Ingest || g.domain != d.name)
                    {
                        continue;
                    }
                    for (auto const& e : g.video)
                    {
                        own.push_back(ids::forVideo(e).flow);
                    }
                    for (auto const& e : g.audio)
                    {
                        own.push_back(ids::forAudio(e).flow);
                    }
                    for (auto const& e : g.anc)
                    {
                        own.push_back(ids::forAnc(e).flow);
                    }
                }
                mxlbridge::removeStaleFlows(result.path, own);
                if (d.gcOnStart)
                {
                    instance->garbageCollectAll();
                }
                _domains.push_back({d.name, result.path, result.id, result.label, instance});
                log::info("mxl_domain_ready", {{"domain", d.name},
                                               {"path", result.path},
                                               {"id", result.id.toString()},
                                               {"fs_type", result.fsType},
                                               {"created", result.domainDefCreated}});
                _bootstrap.push_back(std::move(result));
            }
            catch (mxlbridge::BootstrapError const& ex)
            {
                log::error(ex.event(), ex.fields());
                std::cerr << "mxl-st2110-gateway: " << ex.what() << "\n";
                return exitConfig;
            }
            catch (std::exception const& ex)
            {
                log::error("mxl_domain_open_failed", {{"domain", d.name}, {"path", d.path}, {"error", ex.what()}});
                return exitConfig;
            }
        }
        return exitOk;
    }

    int Application::start()
    {
        auto const format = _options.env("MXLGW_LOG_FORMAT").value_or("json") == "text" ? log::Format::Text : log::Format::Json;
        log::configure(log::Level::Info, format);
        log::startDrain();

        _store = std::make_unique<config::ConfigStore>(_options.configPath, _options.env);
        try
        {
            auto const loaded = _store->load();
            if (loaded == config::ConfigStore::LoadResult::CreatedMinimal)
            {
                log::info("config_created_minimal", {{"path", _options.configPath}});
            }
        }
        catch (config::ConfigError const& ex)
        {
            std::cerr << "mxl-st2110-gateway: invalid configuration " << _options.configPath << ":\n" << config::formatErrors(ex.errors()) << "\n";
            log::error("config_invalid", {{"path", _options.configPath}, {"errors", config::formatErrors(ex.errors())}});
            return exitConfig;
        }
        catch (std::exception const& ex)
        {
            std::cerr << "mxl-st2110-gateway: cannot load " << _options.configPath << ": " << ex.what() << "\n";
            return exitConfig;
        }
        auto cfg = _store->snapshot().config;
        if (auto const level = log::parseLevel(cfg.node.logLevel))
        {
            log::setLevel(*level);
        }
        _setupMode = cfg.unconfigured();
        log::info("gateway_starting", {{"version", version::gateway},
                                       {"mtl", version::mtl},
                                       {"dpdk", version::dpdk},
                                       {"mxl", mxlbridge::mxlVersionString()},
                                       {"nmos_cpp", version::nmosCpp},
                                       {"config", _options.configPath},
                                       {"setup_mode", _setupMode}});

        if (!_setupMode)
        {
            auto const checks = ops::runPreflight(cfg);
            for (auto const& c : checks)
            {
                if (c.level == ops::CheckLevel::Fail)
                {
                    log::error("preflight_failed", {{"check", c.id}, {"message", c.message}, {"anchor", c.anchor}});
                }
                else if (c.level == ops::CheckLevel::Warn)
                {
                    log::warn("preflight_warning", {{"check", c.id}, {"message", c.message}, {"anchor", c.anchor}});
                }
            }
            if (_options.preflightOnly)
            {
                std::cout << ops::toJson(checks).dump(2) << "\n";
                return ops::hasFailures(checks) ? exitConfig : exitOk;
            }
            if (ops::hasFailures(checks))
            {
                return exitConfig;
            }
            if (auto const rc = bootstrapDomains(cfg); rc != exitOk)
            {
                return rc;
            }
            cfg = _store->snapshot().config; // ids written back
        }
        else if (_options.preflightOnly)
        {
            std::cout << ops::toJson(ops::runPreflight(cfg)).dump(2) << "\n";
            return exitOk;
        }

        std::vector<mxlbridge::ConfiguredDomainRef> refs;
        for (auto const& d : _domains)
        {
            refs.push_back({d.name, d.path});
        }
        _directory = std::make_unique<mxlbridge::DomainDirectory>(cfg.mxl.scanPath, refs);
        _directory->rescan();
        _resolver = std::make_unique<group::DomainResolver>(*_directory, _instances);
        _control = std::make_unique<ControlQueue>();
        _connections = std::make_unique<ConnectionState>(stateFileFor(_options.configPath));
        if (cfg.node.resumeConnections)
        {
            _connections->load();
        }
        refreshDomainUsage();

        if (!_setupMode)
        {
            try
            {
                _backend = media::createBackend(cfg);
            }
            catch (std::exception const& ex)
            {
                log::error("media_backend_failed", {{"backend", config::toName(cfg.nic.backend)}, {"error", ex.what()}});
                return exitRuntime;
            }
            auto const st = _backend->status();
            if (st.testBackend)
            {
                log::warn("test_backend", {{"backend", st.backend}, {"details", "no pacing guarantees and no hardware PTP (test-only, §17.2)"}});
            }
            _clock = std::make_unique<ops::ClockSupervisor>([this] { return _backend->ptpTimeNs(); }, [] { return media::hostTaiNs(); });
            _clock->start();
            _groups = std::make_unique<group::GroupManager>(cfg.mxlNodeId(), *_backend, *_resolver, _domains, cfg.nic.appCpus);
        }

        _router = ops::makeGatewayRouter(*this);

        if (_options.withNmos)
        {
            try
            {
                if (_setupMode)
                {
                    _http = nmosnode::createHttpServer(cfg.node.httpPort, cfg.node.tls, _router);
                    _http->start();
                    log::info("setup_mode", {{"http_port", cfg.node.httpPort}, {"details", "NIC not configured: only the admin UI is served"}});
                }
                else
                {
                    nmosnode::Setup setup;
                    setup.config = cfg;
                    setup.interfaces = interfaces();
                    for (auto const& d : _domains)
                    {
                        setup.domains.push_back({d.name, d.path, d.id});
                    }
                    setup.clock = clockInfo();
                    setup.connections = cfg.node.resumeConnections ? _connections.get() : nullptr;
                    setup.routes = &_router;
                    setup.gatewayVersion = version::gateway;
                    nmosnode::Callbacks callbacks;
                    callbacks.activated = [this](nmosnode::Activation const& a) { onActivation(a); };
                    callbacks.resolveMxlDomain = [this](util::Uuid const& groupDomain, std::optional<util::Uuid> const& flow) -> std::optional<util::Uuid>
                    {
                        if (auto const d = _resolver->resolveAuto(groupDomain, flow))
                        {
                            return d->id;
                        }
                        return std::nullopt;
                    };
                    callbacks.checkMxlFlow = [this](util::Uuid const& essence, std::optional<util::Uuid> const& domain, util::Uuid const& flow)
                    { return checkMxlFlow(essence, domain, flow); };
                    callbacks.domainAccessible = [this](util::Uuid const& id) { return _directory->findById(id).has_value(); };
                    _node = nmosnode::createNode(std::move(setup), std::move(callbacks));
                    _groups->apply(cfg);
                    _node->start();
                }
            }
            catch (std::exception const& ex)
            {
                log::error("http_server_failed", {{"port", cfg.node.httpPort}, {"error", ex.what()}});
                return exitRuntime;
            }
        }
        else if (_groups)
        {
            _groups->apply(cfg);
        }

        _housekeeping = std::thread([this] { housekeeping(); });
        _started = true;
        log::info("gateway_started", {{"http_port", cfg.node.httpPort}, {"setup_mode", _setupMode}});
        return exitOk;
    }

    int Application::run()
    {
        auto const rc = start();
        if (rc != exitOk)
        {
            shutdown();
            return rc;
        }
        {
            std::unique_lock lock{_stopMutex};
            _stopCv.wait(lock, [&] { return _stopRequested.load(); });
        }
        shutdown();
        return _exitCode.load();
    }

    void Application::stop(int exitCode)
    {
        _exitCode.store(exitCode);
        {
            std::lock_guard const lock{_stopMutex};
            _stopRequested.store(true);
        }
        _stopCv.notify_all();
    }

    void Application::shutdown()
    {
        if (!_stopRequested.exchange(true))
        {
            _stopCv.notify_all();
        }
        if (_housekeeping.joinable())
        {
            _housekeeping.join();
        }
        // §8.4: stop media, release MXL writers/readers, destroy instances, then MTL.
        if (_node)
        {
            _node->stop();
            _node.reset();
        }
        if (_http)
        {
            _http->stop();
            _http.reset();
        }
        if (_control)
        {
            _control->stop();
        }
        _groups.reset();
        if (_clock)
        {
            _clock->stop();
        }
        _resolver.reset();
        _domains.clear();
        _instances.clear();
        _backend.reset();
        if (_started)
        {
            log::info("gateway_stopped", {{"exit_code", _exitCode.load()}});
            _started = false;
        }
        log::drainNow();
    }

    void Application::countActivation(std::string const& kind, std::string const& transport, std::string const& result)
    {
        std::lock_guard const lock{_cacheMutex};
        ++_activations[{kind, transport, result}];
    }

    void Application::onActivation(nmosnode::Activation const& a)
    {
        // nmos-cpp activation thread: hand over to the single control thread (§3.6).
        _control->post(
            [this, a]
            {
                auto const cfg = _store->snapshot().config;
                if (cfg.node.resumeConnections && _connections)
                {
                    _connections->save(a.resourceId, a.sender ? "sender" : "receiver", a.active);
                }
                config::Group const* group = nullptr;
                for (auto const& g : cfg.groups)
                {
                    auto has = [&](auto const& list)
                    {
                        for (auto const& e : list)
                        {
                            if (e.uid == a.essenceUid)
                            {
                                return true;
                            }
                        }
                        return false;
                    };
                    if (has(g.video) || has(g.audio) || has(g.anc))
                    {
                        group = &g;
                    }
                }
                auto const kind = a.sender ? "sender" : "receiver";
                if (group == nullptr || !_groups)
                {
                    countActivation(kind, a.transport, "unknown_resource");
                    return;
                }
                if (group->direction == config::Direction::Ingest)
                {
                    if (a.sender)
                    {
                        _groups->setMxlSender(a.essenceUid, mxlSenderTarget(a.active));
                    }
                    else
                    {
                        _groups->setRtpReceiver(a.essenceUid, rtpReceiverTarget(a.active));
                    }
                }
                else if (a.sender)
                {
                    _groups->setRtpSender(a.essenceUid, rtpSenderTarget(a.active));
                }
                else
                {
                    _groups->setMxlReceiver(a.essenceUid, mxlReceiverTarget(a.active));
                }
                countActivation(kind, a.transport, "ok");
                log::info("nmos_activation", {{"group", group->label},
                                              {"essence_uid", a.essenceUid.toString()},
                                              {"resource_id", a.resourceId.toString()},
                                              {"kind", kind},
                                              {"transport", a.transport},
                                              {"master_enable", a.active.value("master_enable", false)}});
            });
    }

    std::string Application::checkMxlFlow(util::Uuid const& essenceUid, std::optional<util::Uuid> const& domainId, util::Uuid const& flowId)
    {
        auto const cfg = _store->snapshot().config;
        std::optional<mxlbridge::DomainEntry> domain;
        if (domainId)
        {
            domain = _directory->findById(*domainId);
        }
        if (!domain || !mxlbridge::flowDirExists(domain->path, flowId))
        {
            return {}; // not-yet-existing flows are accepted (§6.4); the essence waits
        }
        auto const text = util::readFile(mxlbridge::flowDirPath(domain->path, flowId) + "/flow_def.json");
        if (!text)
        {
            return {};
        }
        nlohmann::json def;
        try
        {
            def = nlohmann::json::parse(*text);
        }
        catch (std::exception const&)
        {
            return "flow_def.json of flow " + flowId.toString() + " is not valid JSON";
        }
        for (auto const& g : cfg.groups)
        {
            for (auto const& e : g.video)
            {
                if (e.uid == essenceUid)
                {
                    auto const m = mxlbridge::compareVideo(def, e.format);
                    return m.empty() ? std::string() : m.front();
                }
            }
            for (auto const& e : g.audio)
            {
                if (e.uid == essenceUid)
                {
                    auto const m = mxlbridge::compareAudio(def, e.format);
                    return m.empty() ? std::string() : m.front();
                }
            }
            for (auto const& e : g.anc)
            {
                if (e.uid == essenceUid)
                {
                    auto const m = mxlbridge::compareAnc(def, e.format);
                    return m.empty() ? std::string() : m.front();
                }
            }
        }
        return {};
    }

    nlohmann::json Application::applyGroups()
    {
        if (!_control || !_groups)
        {
            return json::array();
        }
        return _control->call(
            [this]
            {
                auto const cfg = _store->snapshot().config;
                json result;
                // NMOS first: a new flow UUID must be in the model before the new writer commits (§7.3).
                if (_node)
                {
                    result["nmos"] = _node->applyGroups(cfg);
                }
                auto const applied = _groups->apply(cfg);
                auto ids = [](std::vector<util::Uuid> const& v)
                {
                    json a = json::array();
                    for (auto const& u : v)
                    {
                        a.push_back(u.toString());
                    }
                    return a;
                };
                result["created"] = ids(applied.created);
                result["rebuilt"] = ids(applied.rebuilt);
                result["removed"] = ids(applied.removed);
                result["updated_live"] = ids(applied.updatedLive);
                return result;
            });
    }

    std::vector<nmosnode::InterfaceInfo> Application::interfaces() const
    {
        std::vector<nmosnode::InterfaceInfo> out;
        auto const cfg = _store->snapshot().config;
        if (_backend)
        {
            auto const st = _backend->status();
            std::string chassis = st.ports.empty() ? std::string() : st.ports.front().mac;
            for (auto const& p : st.ports)
            {
                out.push_back({p.name, p.mac, chassis, {p.ip}});
            }
        }
        return out;
    }

    nmosnode::ClockInfo Application::clockInfo() const
    {
        nmosnode::ClockInfo c;
        auto const cfg = _store->snapshot().config;
        c.domain = cfg.ptp.domain;
        if (!_backend)
        {
            return c;
        }
        auto const st = _backend->status();
        if (!st.ports.empty())
        {
            c.localMac = st.ports.front().mac;
        }
        if (st.ptpAvailable)
        {
            for (auto const& p : st.ptp)
            {
                if (p.selected && p.announce)
                {
                    c.ptp = true;
                    c.locked = p.locked;
                    c.traceable = p.announce->clockClass <= 7;
                    c.gmid = timing::formatClockIdentity(p.announce->grandmaster);
                    c.domain = p.domain;
                }
            }
        }
        return c;
    }

    void Application::refreshDomainUsage()
    {
        std::vector<ops::ConfiguredDomainUsage> usage;
        bool ok = true;
        for (auto const& d : _domains)
        {
            auto const fs = util::inspectFs(d.path);
            ops::ConfiguredDomainUsage u;
            u.name = d.name;
            u.usedBytes = fs.totalBytes > fs.freeBytes ? fs.totalBytes - fs.freeBytes : 0;
            u.freeBytes = fs.freeBytes;
            u.flows = mxlbridge::listFlowDirs(d.path, false).size();
            usage.push_back(u);
            ok = ok && fs.exists && fs.tmpfs && std::filesystem::exists(mxlbridge::domainDefPath(d.path));
        }
        std::lock_guard const lock{_cacheMutex};
        _domainUsage = std::move(usage);
        _domainsOk = ok;
    }

    void Application::housekeeping()
    {
        util::setThreadName("housekeeping");
        std::map<std::string, std::string> known; // id -> path
        log::RateLimiter skippedLog{std::chrono::minutes(5)};
        bool diskBanner = false;
        while (!_stopRequested.load())
        {
            if (_directory)
            {
                auto const scan = _directory->rescan();
                std::map<std::string, std::string> now;
                for (auto const& d : scan.domains)
                {
                    now[d.id.toString() + "@" + d.path] = mxlbridge::toName(d.kind);
                    if (known.find(d.id.toString() + "@" + d.path) == known.end() && d.kind != mxlbridge::DomainKind::Configured)
                    {
                        log::info("mxl_domain_discovered", {{"id", d.id.toString()}, {"path", d.path}, {"kind", mxlbridge::toName(d.kind)}});
                    }
                }
                for (auto const& [key, kind] : known)
                {
                    if (now.find(key) == now.end())
                    {
                        log::info("mxl_domain_removed", {{"domain", key}, {"kind", kind}});
                    }
                }
                for (auto const& c : scan.conflicts)
                {
                    if (skippedLog.allow("conflict:" + c.path))
                    {
                        log::warn("mxl_domain_conflict", {{"id", c.id.toString()}, {"path", c.path}});
                    }
                }
                for (auto const& s : scan.skipped)
                {
                    if (skippedLog.allow("skipped:" + s.path))
                    {
                        log::warn("mxl_domain_skipped", {{"path", s.path}, {"reason", s.reason}});
                    }
                }
                known = std::move(now);
            }
            refreshDomainUsage();
            if (_store && _store->changedOnDisk() && !diskBanner)
            {
                diskBanner = true;
                log::warn("config_changed_on_disk", {{"path", _store->path()}, {"details", "restart required; UI saves are blocked until resolved"}});
            }
            if (_node && _backend)
            {
                auto const c = clockInfo();
                if (c.gmid != _lastGmid)
                {
                    if (!_lastGmid.empty())
                    {
                        log::warn("ptp_gm_changed", {{"from", _lastGmid}, {"to", c.gmid}});
                    }
                    _lastGmid = c.gmid;
                    _node->updateClock(c);
                }
            }
            std::unique_lock lock{_stopMutex};
            _stopCv.wait_for(lock, std::chrono::seconds(2), [&] { return _stopRequested.load(); });
        }
    }

    nlohmann::json Application::backendJson() const
    {
        if (!_backend)
        {
            return nullptr;
        }
        auto const st = _backend->status();
        json ports = json::array();
        for (std::size_t i = 0; i < st.ports.size(); ++i)
        {
            auto const& p = st.ports[i];
            ports.push_back({{"leg", ops::portLabel(i)},
                             {"name", p.name},
                             {"pci", p.pci},
                             {"ifname", p.ifname},
                             {"mac", p.mac},
                             {"ip", p.ip},
                             {"link_up", p.linkUp},
                             {"link_speed_mbps", p.linkSpeedMbps},
                             {"bind_mode", p.bindMode},
                             {"driver", p.driver},
                             {"ddp_package", p.ddpPackage},
                             {"rx_packets", p.rxPackets},
                             {"tx_packets", p.txPackets},
                             {"rx_bytes", p.rxBytes},
                             {"tx_bytes", p.txBytes},
                             {"rx_errors", p.rxErrors},
                             {"rx_missed", p.rxMissed}});
        }
        return {{"backend", st.backend}, {"test_backend", st.testBackend}, {"mtl_version", st.mtlVersion}, {"ports", ports}};
    }

    nlohmann::json Application::nic()
    {
        auto j = backendJson();
        if (j.is_null())
        {
            j = json::object();
        }
        j["config"] = _store->snapshot().overlay.effective.value("nic", json::object());
        return j;
    }

    nlohmann::json Application::ptp()
    {
        auto const cfg = _store->snapshot().config;
        json j;
        j["mode"] = config::toName(cfg.ptp.mode);
        j["domain"] = cfg.ptp.domain;
        j["require_lock"] = cfg.ptp.requireLock;
        j["warn_offset_ns"] = cfg.ptp.warnOffsetNs;
        j["max_offset_ns"] = cfg.ptp.maxOffsetNs;
        j["kernel_tai_offset_s"] = ops::kernelTaiOffset();
        if (_clock)
        {
            auto const c = _clock->latest();
            j["clock"] = c.valid ? json{{"mtl_minus_host_tai_ns", c.offsetNs}, {"min_60s_ns", c.min60Ns}, {"max_60s_ns", c.max60Ns}} : json();
        }
        json ports = json::array();
        if (_backend)
        {
            auto const st = _backend->status();
            j["available"] = st.ptpAvailable;
            j["selection_changes"] = st.ptpSelectionChanges;
            j["phc2sys_locked"] = st.phc2sysLocked;
            // ptp.mode = external / kernel backend: no MTL PTP instance, so no per-port state to show.
            for (std::size_t i = 0; st.ptpAvailable && i < st.ports.size() && i < st.ptp.size(); ++i)
            {
                auto const& p = st.ptp[i];
                json pj{{"leg", ops::portLabel(i)},
                        {"active", p.active},
                        {"locked", p.locked},
                        {"selected", p.selected},
                        {"domain", p.domain},
                        {"utc_offset", p.utcOffset},
                        {"offset_ns", {{"last", p.lastDeltaNs}, {"min", p.minDeltaNs}, {"max", p.maxDeltaNs}, {"avg", p.avgDeltaNs}}},
                        {"path_delay_ns", {{"last", p.lastPathDelayNs}, {"min", p.minPathDelayNs}, {"max", p.maxPathDelayNs}, {"avg", p.avgPathDelayNs}}},
                        {"sync_count", p.syncCount},
                        {"gm_changes", p.gmChanges}};
                pj["parent"] = p.parent ? json(p.parent->toString()) : json();
                if (p.announce)
                {
                    pj["grandmaster"] = {{"identity", timing::formatClockIdentity(p.announce->grandmaster)},
                                         {"priority1", p.announce->priority1},
                                         {"priority2", p.announce->priority2},
                                         {"clock_class", p.announce->clockClass},
                                         {"clock_accuracy", p.announce->clockAccuracy},
                                         {"offset_scaled_log_variance", p.announce->offsetScaledLogVariance},
                                         {"steps_removed", p.announce->stepsRemoved},
                                         {"time_source", p.announce->timeSource}};
                }
                ports.push_back(pj);
            }
        }
        j["ports"] = ports;
        return j;
    }

    nlohmann::json Application::domains()
    {
        json j;
        json configured = json::array();
        for (auto const& b : _bootstrap)
        {
            auto const fs = util::inspectFs(b.path);
            configured.push_back({{"name", b.name},
                                  {"path", b.path},
                                  {"id", b.id.toString()},
                                  {"label", b.label},
                                  {"kind", "configured"},
                                  {"fs_type", fs.typeName},
                                  {"tmpfs", fs.tmpfs},
                                  {"total_bytes", fs.totalBytes},
                                  {"free_bytes", fs.freeBytes},
                                  {"flows", mxlbridge::listFlowDirs(b.path, false).size()}});
        }
        j["configured"] = configured;
        json accessible = json::array();
        json conflicts = json::array();
        json skipped = json::array();
        if (_directory)
        {
            auto const scan = _directory->last();
            for (auto const& d : scan.domains)
            {
                accessible.push_back(domainEntryJson(d));
            }
            for (auto const& d : scan.conflicts)
            {
                conflicts.push_back(domainEntryJson(d));
            }
            for (auto const& s : scan.skipped)
            {
                skipped.push_back({{"path", s.path}, {"reason", s.reason}});
            }
            j["scan_path"] = _directory->scanPath() ? json(*_directory->scanPath()) : json();
        }
        j["domains"] = accessible;
        j["conflicts"] = conflicts;
        j["skipped"] = skipped;
        return j;
    }

    std::optional<nlohmann::json> Application::flows(std::string const& domainId)
    {
        auto const id = util::parseUuid(domainId);
        if (!id || !_directory)
        {
            return std::nullopt;
        }
        auto const domain = _directory->findById(*id);
        if (!domain)
        {
            return std::nullopt;
        }
        std::shared_ptr<mxlbridge::Instance> instance;
        try
        {
            instance = _instances.acquire(domain->path);
        }
        catch (std::exception const&)
        {}
        json out = json::array();
        for (auto const& f : mxlbridge::listFlowDirs(domain->path, true))
        {
            json j;
            j["id"] = f.id.toString();
            j["label"] = f.flowDef ? f.flowDef->value("label", std::string()) : std::string();
            j["media_type"] = f.flowDef ? f.flowDef->value("media_type", std::string()) : std::string();
            j["format"] = f.flowDef ? f.flowDef->value("format", std::string()) : std::string();
            j["active"] = mxlbridge::flowInUse(domain->path, f.id);
            j["head_index"] = nullptr;
            j["last_write_tai_ns"] = nullptr;
            if (instance && f.flowDef)
            {
                mxlStatus status = MXL_ERR_UNKNOWN;
                bool const audio = j["media_type"] == "audio/float32";
                auto reader = mxlbridge::openReader(instance, f.id.toString(), audio, status);
                if (reader)
                {
                    if (auto const rt = reader->runtime())
                    {
                        j["head_index"] = rt->headIndex;
                        j["last_write_tai_ns"] = rt->lastWriteTime;
                    }
                }
            }
            out.push_back(j);
        }
        return json{{"domain", domainEntryJson(*domain)}, {"flows", out}};
    }

    nlohmann::json Application::nmos()
    {
        if (!_node)
        {
            return {{"enabled", false}, {"setup_mode", _setupMode}};
        }
        auto j = _node->status();
        j["enabled"] = true;
        return j;
    }

    std::vector<ops::CheckResult> Application::preflight()
    {
        ops::PreflightEnv env;
        env.checkPortInUse = false;
        return ops::runPreflight(_store->snapshot().config, env);
    }

    ops::Readiness Application::readiness()
    {
        auto const cfg = _store->snapshot().config;
        ops::ReadinessInputs in;
        in.setupMode = _setupMode;
        in.configValid = true;
        in.mediaUp = _backend != nullptr;
        in.maxOffsetNs = cfg.ptp.maxOffsetNs;
        in.requireLock = cfg.ptp.requireLock;
        if (_backend)
        {
            auto const st = _backend->status();
            in.testBackend = st.testBackend;
            if (st.testBackend)
            {
                in.requireLock = false; // §17.2
            }
            if (st.ptpAvailable && cfg.ptp.mode != config::PtpMode::External)
            {
                bool locked = false;
                for (auto const& p : st.ptp)
                {
                    locked = locked || (p.selected && p.locked);
                }
                in.ptpLocked = locked;
            }
        }
        if (_clock && cfg.ptp.mode != config::PtpMode::External)
        {
            if (auto const c = _clock->latest(); c.valid)
            {
                in.clockOffsetNs = c.offsetNs;
            }
        }
        {
            std::lock_guard const lock{_cacheMutex};
            in.domainsOk = _domainsOk;
        }
        in.nmosRegistered = _node && _node->registered();
        in.registryAbsentIntended = !_options.withNmos;
        auto r = ops::evaluateReadiness(in);
        if (restartRequired())
        {
            r.warnings.push_back("restart_required");
        }
        if (_store->changedOnDisk())
        {
            r.warnings.push_back("config_changed_on_disk");
        }
        return r;
    }

    nlohmann::json Application::status()
    {
        auto const snap = _store->snapshot();
        auto const& cfg = snap.config;
        json j;
        j["node"] = {{"id", cfg.mxlNodeId().isNil() ? json() : json(cfg.mxlNodeId().toString())},
                     {"label", cfg.node.label},
                     {"description", cfg.node.description},
                     {"http_port", cfg.node.httpPort},
                     {"device_id", cfg.mxlNodeId().isNil() ? json() : json(ids::deviceId(cfg.mxlNodeId()).toString())}};
        j["versions"] = {{"gateway", version::gateway},
                         {"mtl", version::mtl},
                         {"dpdk", version::dpdk},
                         {"mxl", mxlbridge::mxlVersionString()},
                         {"nmos_cpp", version::nmosCpp}};
        j["setup_mode"] = _setupMode;
        j["readiness"] = readiness().toJson();
        j["restart_required"] = restartRequired();
        j["restart_reasons"] = restartReasons();
        j["config_changed_on_disk"] = _store->changedOnDisk();
        j["media"] = backendJson();
        j["ptp"] = ptp();
        json groups = json::array();
        if (_groups)
        {
            for (auto const& g : _groups->snapshot())
            {
                json gj{{"uid", g.uid.toString()},
                        {"label", g.label},
                        {"direction", config::toName(g.direction)},
                        {"enabled", g.enabled},
                        {"domain", g.domain},
                        {"output_delay_ns", g.direction == config::Direction::Egress ? json(g.outputDelayNs) : json()}};
                json essences = json::array();
                for (auto const& e : g.essences)
                {
                    essences.push_back(essenceJson(e));
                }
                gj["essences"] = essences;
                groups.push_back(gj);
            }
        }
        // Disabled groups are listed too (no runtime objects, §7.6).
        for (auto const& g : cfg.groups)
        {
            if (!g.enabled)
            {
                groups.push_back({{"uid", g.uid.toString()},
                                  {"label", g.label},
                                  {"direction", config::toName(g.direction)},
                                  {"enabled", false},
                                  {"domain", g.domain},
                                  {"essences", json::array()}});
            }
        }
        j["groups"] = groups;
        j["domains"] = domains();
        j["nmos"] = _node ? _node->status() : json{{"enabled", false}};
        return j;
    }

    std::string Application::metrics()
    {
        ops::MetricsInput in;
        in.ready = readiness().ready;
        in.restartRequired = restartRequired();
        if (_backend)
        {
            in.backend = _backend->status();
        }
        if (_clock)
        {
            in.clock = _clock->latest();
        }
        if (_groups)
        {
            in.groups = _groups->snapshot();
        }
        if (_directory)
        {
            in.scan = _directory->last();
        }
        {
            std::lock_guard const lock{_cacheMutex};
            in.domains = _domainUsage;
            in.activations = _activations;
        }
        in.nmosRegistered = _node && _node->registered();
        ops::MetricsWriter w;
        ops::exportMetrics(w, in);
        return w.render();
    }

    std::string_view Application::adminHtml() const
    {
        return embedded::webUiHtml();
    }

    void Application::requestRestart()
    {
        log::info("restart_requested", {});
        stop(exitOk);
    }
}
