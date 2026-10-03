// SPDX-License-Identifier: MIT
// nmos-cpp node (IS-04 v1.3, IS-05 v1.1/v1.2, BCP-007-03) with the gateway routes on the same port (§7).
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <set>

#include "cpprest/host_utils.h"
#include "nmos/activation_mode.h"
#include "nmos/api_utils.h"
#include "nmos/capabilities.h"
#include "nmos/certificate_handlers.h"
#include "nmos/channels.h"
#include "nmos/clock_name.h"
#include "nmos/colorspace.h"
#include "nmos/connection_activation.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/interlace_mode.h"
#include "nmos/is04_versions.h"
#include "nmos/log_model.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/resource.h"
#include "nmos/sdp_utils.h"
#include "nmos/server.h"
#include "nmos/server_utils.h"
#include "nmos/settings.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "nmos/version.h"
#include "sdp/sdp.h"

#include "app/connection_state.hpp"
#include "app/targets.hpp"
#include "group/flow_identity.hpp"
#include "mtl/sdp_map.hpp"
#include "nmos/http_adapter.hpp"
#include "nmos/ids.hpp"
#include "nmos/node_api.hpp"
#include "nmos/sdp_parse.hpp"
#include "util/logging.hpp"

namespace mxlgw::nmosnode
{
    namespace
    {
        using web::json::value;
        using web::json::value_of;
        using njson = nlohmann::json;

        value toWeb(njson const& j)
        {
            return value::parse(j.dump());
        }

        njson toNl(value const& v)
        {
            return njson::parse(v.serialize());
        }

        utility::string_t us(std::string const& s)
        {
            return s;
        }

        nmos::rational rationalOf(util::Rational r)
        {
            return nmos::rational{r.num, r.den};
        }

        nmos::interlace_mode interlaceOf(config::Interlace i)
        {
            switch (i)
            {
                case config::Interlace::Progressive: return nmos::interlace_modes::progressive;
                case config::Interlace::InterlacedTff: return nmos::interlace_modes::interlaced_tff;
                case config::Interlace::InterlacedBff: return nmos::interlace_modes::interlaced_bff;
            }
            return nmos::interlace_modes::progressive;
        }

        nmos::colorspace colorspaceOf(std::string const& c)
        {
            return nmos::colorspace{us(c.empty() ? std::string("BT709") : c)};
        }

        nmos::transfer_characteristic tcsOf(std::string const& t)
        {
            return nmos::transfer_characteristic{us(t.empty() ? std::string("SDR") : t)};
        }

        char const* roleOf(config::EssenceType t)
        {
            return t == config::EssenceType::Video ? "Video" : t == config::EssenceType::Audio ? "Audio" : "Data";
        }

        void tag(nmos::resource& r, std::string const& hint)
        {
            if (!r.data.has_field(U("tags")) || !r.data.at(U("tags")).is_object())
            {
                r.data[U("tags")] = value::object();
            }
            r.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")] = value_of({value::string(us(hint))});
        }

        /// BCP-002-01 roles must be unique within a group across Senders and Receivers (IS-04-01 test_23); an
        /// essence's Receiver and Sender would otherwise share "<Role> <n>" (docs/decisions.md, open question O-5).
        std::string receiverHint(std::string const& hint)
        {
            return hint + " Input";
        }

        void label(nmos::resource& r, std::string const& l, std::string const& d)
        {
            r.data[U("label")] = value::string(us(l));
            r.data[U("description")] = value::string(us(d));
        }

        value captureSet(std::initializer_list<std::pair<utility::string_t, value>> items)
        {
            value set = value::object();
            for (auto const& [k, v] : items)
            {
                set[k] = v;
            }
            return set;
        }

        void setCaps(nmos::resource& receiver, value set)
        {
            receiver.data[nmos::fields::caps][nmos::fields::constraint_sets] = value_of({set});
            receiver.data[nmos::fields::version] = receiver.data[nmos::fields::caps][nmos::fields::version] = value(nmos::make_version());
        }

        std::string jsonText(value const& v, utility::string_t const& key)
        {
            return v.has_field(key) && v.at(key).is_string() ? v.at(key).as_string() : std::string();
        }

        /// IS-04 interfaces use lower-case, dash-separated MAC addresses ("02-00-00-00-00-01").
        std::string nmosMac(std::string mac)
        {
            for (auto& c : mac)
            {
                c = c == ':' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return mac;
        }

        /// Deterministic SSM default address for a sender leg without configured destination.
        std::string ssmAddress(nmos::id const& id, int leg)
        {
            unsigned h = 2166136261u;
            for (auto c : id)
            {
                h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
            }
            return "232." + std::to_string((h >> 16) & 0xFF) + "." + std::to_string((h >> 8) & 0xFF) + "." + std::to_string((h & 0xFE) + leg);
        }
    }

    /// One Sender or Receiver of ours.
    struct ResourceRef
    {
        util::Uuid groupUid;
        util::Uuid essenceUid;
        config::EssenceType type = config::EssenceType::Video;
        config::Direction direction = config::Direction::Ingest;
        std::size_t index = 0;
        bool sender = false;
        bool rtp = false;
    };

    class NodeImpl final : public Node
    {
    public:
        NodeImpl(Setup setup, Callbacks callbacks)
            : _setup(std::move(setup))
            , _callbacks(std::move(callbacks))
        {
            _nodeId = us(_setup.config.mxlNodeId().toString());
            _deviceId = us(ids::deviceId(_setup.config.mxlNodeId()).toString());
            for (auto const& d : _setup.domains)
            {
                _domainIds[d.name] = d.id;
            }
            buildSettings();
        }

        ~NodeImpl() override { stop(); }

        void start() override
        {
            auto implementation = nmos::experimental::node_implementation()
                                      .on_load_server_certificates(nmos::make_load_server_certificates_handler(_model.settings, nmosGate()))
                                      .on_load_dh_param(nmos::make_load_dh_param_handler(_model.settings, nmosGate()))
                                      .on_load_ca_certificates(nmos::make_load_ca_certificates_handler(_model.settings, nmosGate()))
                                      .on_registration_changed([this](web::uri const& uri) { onRegistration(uri); })
                                      .on_parse_transport_file([this](nmos::resource const& r, nmos::resource const& c, utility::string_t const& type,
                                                                      utility::string_t const& data, slog::base_gate& gate)
                                                               { return parseTransportFile(r, c, type, data, gate); })
                                      .on_validate_connection_resource_patch([this](nmos::resource const& r, nmos::resource const& c, value const& staged,
                                                                                    slog::base_gate&) { validateStaged(r, c, staged); })
                                      .on_resolve_auto([this](nmos::resource const& r, nmos::resource const& c, value& params) { resolveAuto(r, c, params); })
                                      .on_set_transportfile([this](nmos::resource const& s, nmos::resource const& c, value& tf) { setTransportFile(s, c, tf); })
                                      .on_connection_activated([this](nmos::resource const& r, nmos::resource const& c) { onActivated(r, c); });
            _server.emplace(nmos::experimental::make_node_server(_model, implementation, _logModel, nmosGate()));
            if (_setup.routes != nullptr)
            {
                // §7.1: all gateway routes on the node's single listener.
                auto& api = _server->api_routers[{{}, nmos::fields::node_port(_model.settings)}];
                mountGatewayRoutes(api, *_setup.routes, _model.settings, nmosGate(), true);
            }
            {
                auto lock = _model.write_lock();
                buildNode();
                for (auto const& g : _setup.config.groups)
                {
                    if (g.enabled)
                    {
                        insertGroup(g, {});
                    }
                }
                updateDevice();
                _model.notify();
            }
            try
            {
                _server->open().wait();
            }
            catch (std::exception const& ex)
            {
                _server.reset();
                throw std::runtime_error(std::string("cannot open the HTTP listener on port ") + std::to_string(_setup.config.node.httpPort) + ": " +
                                         ex.what());
            }
            _open = true;
            log::info("nmos_node_started", {{"node_id", _nodeId},
                                            {"device_id", _deviceId},
                                            {"http_port", _setup.config.node.httpPort},
                                            {"registry", _setup.config.node.registry.dnsSd ? "dns-sd" : "static"}});
        }

        void stop() override
        {
            if (!_server)
            {
                return;
            }
            {
                auto lock = _model.write_lock();
                _model.shutdown = true;
                _model.notify();
            }
            if (_open)
            {
                try
                {
                    _server->close().wait();
                }
                catch (std::exception const& ex)
                {
                    log::warn("nmos_node_stop_failed", {{"error", ex.what()}});
                }
            }
            _open = false;
            _server.reset();
        }

        njson applyGroups(config::Config const& config) override
        {
            njson result = njson::object();
            auto lock = _model.write_lock();
            std::map<util::Uuid, config::Group const*> wanted;
            for (auto const& g : config.groups)
            {
                if (g.enabled)
                {
                    wanted[g.uid] = &g;
                }
            }
            std::vector<util::Uuid> removed;
            std::vector<util::Uuid> changed;
            std::vector<util::Uuid> added;
            for (auto const& [uid, g] : _groups)
            {
                if (wanted.find(uid) == wanted.end())
                {
                    removed.push_back(uid);
                }
            }
            for (auto const& [uid, g] : wanted)
            {
                auto const it = _groups.find(uid);
                if (it == _groups.end())
                {
                    added.push_back(uid);
                }
                else if (config::toJson(it->second).dump() != config::toJson(*g).dump())
                {
                    changed.push_back(uid);
                }
            }
            for (auto const& uid : removed)
            {
                eraseGroup(uid, nullptr, true);
            }
            for (auto const& uid : changed)
            {
                // Keep /staged and /active of surviving Senders/Receivers across the rebuild (§9.3).
                std::map<nmos::id, std::pair<value, value>> keep;
                eraseGroup(uid, &keep, false);
                insertGroup(*wanted[uid], keep);
            }
            for (auto const& uid : added)
            {
                insertGroup(*wanted[uid], {});
            }
            _setup.config = config;
            updateDevice();
            _model.notify();
            auto list = [](std::vector<util::Uuid> const& v)
            {
                njson a = njson::array();
                for (auto const& u : v)
                {
                    a.push_back(u.toString());
                }
                return a;
            };
            result["added"] = list(added);
            result["reregistered"] = list(changed);
            result["removed"] = list(removed);
            return result;
        }

        void updateClock(ClockInfo const& clock) override
        {
            auto lock = _model.write_lock();
            _setup.clock = clock;
            auto const node = nmos::find_resource(_model.node_resources, {_nodeId, nmos::types::node});
            if (node == _model.node_resources.end())
            {
                return;
            }
            nmos::modify_resource(_model.node_resources, _nodeId,
                                  [&](nmos::resource& r)
                                  {
                                      r.data[U("clocks")] = clocks();
                                      r.data[nmos::fields::version] = value(nmos::make_version());
                                  });
            _model.notify();
        }

        bool registered() const override { return _registered.load(); }

        njson status() const override
        {
            njson j;
            j["node_id"] = _nodeId;
            j["device_id"] = _deviceId;
            j["registered"] = _registered.load();
            {
                std::lock_guard const lock{_statusMutex};
                j["registration_uri"] = _registrationUri;
            }
            j["registry_mode"] = _setup.config.node.registry.dnsSd ? "dns-sd" : "static";
            njson senders = njson::array();
            njson receivers = njson::array();
            auto lock = _model.read_lock();
            for (auto const& [id, ref] : _refs)
            {
                auto const type = ref.sender ? nmos::types::sender : nmos::types::receiver;
                auto const r = nmos::find_resource(_model.node_resources, {id, type});
                auto const c = nmos::find_resource(_model.connection_resources, {id, type});
                if (r == _model.node_resources.end())
                {
                    continue;
                }
                njson e;
                e["id"] = id;
                e["label"] = jsonText(r->data, U("label"));
                e["transport"] = jsonText(r->data, U("transport"));
                e["essence_uid"] = ref.essenceUid.toString();
                e["group_uid"] = ref.groupUid.toString();
                e["type"] = config::toName(ref.type);
                if (r->data.has_field(U("tags")))
                {
                    e["tags"] = toNl(r->data.at(U("tags")));
                }
                if (c != _model.connection_resources.end())
                {
                    e["active"] = toNl(nmos::fields::endpoint_active(c->data));
                    if (ref.sender && ref.rtp && c->data.has_field(nmos::fields::endpoint_transportfile))
                    {
                        auto const& tf = nmos::fields::endpoint_transportfile(c->data);
                        if (tf.has_field(U("data")) && tf.at(U("data")).is_string())
                        {
                            e["sdp"] = tf.at(U("data")).as_string();
                        }
                    }
                }
                (ref.sender ? senders : receivers).push_back(e);
            }
            j["senders"] = senders;
            j["receivers"] = receivers;
            return j;
        }

    private:
        void buildSettings()
        {
            auto const& n = _setup.config.node;
            value s = value::object();
            s[U("http_port")] = n.httpPort;
            s[U("label")] = value::string(us(n.label));
            s[U("description")] = value::string(us(n.description));
            s[U("seed_id")] = value::string(_nodeId);
            s[U("logging_level")] = log::enabled(log::Level::Trace) ? slog::severities::more_info : log::enabled(log::Level::Debug) ? 0 : 10;
            s[U("is04_versions")] = value_of({U("v1.3")});
            s[U("is05_versions")] = value_of({U("v1.1"), U("v1.2")});
            // §7.1: only node.http_port is opened; everything optional is disabled with a negative port.
            // VERIFIED: sony/nmos-cpp@fe30384 Development/nmos/server.cpp open_listeners skips ports < 0.
            for (auto const* key : {"events_port", "events_ws_port", "channelmapping_port", "configuration_port", "control_protocol_ws_port", "annotation_port",
                                    "settings_port", "logging_port", "system_port"})
            {
                s[us(key)] = -1;
            }
            std::vector<std::string> addresses = n.managementAddresses;
            if (n.hostAddress)
            {
                addresses = {*n.hostAddress};
            }
            if (!addresses.empty())
            {
                s[U("host_address")] = value::string(us(addresses.front()));
                value arr = value::array();
                for (auto const& a : addresses)
                {
                    web::json::push_back(arr, value::string(us(a)));
                }
                s[U("host_addresses")] = arr;
            }
            if (n.publicPort)
            {
                // C1: advertise the proxy/mapped port, listen on http_port.
                for (auto const* key : {"http_port", "node_port", "connection_port", "manifest_port"})
                {
                    s[us(key)] = *n.publicPort;
                }
                s[U("proxy_map")] = value_of({value_of({{U("client_port"), *n.publicPort}, {U("server_port"), n.httpPort}})});
            }
            if (!n.registry.address.empty())
            {
                s[U("registry_address")] = value::string(us(n.registry.address));
                s[U("registration_port")] = n.registry.port;
                s[U("registry_version")] = value::string(U("v1.3"));
            }
            if (n.tls.enabled)
            {
                s[U("server_secure")] = true;
                s[U("server_certificates")] = value_of({value_of({{U("key_algorithm"), U("ECDSA")},
                                                                  {U("private_key_file"), value::string(us(n.tls.privateKey))},
                                                                  {U("certificate_chain_file"), value::string(us(n.tls.certificate))}})});
            }
            _model.settings = s;
            nmos::insert_node_default_settings(_model.settings);
            _logModel.settings = _model.settings;
            _logModel.level = nmos::fields::logging_level(_logModel.settings);
        }

        value clocks() const
        {
            auto const& c = _setup.clock;
            if (c.ptp && !c.gmid.empty())
            {
                return value_of({nmos::make_ptp_clock(nmos::clock_names::clk0, c.traceable, us(c.gmid), c.locked)});
            }
            return value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
        }

        std::vector<utility::string_t> mediaInterfaces(bool redundant) const
        {
            std::vector<utility::string_t> names;
            auto const& pairs = _setup.config.nic.portPairs;
            if (!pairs.empty())
            {
                names.push_back(us(pairs.front().primary.name));
                if (redundant && pairs.front().redundant)
                {
                    names.push_back(us(pairs.front().redundant->name));
                }
            }
            return names;
        }

        std::string portIp(int leg) const
        {
            auto const& pairs = _setup.config.nic.portPairs;
            if (pairs.empty())
            {
                return {};
            }
            if (leg == 1 && pairs.front().redundant)
            {
                return pairs.front().redundant->ip;
            }
            return pairs.front().primary.ip;
        }

        void buildNode()
        {
            std::map<utility::string_t, nmos::node_interface> interfaces;
            for (auto const& i : _setup.interfaces)
            {
                interfaces[us(i.name)] = nmos::node_interface{us(nmosMac(i.chassisMac)), us(nmosMac(i.mac)), us(i.name), {}, {}};
            }
            // The management interface(s) from the kernel keep href/api.endpoints consistent (§4.5).
            for (auto const& [name, iface] : nmos::experimental::node_interfaces(nmos::get_host_interfaces(_model.settings)))
            {
                if (interfaces.find(name) == interfaces.end())
                {
                    interfaces[name] = iface;
                }
            }
            auto node = nmos::make_node(_nodeId, clocks(), nmos::make_node_interfaces(interfaces), _model.settings);
            label(node, _setup.config.node.label, _setup.config.node.description);
            nmos::insert_resource(_model.node_resources, std::move(node));
            auto device = nmos::make_device(_deviceId, _nodeId, {}, {}, _model.settings);
            label(device, _setup.config.node.label, "mxl-st2110-gateway " + _setup.gatewayVersion);
            nmos::insert_resource(_model.node_resources, std::move(device));
        }

        void updateDevice()
        {
            std::vector<nmos::id> senders;
            std::vector<nmos::id> receivers;
            for (auto const& [id, ref] : _refs)
            {
                (ref.sender ? senders : receivers).push_back(id);
            }
            nmos::modify_resource(_model.node_resources, _deviceId,
                                  [&](nmos::resource& r)
                                  {
                                      value s = value::array();
                                      for (auto const& id : senders)
                                      {
                                          web::json::push_back(s, value::string(id));
                                      }
                                      value rr = value::array();
                                      for (auto const& id : receivers)
                                      {
                                          web::json::push_back(rr, value::string(id));
                                      }
                                      r.data[U("senders")] = s;
                                      r.data[U("receivers")] = rr;
                                      r.data[nmos::fields::version] = value(nmos::make_version());
                                  });
        }

        util::Uuid groupDomainId(config::Group const& g) const
        {
            auto const it = _domainIds.find(g.domain);
            return it == _domainIds.end() ? util::Uuid{} : it->second;
        }

        /// Staged + scheduled immediate activation (§7.6): saved /active, kept state or defaults.
        void stageInitial(nmos::resource& connection, value masterEnable, value transportParams)
        {
            auto& staged = connection.data[nmos::fields::endpoint_staged];
            staged[nmos::fields::master_enable] = masterEnable;
            auto& legs = staged[nmos::fields::transport_params];
            for (std::size_t i = 0; i < legs.size() && i < transportParams.size(); ++i)
            {
                for (auto const& field : transportParams.at(i).as_object())
                {
                    legs[i][field.first] = field.second;
                }
            }
            staged[nmos::fields::activation] = value_of({{nmos::fields::mode, value::string(nmos::activation_modes::activate_scheduled_relative.name)},
                                                         {nmos::fields::requested_time, value::string(U("0:0"))},
                                                         {nmos::fields::activation_time, value::string(nmos::make_version())}});
        }

        bool restoreSaved(nmos::resource& connection, std::map<nmos::id, std::pair<value, value>> const& keep)
        {
            if (auto const it = keep.find(connection.id); it != keep.end())
            {
                connection.data[nmos::fields::endpoint_staged] = it->second.first;
                connection.data[nmos::fields::endpoint_active] = it->second.second;
                return true;
            }
            if (_setup.connections == nullptr)
            {
                return false;
            }
            auto const uid = util::parseUuid(connection.id);
            if (!uid)
            {
                return false;
            }
            auto const saved = _setup.connections->active(*uid);
            if (!saved || !saved->contains("transport_params"))
            {
                return false;
            }
            auto const params = toWeb((*saved)["transport_params"]);
            if (params.size() != nmos::fields::transport_params(connection.data[nmos::fields::endpoint_staged]).size())
            {
                return false; // redundancy changed since: start from defaults
            }
            stageInitial(connection, value::boolean(saved->value("master_enable", false)), params);
            return true;
        }

        void insertEssence(config::Group const& g, config::EssenceType type, std::size_t index, std::map<nmos::id, std::pair<value, value>> const& keep)
        {
            auto const ids = group::essenceIds(g, type, index);
            auto const hint = group::groupHint(g, type, index);
            auto const role = std::string(roleOf(type)) + " " + std::to_string(index + 1);
            std::string label_;
            int payloadType = 96;
            std::vector<config::Leg> defaultLegs;
            config::EssenceCommon const* common = nullptr;
            switch (type)
            {
                case config::EssenceType::Video: common = &g.video.at(index); break;
                case config::EssenceType::Audio: common = &g.audio.at(index); break;
                case config::EssenceType::Anc: common = &g.anc.at(index); break;
            }
            label_ = common->label;
            payloadType = common->payloadType;
            auto const sourceId = us(ids.source.toString());
            auto const flowId = us(ids.flow.toString());
            auto const senderId = us(ids.sender.toString());
            auto const receiverId = us(ids.receiver.toString());
            auto const redundant = g.redundancy;
            auto const rate = type == config::EssenceType::Video ? g.video.at(index).format.rate
                              : type == config::EssenceType::Anc ? g.anc.at(index).format.rate
                                                                 : util::Rational{g.audio.at(index).format.sampleRate, 1};

            // ---- Source + Flow
            nmos::resource source;
            if (type == config::EssenceType::Video)
            {
                source = nmos::make_video_source(sourceId, _deviceId, nmos::clock_names::clk0, rationalOf(rate), _model.settings);
            }
            else if (type == config::EssenceType::Audio)
            {
                std::vector<nmos::channel> channels;
                for (int c = 1; c <= g.audio.at(index).format.channels; ++c)
                {
                    channels.push_back(nmos::channel{us("Ch " + std::to_string(c)), nmos::channel_symbols::Undefined(static_cast<unsigned>(c))});
                }
                auto const frameRate = g.video.empty() ? util::Rational{50, 1} : g.video.front().format.rate;
                source = nmos::make_audio_source(sourceId, _deviceId, nmos::clock_names::clk0, rationalOf(frameRate), channels, _model.settings);
            }
            else
            {
                source = nmos::make_data_source(sourceId, _deviceId, nmos::clock_names::clk0, rationalOf(rate), _model.settings);
            }
            label(source, label_, g.label + " " + role);
            tag(source, hint);

            nmos::resource flow;
            if (g.direction == config::Direction::Ingest)
            {
                // §7.2: the IS-04 Flow body IS the MXL flow descriptor (flow_def.json).
                auto def = group::flowDefinition(_setup.config.mxlNodeId(), g, type, index, nmos::make_version());
                flow = nmos::resource{nmos::is04_versions::v1_3, nmos::types::flow, toWeb(def), false};
            }
            else if (type == config::EssenceType::Video)
            {
                auto const& f = g.video.at(index).format;
                flow = nmos::make_raw_video_flow(flowId, sourceId, _deviceId, rationalOf(f.rate), static_cast<unsigned>(f.width),
                                                 static_cast<unsigned>(f.height), interlaceOf(f.interlace), colorspaceOf(f.colorimetry), tcsOf(f.tcs),
                                                 sdp::samplings::YCbCr_4_2_2, 10, _model.settings);
            }
            else if (type == config::EssenceType::Audio)
            {
                auto const& f = g.audio.at(index).format;
                flow =
                    nmos::make_raw_audio_flow(flowId, sourceId, _deviceId, nmos::rational{f.sampleRate, 1}, static_cast<unsigned>(f.bitDepth), _model.settings);
            }
            else
            {
                flow = nmos::make_sdianc_data_flow(flowId, sourceId, _deviceId, {}, _model.settings);
                flow.data[nmos::fields::grain_rate] = nmos::make_rational(rationalOf(g.anc.at(index).format.rate));
            }
            label(flow, label_, g.label + " " + role + (g.direction == config::Direction::Ingest ? " (MXL)" : " (ST 2110)"));
            tag(flow, hint);
            nmos::insert_resource(_model.node_resources, std::move(source));
            nmos::insert_resource(_model.node_resources, std::move(flow));

            if (g.direction == config::Direction::Ingest)
            {
                // ---- ST 2110 Receiver
                nmos::resource receiver;
                if (type == config::EssenceType::Video)
                {
                    auto const& f = g.video.at(index).format;
                    receiver = nmos::make_receiver(receiverId, _deviceId, nmos::transports::rtp, mediaInterfaces(redundant), nmos::formats::video,
                                                   {nmos::media_types::video_raw}, _model.settings);
                    setCaps(receiver, captureSet({{nmos::caps::format::media_type, nmos::make_caps_string_constraint({nmos::media_types::video_raw.name})},
                                                  {nmos::caps::format::grain_rate, nmos::make_caps_rational_constraint({rationalOf(f.rate)})},
                                                  {nmos::caps::format::frame_width, nmos::make_caps_integer_constraint({f.width})},
                                                  {nmos::caps::format::frame_height, nmos::make_caps_integer_constraint({f.height})},
                                                  {nmos::caps::format::interlace_mode, nmos::make_caps_string_constraint({interlaceOf(f.interlace).name})},
                                                  {nmos::caps::format::color_sampling, nmos::make_caps_string_constraint({sdp::samplings::YCbCr_4_2_2.name})},
                                                  {nmos::caps::format::component_depth, nmos::make_caps_integer_constraint({10})}}));
                }
                else if (type == config::EssenceType::Audio)
                {
                    auto const& f = g.audio.at(index).format;
                    receiver = nmos::make_audio_receiver(receiverId, _deviceId, nmos::transports::rtp, mediaInterfaces(redundant),
                                                         static_cast<unsigned>(f.bitDepth), _model.settings);
                    setCaps(receiver, captureSet({{nmos::caps::format::channel_count, nmos::make_caps_integer_constraint({f.channels})},
                                                  {nmos::caps::format::sample_rate, nmos::make_caps_rational_constraint({nmos::rational{f.sampleRate, 1}})},
                                                  {nmos::caps::format::sample_depth, nmos::make_caps_integer_constraint({f.bitDepth})},
                                                  {nmos::caps::transport::packet_time, nmos::make_caps_number_constraint({f.ptimeUs / 1000.0})}}));
                }
                else
                {
                    receiver = nmos::make_sdianc_data_receiver(receiverId, _deviceId, nmos::transports::rtp, mediaInterfaces(redundant), _model.settings);
                    setCaps(receiver,
                            captureSet({{nmos::caps::format::grain_rate, nmos::make_caps_rational_constraint({rationalOf(g.anc.at(index).format.rate)})}}));
                }
                label(receiver, label_, g.label + " " + role + " ST 2110 receiver");
                tag(receiver, receiverHint(hint));
                auto rconn = nmos::make_connection_rtp_receiver(receiverId, redundant);
                for (int leg = 0; leg < (redundant ? 2 : 1); ++leg)
                {
                    if (!portIp(leg).empty())
                    {
                        rconn.data[nmos::fields::endpoint_constraints][leg][nmos::fields::interface_ip] =
                            value_of({{nmos::fields::constraint_enum, value_of({value::string(us(portIp(leg)))})}});
                    }
                }
                if (!restoreSaved(rconn, keep))
                {
                    njson legs = app::defaultRtpReceiverParams(*common, redundant);
                    stageInitial(rconn, value::boolean(!common->legs.empty()), toWeb(legs));
                }
                nmos::insert_resource(_model.node_resources, std::move(receiver));
                nmos::insert_resource(_model.connection_resources, std::move(rconn));
                _refs[receiverId] = {g.uid, common->uid, type, g.direction, index, false, true};

                // ---- MXL Sender
                auto sender = nmos::make_sender(senderId, flowId, nmos::transports::mxl, _deviceId, {}, {}, _model.settings);
                label(sender, label_, g.label + " " + role + " MXL sender");
                tag(sender, hint);
                auto const domainId = us(groupDomainId(g).toString());
                auto sconn = nmos::make_connection_mxl_sender(senderId, domainId, flowId);
                if (!restoreSaved(sconn, keep))
                {
                    stageInitial(sconn, value::boolean(true),
                                 value_of({value_of({{U("mxl_domain_id"), value::string(domainId)}, {U("mxl_flow_id"), value::string(flowId)}})}));
                }
                else
                {
                    // A format change minted a new flow UUID (§7.3): keep the activation, point it at the new flow.
                    for (auto* endpoint : {&sconn.data[nmos::fields::endpoint_staged], &sconn.data[nmos::fields::endpoint_active]})
                    {
                        auto& legs = (*endpoint)[nmos::fields::transport_params];
                        if (legs.size() > 0 && legs[0].has_field(U("mxl_flow_id")) && legs[0][U("mxl_flow_id")].is_string() &&
                            legs[0][U("mxl_flow_id")].as_string() != U("auto"))
                        {
                            legs[0][U("mxl_flow_id")] = value::string(flowId);
                        }
                    }
                }
                nmos::insert_resource(_model.node_resources, std::move(sender));
                nmos::insert_resource(_model.connection_resources, std::move(sconn));
                _refs[senderId] = {g.uid, common->uid, type, g.direction, index, true, false};
            }
            else
            {
                // ---- MXL Receiver
                nmos::resource receiver;
                if (type == config::EssenceType::Video)
                {
                    auto const& f = g.video.at(index).format;
                    receiver = nmos::make_receiver(receiverId, _deviceId, nmos::transports::mxl, {}, nmos::formats::video, {nmos::media_types::video_v210},
                                                   _model.settings);
                    setCaps(receiver, captureSet({{nmos::caps::format::media_type, nmos::make_caps_string_constraint({nmos::media_types::video_v210.name})},
                                                  {nmos::caps::format::grain_rate, nmos::make_caps_rational_constraint({rationalOf(f.rate)})},
                                                  {nmos::caps::format::frame_width, nmos::make_caps_integer_constraint({f.width})},
                                                  {nmos::caps::format::frame_height, nmos::make_caps_integer_constraint({f.height})},
                                                  {nmos::caps::format::interlace_mode, nmos::make_caps_string_constraint({interlaceOf(f.interlace).name})},
                                                  {nmos::caps::format::color_sampling, nmos::make_caps_string_constraint({sdp::samplings::YCbCr_4_2_2.name})},
                                                  {nmos::caps::format::component_depth, nmos::make_caps_integer_constraint({10})}}));
                }
                else if (type == config::EssenceType::Audio)
                {
                    auto const& f = g.audio.at(index).format;
                    receiver = nmos::make_receiver(receiverId, _deviceId, nmos::transports::mxl, {}, nmos::formats::audio, {nmos::media_types::audio_float32},
                                                   _model.settings);
                    setCaps(receiver, captureSet({{nmos::caps::format::media_type, nmos::make_caps_string_constraint({nmos::media_types::audio_float32.name})},
                                                  {nmos::caps::format::channel_count, nmos::make_caps_integer_constraint({f.channels})},
                                                  {nmos::caps::format::sample_rate, nmos::make_caps_rational_constraint({nmos::rational{f.sampleRate, 1}})},
                                                  {nmos::caps::format::sample_depth, nmos::make_caps_integer_constraint({32})}}));
                }
                else
                {
                    receiver = nmos::make_sdianc_data_receiver(receiverId, _deviceId, nmos::transports::mxl, {}, _model.settings);
                    setCaps(receiver, captureSet({{nmos::caps::format::media_type, nmos::make_caps_string_constraint({nmos::media_types::video_smpte291.name})},
                                                  {nmos::caps::format::grain_rate,
                                                   nmos::make_caps_rational_constraint({rationalOf(g.anc.at(index).format.grainRate())})}}));
                }
                label(receiver, label_, g.label + " " + role + " MXL receiver");
                tag(receiver, receiverHint(hint));
                // C3: mxl_domain_id unconstrained so a domain that does not exist yet can be staged.
                auto rconn = nmos::make_connection_mxl_receiver(receiverId, {});
                if (!restoreSaved(rconn, keep))
                {
                    stageInitial(rconn, value::boolean(false), value_of({value_of({{U("mxl_domain_id"), U("auto")}, {U("mxl_flow_id"), value::null()}})}));
                }
                nmos::insert_resource(_model.node_resources, std::move(receiver));
                nmos::insert_resource(_model.connection_resources, std::move(rconn));
                _refs[receiverId] = {g.uid, common->uid, type, g.direction, index, false, false};

                // ---- ST 2110 Sender
                auto const manifest = nmos::experimental::make_manifest_api_manifest(senderId, _model.settings);
                auto sender =
                    nmos::make_sender(senderId, flowId, nmos::transports::rtp, _deviceId, manifest.to_string(), mediaInterfaces(redundant), _model.settings);
                label(sender, label_, g.label + " " + role + " ST 2110 sender");
                tag(sender, hint);
                auto sconn = nmos::make_connection_rtp_sender(senderId, redundant);
                for (int leg = 0; leg < (redundant ? 2 : 1); ++leg)
                {
                    if (!portIp(leg).empty())
                    {
                        sconn.data[nmos::fields::endpoint_constraints][leg][nmos::fields::source_ip] =
                            value_of({{nmos::fields::constraint_enum, value_of({value::string(us(portIp(leg)))})}});
                    }
                }
                if (!restoreSaved(sconn, keep))
                {
                    auto const legs = app::defaultRtpSenderParams(*common, redundant, _setup.config.nic.portPairs.front());
                    stageInitial(sconn, value::boolean(!common->legs.empty()), toWeb(legs));
                }
                nmos::insert_resource(_model.node_resources, std::move(sender));
                nmos::insert_resource(_model.connection_resources, std::move(sconn));
                _refs[senderId] = {g.uid, common->uid, type, g.direction, index, true, true};
            }
            (void)payloadType;
        }

        void insertGroup(config::Group const& g, std::map<nmos::id, std::pair<value, value>> const& keep)
        {
            for (std::size_t i = 0; i < g.video.size(); ++i)
            {
                insertEssence(g, config::EssenceType::Video, i, keep);
            }
            for (std::size_t i = 0; i < g.audio.size(); ++i)
            {
                insertEssence(g, config::EssenceType::Audio, i, keep);
            }
            for (std::size_t i = 0; i < g.anc.size(); ++i)
            {
                insertEssence(g, config::EssenceType::Anc, i, keep);
            }
            _groups[g.uid] = g;
        }

        void eraseGroup(util::Uuid const& uid, std::map<nmos::id, std::pair<value, value>>* keep, bool forget)
        {
            for (auto it = _refs.begin(); it != _refs.end();)
            {
                if (it->second.groupUid != uid)
                {
                    ++it;
                    continue;
                }
                auto const type = it->second.sender ? nmos::types::sender : nmos::types::receiver;
                auto const c = nmos::find_resource(_model.connection_resources, {it->first, type});
                if (c != _model.connection_resources.end() && keep != nullptr)
                {
                    (*keep)[it->first] = {c->data.at(nmos::fields::endpoint_staged), c->data.at(nmos::fields::endpoint_active)};
                }
                if (it->second.sender)
                {
                    auto const s = nmos::find_resource(_model.node_resources, {it->first, nmos::types::sender});
                    if (s != _model.node_resources.end())
                    {
                        auto const flowId = jsonText(s->data, U("flow_id"));
                        if (auto const f = nmos::find_resource(_model.node_resources, {flowId, nmos::types::flow}); f != _model.node_resources.end())
                        {
                            auto const sourceId = jsonText(f->data, U("source_id"));
                            nmos::erase_resource(_model.node_resources, flowId);
                            nmos::erase_resource(_model.node_resources, sourceId);
                        }
                    }
                }
                nmos::erase_resource(_model.connection_resources, it->first);
                nmos::erase_resource(_model.node_resources, it->first);
                if (forget && _setup.connections != nullptr)
                {
                    if (auto const id = util::parseUuid(it->first))
                    {
                        _setup.connections->erase(*id);
                    }
                }
                it = _refs.erase(it);
            }
            _groups.erase(uid);
        }

        ResourceRef const* refOf(nmos::id const& id) const
        {
            auto const it = _refs.find(id);
            return it == _refs.end() ? nullptr : &it->second;
        }

        config::Group const* groupOf(ResourceRef const& ref) const
        {
            auto const it = _groups.find(ref.groupUid);
            return it == _groups.end() ? nullptr : &it->second;
        }

        // ---------------------------------------------------------------- IS-05 callbacks

        value parseTransportFile(nmos::resource const& receiver, nmos::resource const& connection, utility::string_t const& type, utility::string_t const& data,
                                 slog::base_gate& gate)
        {
            auto const* ref = refOf(connection.id);
            if (ref != nullptr && !ref->rtp)
            {
                // MXL Receivers take no transport file (BCP-007-03); rejected with 400 in validateStaged.
                return nmos::fields::transport_params(nmos::fields::endpoint_staged(connection.data));
            }
            // §6.4 / Q14: parse without nmos-cpp's caps check (which nmos-cpp would turn into a 500); the format
            // check is done in validateStaged so a mismatch is a 400. A malformed SDP still throws (500).
            return nmos::details::parse_rtp_transport_file([](value const&, nmos::sdp_parameters const&) {}, receiver, connection, type, data, gate);
        }

        void validateStaged(nmos::resource const& resource, nmos::resource const& connection, value const& staged)
        {
            auto const* ref = refOf(connection.id);
            if (ref == nullptr)
            {
                return;
            }
            auto const* g = groupOf(*ref);
            if (g == nullptr)
            {
                return;
            }
            bool const hasFile = staged.has_field(nmos::fields::transport_file) && staged.at(nmos::fields::transport_file).has_field(nmos::fields::data) &&
                                 !staged.at(nmos::fields::transport_file).at(nmos::fields::data).is_null();
            if (!ref->rtp && !ref->sender)
            {
                if (hasFile)
                {
                    throw web::json::json_exception("MXL Receivers do not accept a transport file (BCP-007-03)");
                }
                auto const& legs = nmos::fields::transport_params(staged);
                if (legs.size() == 0)
                {
                    return;
                }
                auto const& leg = legs.at(0);
                auto const flowText =
                    leg.has_field(U("mxl_flow_id")) && leg.at(U("mxl_flow_id")).is_string() ? leg.at(U("mxl_flow_id")).as_string() : utility::string_t();
                if (flowText == U("auto"))
                {
                    throw web::json::json_exception("mxl_flow_id MUST NOT be auto on an MXL Receiver (BCP-007-03)");
                }
                auto const domainText =
                    leg.has_field(U("mxl_domain_id")) && leg.at(U("mxl_domain_id")).is_string() ? leg.at(U("mxl_domain_id")).as_string() : utility::string_t();
                std::optional<util::Uuid> domain;
                if (!domainText.empty() && domainText != U("auto"))
                {
                    domain = util::parseUuid(domainText);
                    if (domain && _callbacks.domainAccessible && !_callbacks.domainAccessible(*domain) && _unknownDomainLog.allow(connection.id))
                    {
                        // Owner decision C3: accepted, waited for (deviation from BCP-007-03, docs/decisions.md).
                        log::warn("mxl_domain_unknown", {{"receiver_id", connection.id}, {"mxl_domain_id", domainText}});
                    }
                }
                if (!flowText.empty() && _callbacks.checkMxlFlow)
                {
                    if (auto const flow = util::parseUuid(flowText))
                    {
                        auto checkDomain = domain;
                        if (!checkDomain && _callbacks.resolveMxlDomain)
                        {
                            checkDomain = _callbacks.resolveMxlDomain(groupDomainId(*g), flow);
                        }
                        auto const mismatch = _callbacks.checkMxlFlow(ref->essenceUid, checkDomain, *flow);
                        if (!mismatch.empty())
                        {
                            throw web::json::json_exception(("flow " + flowText + " does not match the receiver's format: " + mismatch).c_str());
                        }
                    }
                }
                return;
            }
            if (ref->rtp && !ref->sender && hasFile)
            {
                auto const& tf = staged.at(nmos::fields::transport_file);
                auto const sdpText = tf.at(nmos::fields::data).as_string();
                std::optional<sdpmap::SdpMedia> media;
                try
                {
                    media = parseSdpMedia(utility::us2s(sdpText));
                }
                catch (std::exception const& ex)
                {
                    throw web::json::json_exception((std::string("invalid SDP: ") + ex.what()).c_str());
                }
                std::vector<std::string> errors;
                switch (ref->type)
                {
                    case config::EssenceType::Video: errors = sdpmap::checkVideo(*media, g->video.at(ref->index)); break;
                    case config::EssenceType::Audio: errors = sdpmap::checkAudio(*media, g->audio.at(ref->index)); break;
                    case config::EssenceType::Anc: errors = sdpmap::checkAnc(*media, g->anc.at(ref->index)); break;
                }
                if (!errors.empty())
                {
                    std::string message = "SDP does not match the configured essence format:";
                    for (auto const& e : errors)
                    {
                        message += " " + e + ";";
                    }
                    throw web::json::json_exception(message.c_str());
                }
            }
            (void)resource;
        }

        void resolveAuto(nmos::resource const& resource, nmos::resource const& connection, value& params)
        {
            auto const* ref = refOf(connection.id);
            if (ref == nullptr || params.size() == 0)
            {
                return;
            }
            auto const* g = groupOf(*ref);
            if (g == nullptr)
            {
                return;
            }
            auto const& constraints = nmos::fields::endpoint_constraints(connection.data);
            if (ref->rtp)
            {
                config::EssenceCommon const* common = nullptr;
                switch (ref->type)
                {
                    case config::EssenceType::Video: common = &g->video.at(ref->index); break;
                    case config::EssenceType::Audio: common = &g->audio.at(ref->index); break;
                    case config::EssenceType::Anc: common = &g->anc.at(ref->index); break;
                }
                for (std::size_t leg = 0; leg < params.size(); ++leg)
                {
                    auto const ip = portIp(static_cast<int>(leg));
                    if (ref->sender)
                    {
                        nmos::details::resolve_auto(params[leg], nmos::fields::source_ip,
                                                    [&]
                                                    {
                                                        if (leg < constraints.size() && constraints.at(leg).has_field(nmos::fields::source_ip))
                                                        {
                                                            return web::json::front(
                                                                nmos::fields::constraint_enum(constraints.at(leg).at(nmos::fields::source_ip)));
                                                        }
                                                        return value::string(us(ip.empty() ? std::string("0.0.0.0") : ip));
                                                    });
                        nmos::details::resolve_auto(params[leg], nmos::fields::destination_ip,
                                                    [&]
                                                    {
                                                        if (leg < common->legs.size() && !common->legs[leg].multicast.empty())
                                                        {
                                                            return value::string(us(common->legs[leg].multicast));
                                                        }
                                                        return value::string(us(ssmAddress(connection.id, static_cast<int>(leg))));
                                                    });
                        nmos::details::resolve_auto(params[leg], nmos::fields::destination_port, [&]
                                                    { return value(leg < common->legs.size() && common->legs[leg].port > 0 ? common->legs[leg].port : 5004); });
                    }
                    else
                    {
                        nmos::details::resolve_auto(params[leg], nmos::fields::interface_ip,
                                                    [&]
                                                    {
                                                        if (leg < constraints.size() && constraints.at(leg).has_field(nmos::fields::interface_ip))
                                                        {
                                                            return web::json::front(
                                                                nmos::fields::constraint_enum(constraints.at(leg).at(nmos::fields::interface_ip)));
                                                        }
                                                        return value::string(us(ip.empty() ? std::string("0.0.0.0") : ip));
                                                    });
                    }
                }
                nmos::resolve_rtp_auto(ref->sender ? nmos::types::sender : nmos::types::receiver, params);
                return;
            }
            if (ref->sender)
            {
                auto const domainId = us(groupDomainId(*g).toString());
                nmos::details::resolve_auto(params[0], nmos::fields::mxl_domain_id, [&] { return value::string(domainId); });
                nmos::details::resolve_auto(params[0], nmos::fields::mxl_flow_id,
                                            [&] { return value::string(us(group::essenceIds(*g, ref->type, ref->index).flow.toString())); });
                return;
            }
            // MXL Receiver: `auto` -> group domain or the accessible domain holding the flow (rescans, §7.4).
            nmos::details::resolve_auto(params[0], nmos::fields::mxl_domain_id,
                                        [&]
                                        {
                                            std::optional<util::Uuid> flow;
                                            if (params[0].has_field(U("mxl_flow_id")) && params[0].at(U("mxl_flow_id")).is_string())
                                            {
                                                flow = util::parseUuid(params[0].at(U("mxl_flow_id")).as_string());
                                            }
                                            std::optional<util::Uuid> resolved;
                                            if (_callbacks.resolveMxlDomain)
                                            {
                                                resolved = _callbacks.resolveMxlDomain(groupDomainId(*g), flow);
                                            }
                                            if (!resolved)
                                            {
                                                throw std::logic_error("cannot resolve mxl_domain_id 'auto'"); // 500 for immediate activations (BCP-007-03)
                                            }
                                            return value::string(us(resolved->toString()));
                                        });
            (void)resource;
        }

        void setTransportFile(nmos::resource const& sender, nmos::resource const& connection, value& transportFile)
        {
            auto const* ref = refOf(connection.id);
            if (ref == nullptr || !ref->rtp)
            {
                transportFile = value::null();
                return;
            }
            auto const* g = groupOf(*ref);
            if (g == nullptr)
            {
                return;
            }
            // Model lock is held by the activation thread.
            auto const node = nmos::find_resource(_model.node_resources, {_nodeId, nmos::types::node});
            auto const flowId = jsonText(sender.data, U("flow_id"));
            auto const flow = nmos::find_resource(_model.node_resources, {flowId, nmos::types::flow});
            if (node == _model.node_resources.end() || flow == _model.node_resources.end())
            {
                throw std::logic_error("matching IS-04 node or flow not found");
            }
            auto const source = nmos::find_resource(_model.node_resources, {jsonText(flow->data, U("source_id")), nmos::types::source});
            if (source == _model.node_resources.end())
            {
                throw std::logic_error("matching IS-04 source not found");
            }
            std::vector<utility::string_t> const mids{U("PRIMARY"), U("SECONDARY")};
            auto const ptpDomain = bst::optional<int>(_setup.clock.domain);
            nmos::sdp_parameters params;
            switch (ref->type)
            {
                case config::EssenceType::Video:
                {
                    auto const& e = g->video.at(ref->index);
                    auto const tp = e.pacing == config::Pacing::Linear ? sdp::type_parameters::type_NL
                                    : e.pacing == config::Pacing::Wide ? sdp::type_parameters::type_W
                                                                       : sdp::type_parameters::type_N;
                    params = nmos::make_video_sdp_parameters(node->data, source->data, flow->data, sender.data, static_cast<uint64_t>(e.payloadType), mids,
                                                             ptpDomain, tp);
                    auto const pm = e.packing == config::Packing::Bpm ? U("2110BPM") : U("2110GPM");
                    for (auto& f : params.fmtp)
                    {
                        if (f.first == U("PM"))
                        {
                            f.second = pm;
                        }
                    }
                    break;
                }
                case config::EssenceType::Audio:
                {
                    auto const& e = g->audio.at(ref->index);
                    params = nmos::make_audio_sdp_parameters(node->data, source->data, flow->data, sender.data, static_cast<uint64_t>(e.payloadType), mids,
                                                             ptpDomain, e.format.ptimeUs / 1000.0);
                    break;
                }
                case config::EssenceType::Anc:
                {
                    auto const& e = g->anc.at(ref->index);
                    params = nmos::make_data_sdp_parameters(node->data, source->data, flow->data, sender.data, static_cast<uint64_t>(e.payloadType), mids,
                                                            ptpDomain, {});
                    break;
                }
            }
            auto const& legs = nmos::fields::transport_params(nmos::fields::endpoint_active(connection.data));
            auto const session = nmos::make_session_description(params, legs);
            transportFile = nmos::make_connection_rtp_sender_transportfile(sdp::make_session_description(session));
        }

        void onActivated(nmos::resource const& resource, nmos::resource const& connection)
        {
            auto const* ref = refOf(connection.id);
            if (ref == nullptr || !_callbacks.activated)
            {
                return;
            }
            Activation a;
            a.essenceUid = ref->essenceUid;
            a.resourceId = *util::parseUuid(connection.id);
            a.sender = ref->sender;
            a.transport = ref->rtp ? "rtp" : "mxl";
            a.active = toNl(nmos::fields::endpoint_active(connection.data));
            _callbacks.activated(a);
            (void)resource;
        }

        void onRegistration(web::uri const& uri)
        {
            bool const now = !uri.is_empty();
            {
                std::lock_guard const lock{_statusMutex};
                _registrationUri = uri.to_string();
            }
            if (_registered.exchange(now) != now)
            {
                log::info(now ? "nmos_registered" : "nmos_unregistered", {{"registration_uri", uri.to_string()}});
            }
        }

        Setup _setup;
        Callbacks _callbacks;
        nmos::node_model _model;
        nmos::experimental::log_model _logModel;
        std::optional<nmos::server> _server;
        bool _open = false;
        nmos::id _nodeId;
        nmos::id _deviceId;
        std::map<std::string, util::Uuid> _domainIds;
        std::map<nmos::id, ResourceRef> _refs;
        std::map<util::Uuid, config::Group> _groups;
        std::atomic<bool> _registered{false};
        mutable std::mutex _statusMutex;
        std::string _registrationUri;
        log::RateLimiter _unknownDomainLog{std::chrono::seconds(30)};
    };

    std::unique_ptr<Node> createNode(Setup setup, Callbacks callbacks)
    {
        return std::make_unique<NodeImpl>(std::move(setup), std::move(callbacks));
    }
}
