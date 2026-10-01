// SPDX-License-Identifier: MIT
#include "config/config.hpp"

#include <algorithm>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "util/cpuset.hpp"
#include "util/net.hpp"

namespace mxlgw::config
{
    namespace
    {
        using json = nlohmann::json;

        std::string str(json const& j, char const* key, std::string const& fallback = {})
        {
            if (!j.is_object() || !j.contains(key) || j.at(key).is_null())
            {
                return fallback;
            }
            return j.at(key).get<std::string>();
        }

        template <typename T>
        T num(json const& j, char const* key, T fallback)
        {
            if (!j.is_object() || !j.contains(key) || j.at(key).is_null())
            {
                return fallback;
            }
            return j.at(key).get<T>();
        }

        template <typename T>
        std::optional<T> optNum(json const& j, char const* key)
        {
            if (!j.is_object() || !j.contains(key) || j.at(key).is_null())
            {
                return std::nullopt;
            }
            return j.at(key).get<T>();
        }

        bool boolean(json const& j, char const* key, bool fallback)
        {
            if (!j.is_object() || !j.contains(key) || j.at(key).is_null())
            {
                return fallback;
            }
            return j.at(key).get<bool>();
        }

        std::optional<util::Uuid> optUuid(json const& j, char const* key)
        {
            auto const text = str(j, key);
            if (text.empty())
            {
                return std::nullopt;
            }
            auto const parsed = util::parseUuid(text);
            if (!parsed)
            {
                throw std::invalid_argument(std::string("invalid UUID in ") + key);
            }
            return parsed;
        }

        util::Uuid reqUuid(json const& j, char const* key)
        {
            auto const id = optUuid(j, key);
            if (!id)
            {
                throw std::invalid_argument(std::string("missing UUID ") + key);
            }
            return *id;
        }

        Rational rate(json const& j, char const* key, Rational fallback)
        {
            auto const text = str(j, key);
            if (text.empty())
            {
                return fallback;
            }
            auto const r = util::parseRational(text);
            if (!r)
            {
                throw std::invalid_argument(std::string("invalid rate in ") + key);
            }
            return *r;
        }

        ReadOffset readOffset(json const& j, char const* grainsKey, char const* nsKey)
        {
            ReadOffset r;
            r.grains = optNum<std::int64_t>(j, grainsKey);
            r.ns = optNum<std::int64_t>(j, nsKey);
            return r;
        }

        void readCommon(json const& j, EssenceCommon& e)
        {
            e.uid = reqUuid(j, "uid");
            e.label = str(j, "label");
            e.payloadType = num<int>(j, "payload_type", e.payloadType);
            e.readOffset = readOffset(j, "read_offset_grains", "read_offset_ns");
            if (j.contains("defaults") && j.at("defaults").is_object() && j.at("defaults").contains("legs"))
            {
                for (auto const& legJson : j.at("defaults").at("legs"))
                {
                    Leg leg;
                    leg.multicast = str(legJson, "multicast");
                    leg.source = str(legJson, "source");
                    leg.port = num<int>(legJson, "port", 0);
                    e.legs.push_back(leg);
                }
            }
        }

        template <typename E>
        E enumFrom(std::string const& text, std::initializer_list<std::pair<char const*, E>> table, char const* what)
        {
            for (auto const& [name, value] : table)
            {
                if (text == name)
                {
                    return value;
                }
            }
            throw std::invalid_argument(std::string("invalid ") + what + ": " + text);
        }

        NicPort readPort(json const& j)
        {
            NicPort p;
            p.name = str(j, "name");
            p.pci = str(j, "pci");
            p.ifname = str(j, "ifname");
            p.ip = str(j, "ip");
            p.netmask = str(j, "netmask");
            p.gateway = str(j, "gateway");
            return p;
        }

        void putOpt(nlohmann::ordered_json& j, char const* key, std::optional<std::int64_t> const& v)
        {
            if (v)
            {
                j[key] = *v;
            }
        }

        nlohmann::ordered_json legsJson(std::vector<Leg> const& legs)
        {
            auto arr = nlohmann::ordered_json::array();
            for (auto const& leg : legs)
            {
                nlohmann::ordered_json l;
                l["multicast"] = leg.multicast;
                if (!leg.source.empty())
                {
                    l["source"] = leg.source;
                }
                l["port"] = leg.port;
                arr.push_back(l);
            }
            return arr;
        }

        void writeCommon(nlohmann::ordered_json& j, EssenceCommon const& e)
        {
            j["uid"] = e.uid.toString();
            j["label"] = e.label;
            j["payload_type"] = e.payloadType;
            putOpt(j, "read_offset_grains", e.readOffset.grains);
            putOpt(j, "read_offset_ns", e.readOffset.ns);
        }

        std::string const& pciPattern()
        {
            static std::string const p = "^([0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]|env:[A-Za-z_][A-Za-z0-9_]*)$";
            return p;
        }

        void add(ValidationErrors& errors, std::string pointer, std::string message)
        {
            errors.push_back({std::move(pointer), std::move(message)});
        }

        bool isMirrorPath(std::string const& path)
        {
            auto p = path;
            while (p.size() > 1 && p.back() == '/')
            {
                p.pop_back();
            }
            auto const slash = p.rfind('/');
            auto const base = slash == std::string::npos ? p : p.substr(slash + 1);
            return base.rfind("mirror-", 0) == 0;
        }
    }

    char const* toName(Direction d)
    {
        return d == Direction::Ingest ? "ingest" : "egress";
    }

    char const* toName(Backend b)
    {
        switch (b)
        {
            case Backend::Dpdk: return "dpdk";
            case Backend::Kernel: return "kernel";
            case Backend::Mock: return "mock";
        }
        return "dpdk";
    }

    char const* toName(PtpMode m)
    {
        switch (m)
        {
            case PtpMode::Builtin: return "builtin";
            case PtpMode::BuiltinPhc2sys: return "builtin_phc2sys";
            case PtpMode::External: return "external";
        }
        return "builtin";
    }

    char const* toName(EssenceType t)
    {
        switch (t)
        {
            case EssenceType::Video: return "video";
            case EssenceType::Audio: return "audio";
            case EssenceType::Anc: return "anc";
        }
        return "video";
    }

    char const* toName(MissingData m)
    {
        return m == MissingData::Black ? "black" : "repeat";
    }

    char const* toName(Pacing p)
    {
        switch (p)
        {
            case Pacing::Narrow: return "narrow";
            case Pacing::Linear: return "linear";
            case Pacing::Wide: return "wide";
        }
        return "narrow";
    }

    char const* toName(Packing p)
    {
        switch (p)
        {
            case Packing::Bpm: return "BPM";
            case Packing::Gpm: return "GPM";
            case Packing::GpmSl: return "GPM_SL";
        }
        return "BPM";
    }

    std::int64_t ReadOffset::toNs(std::int64_t grainDurationNs) const
    {
        if (grains)
        {
            return *grains * grainDurationNs;
        }
        if (ns)
        {
            return *ns;
        }
        return 0;
    }

    std::int64_t Group::cadenceNs() const
    {
        if (!video.empty())
        {
            return video.front().format.grainDurationNs();
        }
        if (!anc.empty() && anc.front().format.rate.num > 0)
        {
            return anc.front().format.grainDurationNs();
        }
        if (!audio.empty())
        {
            return audio.front().format.blockDurationNs();
        }
        return periodNs({50, 1});
    }

    std::int64_t Group::effectiveOutputDelayNs() const
    {
        return outputDelayNs ? *outputDelayNs : 2 * cadenceNs();
    }

    Domain const* Mxl::findDomain(std::string const& name) const
    {
        for (auto const& d : domains)
        {
            if (d.name == name)
            {
                return &d;
            }
        }
        return nullptr;
    }

    Group const* Config::findGroup(util::Uuid const& uid) const
    {
        for (auto const& g : groups)
        {
            if (g.uid == uid)
            {
                return &g;
            }
        }
        return nullptr;
    }

    bool Config::unconfigured() const
    {
        return nic.portPairs.empty();
    }

    ReadOffset Config::effectiveReadOffset(EssenceCommon const& essence) const
    {
        return essence.readOffset.isSet() ? essence.readOffset : mxl.defaultReadOffset;
    }

    Group groupFromJson(json const& g)
    {
        Group group;
        group.uid = reqUuid(g, "uid");
        group.label = str(g, "label");
        group.direction = enumFrom<Direction>(str(g, "direction", "ingest"), {{"ingest", Direction::Ingest}, {"egress", Direction::Egress}}, "direction");
        group.domain = str(g, "domain");
        group.redundancy = boolean(g, "redundancy", false);
        group.enabled = boolean(g, "enabled", true);
        group.outputDelayNs = optNum<std::int64_t>(g, "output_delay_ns");
        group.missingData =
            enumFrom<MissingData>(str(g, "missing_data", "black"), {{"black", MissingData::Black}, {"repeat", MissingData::Repeat}}, "missing_data");

        if (g.contains("video"))
        {
            for (auto const& v : g.at("video"))
            {
                VideoEssence e;
                readCommon(v, e);
                e.format.width = num<int>(v, "width", 1920);
                e.format.height = num<int>(v, "height", 1080);
                e.format.rate = rate(v, "rate", {50, 1});
                auto const il = parseInterlace(str(v, "interlace", "progressive"));
                if (!il)
                {
                    throw std::invalid_argument("invalid interlace");
                }
                e.format.interlace = *il;
                e.format.colorimetry = str(v, "colorimetry", "BT709");
                e.format.tcs = str(v, "tcs", "SDR");
                e.packing = enumFrom<Packing>(str(v, "packing", "BPM"), {{"BPM", Packing::Bpm}, {"GPM", Packing::Gpm}, {"GPM_SL", Packing::GpmSl}}, "packing");
                e.pacing =
                    enumFrom<Pacing>(str(v, "pacing", "narrow"), {{"narrow", Pacing::Narrow}, {"linear", Pacing::Linear}, {"wide", Pacing::Wide}}, "pacing");
                if (e.payloadType == 96 && !v.contains("payload_type"))
                {
                    e.payloadType = 96;
                }
                group.video.push_back(std::move(e));
            }
        }
        if (g.contains("audio"))
        {
            for (auto const& a : g.at("audio"))
            {
                AudioEssence e;
                e.payloadType = 97;
                readCommon(a, e);
                e.format.channels = num<int>(a, "channels", 2);
                e.format.bitDepth = num<int>(a, "bit_depth", 24);
                e.format.sampleRate = num<int>(a, "sample_rate", 48000);
                e.format.ptimeUs = num<int>(a, "ptime_us", 1000);
                e.format.blockUs = num<int>(a, "block_us", e.format.ptimeUs >= 1000 ? e.format.ptimeUs : 1000);
                group.audio.push_back(std::move(e));
            }
        }
        if (g.contains("anc"))
        {
            for (auto const& n : g.at("anc"))
            {
                AncEssence e;
                e.payloadType = 100;
                readCommon(n, e);
                Rational const fallback = group.video.empty() ? Rational{0, 1} : group.video.front().format.rate;
                e.format.rate = n.contains("rate") && !n.at("rate").is_null() ? rate(n, "rate", fallback) : fallback;
                auto const ilText = str(n, "interlace");
                if (!ilText.empty())
                {
                    auto const il = parseInterlace(ilText);
                    if (!il)
                    {
                        throw std::invalid_argument("invalid interlace");
                    }
                    e.format.interlace = *il;
                }
                else if (!group.video.empty())
                {
                    e.format.interlace = group.video.front().format.interlace;
                }
                group.anc.push_back(std::move(e));
            }
        }
        return group;
    }

    Config fromJson(json const& j)
    {
        Config c;
        c.schemaVersion = num<int>(j, "schema_version", 1);

        if (j.contains("node"))
        {
            auto const& n = j.at("node");
            c.node.id = optUuid(n, "id");
            c.node.label = str(n, "label", c.node.label);
            c.node.description = str(n, "description", c.node.description);
            c.node.httpPort = num<int>(n, "http_port", c.node.httpPort);
            if (auto const a = str(n, "public_address"); !a.empty())
            {
                c.node.publicAddress = a;
            }
            c.node.publicPort = optNum<int>(n, "public_port");
            if (n.contains("management_addresses"))
            {
                for (auto const& a : n.at("management_addresses"))
                {
                    c.node.managementAddresses.push_back(a.get<std::string>());
                }
            }
            if (n.contains("registry") && n.at("registry").is_object())
            {
                auto const& r = n.at("registry");
                c.node.registry.mode =
                    enumFrom<RegistryMode>(str(r, "mode", "dns-sd"), {{"dns-sd", RegistryMode::DnsSd}, {"static", RegistryMode::Static}}, "registry mode");
                c.node.registry.address = str(r, "address");
                c.node.registry.port = num<int>(r, "port", 0);
            }
            if (n.contains("tls") && n.at("tls").is_object())
            {
                auto const& t = n.at("tls");
                c.node.tls.enabled = boolean(t, "enabled", false);
                c.node.tls.certificate = str(t, "certificate");
                c.node.tls.privateKey = str(t, "private_key");
            }
            c.node.resumeConnections = boolean(n, "resume_connections", true);
            c.node.logLevel = str(n, "log_level", "info");
        }

        if (j.contains("nic"))
        {
            auto const& n = j.at("nic");
            c.nic.backend =
                enumFrom<Backend>(str(n, "backend", "dpdk"), {{"dpdk", Backend::Dpdk}, {"kernel", Backend::Kernel}, {"mock", Backend::Mock}}, "backend");
            c.nic.lcores = str(n, "lcores");
            c.nic.appCpus = str(n, "app_cpus");
            if (n.contains("hugepage_socket") && n.at("hugepage_socket").is_number_integer())
            {
                c.nic.hugepageSocket = n.at("hugepage_socket").get<int>();
            }
            if (n.contains("port_pairs"))
            {
                for (auto const& pp : n.at("port_pairs"))
                {
                    PortPair pair;
                    pair.name = str(pp, "name", "media");
                    pair.primary = readPort(pp.at("primary"));
                    if (pp.contains("redundant") && pp.at("redundant").is_object())
                    {
                        pair.redundant = readPort(pp.at("redundant"));
                    }
                    c.nic.portPairs.push_back(pair);
                }
            }
        }

        if (j.contains("ptp"))
        {
            auto const& p = j.at("ptp");
            c.ptp.mode =
                enumFrom<PtpMode>(str(p, "mode", "builtin"),
                                  {{"builtin", PtpMode::Builtin}, {"builtin_phc2sys", PtpMode::BuiltinPhc2sys}, {"external", PtpMode::External}}, "ptp mode");
            c.ptp.domain = num<int>(p, "domain", c.ptp.domain);
            c.ptp.requireLock = boolean(p, "require_lock", c.ptp.requireLock);
            c.ptp.warnOffsetNs = num<std::int64_t>(p, "warn_offset_ns", c.ptp.warnOffsetNs);
            c.ptp.maxOffsetNs = num<std::int64_t>(p, "max_offset_ns", c.ptp.maxOffsetNs);
        }

        if (j.contains("mxl"))
        {
            auto const& m = j.at("mxl");
            if (m.contains("scan_path"))
            {
                if (m.at("scan_path").is_null())
                {
                    c.mxl.scanPath.reset();
                }
                else
                {
                    c.mxl.scanPath = m.at("scan_path").get<std::string>();
                }
            }
            c.mxl.defaultReadOffset = readOffset(m, "default_read_offset_grains", "default_read_offset_ns");
            if (m.contains("domains"))
            {
                for (auto const& d : m.at("domains"))
                {
                    Domain domain;
                    domain.name = str(d, "name");
                    domain.path = str(d, "path");
                    domain.id = optUuid(d, "id");
                    domain.label = str(d, "label", domain.name);
                    domain.description = str(d, "description", domain.label);
                    domain.historyDurationNs = optNum<std::int64_t>(d, "history_duration_ns");
                    domain.gcOnStart = boolean(d, "gc_on_start", false);
                    c.mxl.domains.push_back(domain);
                }
            }
        }

        if (j.contains("groups"))
        {
            for (auto const& g : j.at("groups"))
            {
                c.groups.push_back(groupFromJson(g));
            }
        }
        return c;
    }

    nlohmann::ordered_json toJson(Group const& g)
    {
        nlohmann::ordered_json j;
        j["uid"] = g.uid.toString();
        j["label"] = g.label;
        j["direction"] = toName(g.direction);
        j["domain"] = g.domain;
        j["redundancy"] = g.redundancy;
        j["enabled"] = g.enabled;
        if (g.direction == Direction::Egress)
        {
            j["output_delay_ns"] = g.outputDelayNs ? nlohmann::ordered_json(*g.outputDelayNs) : nlohmann::ordered_json(nullptr);
            j["missing_data"] = toName(g.missingData);
        }
        j["video"] = nlohmann::ordered_json::array();
        for (auto const& v : g.video)
        {
            nlohmann::ordered_json e;
            writeCommon(e, v);
            e["width"] = v.format.width;
            e["height"] = v.format.height;
            e["rate"] = v.format.rate.toString();
            e["interlace"] = toName(v.format.interlace);
            e["colorimetry"] = v.format.colorimetry;
            e["tcs"] = v.format.tcs;
            e["packing"] = toName(v.packing);
            if (g.direction == Direction::Egress)
            {
                e["pacing"] = toName(v.pacing);
            }
            e["defaults"] = {{"legs", legsJson(v.legs)}};
            j["video"].push_back(e);
        }
        j["audio"] = nlohmann::ordered_json::array();
        for (auto const& a : g.audio)
        {
            nlohmann::ordered_json e;
            writeCommon(e, a);
            e["channels"] = a.format.channels;
            e["bit_depth"] = a.format.bitDepth;
            e["sample_rate"] = a.format.sampleRate;
            e["ptime_us"] = a.format.ptimeUs;
            e["block_us"] = a.format.blockUs;
            e["defaults"] = {{"legs", legsJson(a.legs)}};
            j["audio"].push_back(e);
        }
        j["anc"] = nlohmann::ordered_json::array();
        for (auto const& n : g.anc)
        {
            nlohmann::ordered_json e;
            writeCommon(e, n);
            if (n.format.rate.num > 0)
            {
                e["rate"] = n.format.rate.toString();
            }
            e["interlace"] = toName(n.format.interlace);
            e["defaults"] = {{"legs", legsJson(n.legs)}};
            j["anc"].push_back(e);
        }
        return j;
    }

    nlohmann::ordered_json toJson(Config const& c)
    {
        nlohmann::ordered_json j;
        j["schema_version"] = c.schemaVersion;
        auto& n = j["node"];
        n["id"] = c.node.id ? nlohmann::ordered_json(c.node.id->toString()) : nlohmann::ordered_json(nullptr);
        n["label"] = c.node.label;
        n["description"] = c.node.description;
        n["http_port"] = c.node.httpPort;
        n["public_address"] = c.node.publicAddress ? nlohmann::ordered_json(*c.node.publicAddress) : nlohmann::ordered_json(nullptr);
        n["public_port"] = c.node.publicPort ? nlohmann::ordered_json(*c.node.publicPort) : nlohmann::ordered_json(nullptr);
        n["management_addresses"] = c.node.managementAddresses;
        n["registry"]["mode"] = c.node.registry.mode == RegistryMode::DnsSd ? "dns-sd" : "static";
        if (c.node.registry.mode == RegistryMode::Static)
        {
            n["registry"]["address"] = c.node.registry.address;
            n["registry"]["port"] = c.node.registry.port;
        }
        n["tls"]["enabled"] = c.node.tls.enabled;
        n["resume_connections"] = c.node.resumeConnections;
        n["log_level"] = c.node.logLevel;

        auto& nic = j["nic"];
        nic["backend"] = toName(c.nic.backend);
        nic["lcores"] = c.nic.lcores.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(c.nic.lcores);
        nic["app_cpus"] = c.nic.appCpus.empty() ? nlohmann::ordered_json(nullptr) : nlohmann::ordered_json(c.nic.appCpus);
        nic["hugepage_socket"] = c.nic.hugepageSocket ? nlohmann::ordered_json(*c.nic.hugepageSocket) : nlohmann::ordered_json("auto");
        nic["port_pairs"] = nlohmann::ordered_json::array();
        auto portJson = [](NicPort const& p)
        {
            nlohmann::ordered_json o;
            o["name"] = p.name;
            if (!p.pci.empty())
            {
                o["pci"] = p.pci;
            }
            if (!p.ifname.empty())
            {
                o["ifname"] = p.ifname;
            }
            if (!p.ip.empty())
            {
                o["ip"] = p.ip;
            }
            if (!p.netmask.empty())
            {
                o["netmask"] = p.netmask;
            }
            if (!p.gateway.empty())
            {
                o["gateway"] = p.gateway;
            }
            return o;
        };
        for (auto const& pp : c.nic.portPairs)
        {
            nlohmann::ordered_json o;
            o["name"] = pp.name;
            o["primary"] = portJson(pp.primary);
            if (pp.redundant)
            {
                o["redundant"] = portJson(*pp.redundant);
            }
            nic["port_pairs"].push_back(o);
        }

        auto& p = j["ptp"];
        p["mode"] = toName(c.ptp.mode);
        p["domain"] = c.ptp.domain;
        p["require_lock"] = c.ptp.requireLock;
        p["warn_offset_ns"] = c.ptp.warnOffsetNs;
        p["max_offset_ns"] = c.ptp.maxOffsetNs;

        auto& m = j["mxl"];
        m["scan_path"] = c.mxl.scanPath ? nlohmann::ordered_json(*c.mxl.scanPath) : nlohmann::ordered_json(nullptr);
        putOpt(m, "default_read_offset_grains", c.mxl.defaultReadOffset.grains);
        putOpt(m, "default_read_offset_ns", c.mxl.defaultReadOffset.ns);
        m["domains"] = nlohmann::ordered_json::array();
        for (auto const& d : c.mxl.domains)
        {
            nlohmann::ordered_json o;
            o["name"] = d.name;
            o["path"] = d.path;
            o["id"] = d.id ? nlohmann::ordered_json(d.id->toString()) : nlohmann::ordered_json(nullptr);
            o["label"] = d.label;
            o["description"] = d.description;
            putOpt(o, "history_duration_ns", d.historyDurationNs);
            o["gc_on_start"] = d.gcOnStart;
            m["domains"].push_back(o);
        }

        j["groups"] = nlohmann::ordered_json::array();
        for (auto const& g : c.groups)
        {
            j["groups"].push_back(toJson(g));
        }
        return j;
    }

    ValidationErrors validateSemantics(Config const& c)
    {
        ValidationErrors errors;
        static std::regex const pciRe{pciPattern()};

        // node
        if (c.node.registry.mode == RegistryMode::Static)
        {
            if (c.node.registry.address.empty())
            {
                add(errors, "/node/registry/address", "is required for a static registry");
            }
            if (c.node.registry.port <= 0)
            {
                add(errors, "/node/registry/port", "is required for a static registry");
            }
        }
        if (c.node.tls.enabled && (c.node.tls.certificate.empty() || c.node.tls.privateKey.empty()))
        {
            add(errors, "/node/tls", "certificate and private_key are required when TLS is enabled");
        }

        // nic
        std::set<std::string> portNames;
        for (std::size_t i = 0; i < c.nic.portPairs.size(); ++i)
        {
            auto const& pp = c.nic.portPairs[i];
            auto const base = "/nic/port_pairs/" + std::to_string(i);
            auto checkPort = [&](NicPort const& port, std::string const& ptr)
            {
                if (!portNames.insert(port.name).second)
                {
                    add(errors, ptr + "/name", "duplicate port name '" + port.name + "'");
                }
                if (c.nic.backend == Backend::Dpdk)
                {
                    if (port.pci.empty())
                    {
                        add(errors, ptr + "/pci", "is required for nic.backend = dpdk");
                    }
                    else if (!std::regex_match(port.pci, pciRe))
                    {
                        add(errors, ptr + "/pci", "must be a PCI address (dddd:bb:dd.f) or env:VARIABLE");
                    }
                    if (port.ip.empty())
                    {
                        add(errors, ptr + "/ip", "is required for nic.backend = dpdk (the port has no kernel IP configuration)");
                    }
                    if (port.netmask.empty())
                    {
                        add(errors, ptr + "/netmask", "is required for nic.backend = dpdk");
                    }
                }
                if (c.nic.backend == Backend::Kernel && port.ifname.empty())
                {
                    add(errors, ptr + "/ifname", "is required for nic.backend = kernel");
                }
                auto const ip = util::parseIpv4(port.ip);
                auto const mask = util::parseIpv4(port.netmask);
                if (!port.netmask.empty() && mask && !util::isNetmask(*mask))
                {
                    add(errors, ptr + "/netmask", "is not a contiguous netmask");
                }
                if (!port.gateway.empty() && ip && mask)
                {
                    auto const gw = util::parseIpv4(port.gateway);
                    if (gw && !util::sameSubnet(*ip, *gw, *mask))
                    {
                        add(errors, ptr + "/gateway", "is not in the port's subnet");
                    }
                }
                if (ip && util::isMulticast(*ip))
                {
                    add(errors, ptr + "/ip", "must not be a multicast address");
                }
            };
            checkPort(pp.primary, base + "/primary");
            if (pp.redundant)
            {
                checkPort(*pp.redundant, base + "/redundant");
            }
        }

        std::optional<std::set<int>> lcores;
        std::optional<std::set<int>> appCpus;
        if (!c.nic.lcores.empty())
        {
            lcores = util::parseCpuList(c.nic.lcores);
            if (!lcores)
            {
                add(errors, "/nic/lcores", "is not a valid CPU list (e.g. \"4-9\")");
            }
        }
        if (!c.nic.appCpus.empty())
        {
            appCpus = util::parseCpuList(c.nic.appCpus);
            if (!appCpus)
            {
                add(errors, "/nic/app_cpus", "is not a valid CPU list (e.g. \"10-15\")");
            }
        }
        if (lcores && appCpus && !util::disjoint(*lcores, *appCpus))
        {
            add(errors, "/nic/app_cpus", "must be disjoint from nic.lcores");
        }

        // ptp
        if (c.ptp.warnOffsetNs > c.ptp.maxOffsetNs)
        {
            add(errors, "/ptp/warn_offset_ns", "must not exceed ptp.max_offset_ns");
        }

        // mxl
        if (c.mxl.defaultReadOffset.grains && c.mxl.defaultReadOffset.ns)
        {
            add(errors, "/mxl/default_read_offset_ns", "default_read_offset_grains and default_read_offset_ns are mutually exclusive");
        }
        std::set<std::string> domainNames;
        std::set<std::string> domainPaths;
        for (std::size_t i = 0; i < c.mxl.domains.size(); ++i)
        {
            auto const& d = c.mxl.domains[i];
            auto const ptr = "/mxl/domains/" + std::to_string(i);
            if (!domainNames.insert(d.name).second)
            {
                add(errors, ptr + "/name", "duplicate domain name '" + d.name + "'");
            }
            if (!domainPaths.insert(d.path).second)
            {
                add(errors, ptr + "/path", "duplicate domain path '" + d.path + "'");
            }
            if (isMirrorPath(d.path))
            {
                add(errors, ptr + "/path", "must not be a mirror domain (mirror-*), the gateway never writes into mirror domains");
            }
        }

        // groups
        bool anyRedundant = false;
        std::set<std::string> groupLabels;
        std::set<util::Uuid> uids;
        std::set<std::pair<std::string, int>> egressDestinations;
        for (std::size_t gi = 0; gi < c.groups.size(); ++gi)
        {
            auto const& g = c.groups[gi];
            auto const gptr = "/groups/" + std::to_string(gi);
            anyRedundant = anyRedundant || g.redundancy;
            if (!groupLabels.insert(g.label).second)
            {
                add(errors, gptr + "/label", "duplicate group label '" + g.label + "'");
            }
            if (!uids.insert(g.uid).second)
            {
                add(errors, gptr + "/uid", "duplicate uid");
            }
            if (!c.mxl.findDomain(g.domain))
            {
                add(errors, gptr + "/domain", "refers to unknown domain '" + g.domain + "'");
            }
            if (g.essenceCount() == 0)
            {
                add(errors, gptr, "must contain at least one essence");
            }
            if (g.direction == Direction::Ingest && g.outputDelayNs)
            {
                add(errors, gptr + "/output_delay_ns", "only applies to egress groups");
            }

            std::int64_t maxReadOffsetNs = 0;
            auto const cadence = g.cadenceNs();
            auto checkCommon = [&](EssenceCommon const& e, std::string const& ptr)
            {
                if (!uids.insert(e.uid).second)
                {
                    add(errors, ptr + "/uid", "duplicate uid");
                }
                if (e.readOffset.grains && e.readOffset.ns)
                {
                    add(errors, ptr + "/read_offset_ns", "read_offset_grains and read_offset_ns are mutually exclusive");
                }
                if (g.direction == Direction::Ingest && e.readOffset.isSet())
                {
                    add(errors, ptr + "/read_offset_grains", "read offsets only apply to egress essences (MXL Receivers)");
                }
                if (g.direction == Direction::Egress)
                {
                    maxReadOffsetNs = std::max(maxReadOffsetNs, c.effectiveReadOffset(e).toNs(cadence));
                }
                std::size_t const maxLegs = g.redundancy ? 2 : 1;
                if (e.legs.size() > maxLegs)
                {
                    add(errors, ptr + "/defaults/legs", "a second leg requires group redundancy");
                }
                for (std::size_t li = 0; li < e.legs.size(); ++li)
                {
                    auto const& leg = e.legs[li];
                    auto const lptr = ptr + "/defaults/legs/" + std::to_string(li);
                    auto const ip = util::parseIpv4(leg.multicast);
                    if (!ip || !util::isMulticast(*ip))
                    {
                        add(errors, lptr + "/multicast", "must be a multicast address (224.0.0.0/4)");
                    }
                    if (leg.port <= 0)
                    {
                        add(errors, lptr + "/port", "is required");
                    }
                    if (g.direction == Direction::Egress && ip && leg.port > 0 && !egressDestinations.insert({leg.multicast, leg.port}).second)
                    {
                        add(errors, lptr, "duplicate egress destination " + leg.multicast + ":" + std::to_string(leg.port));
                    }
                }
            };

            for (std::size_t i = 0; i < g.video.size(); ++i)
            {
                auto const& v = g.video[i];
                auto const ptr = gptr + "/video/" + std::to_string(i);
                checkCommon(v, ptr);
                bool const sizeOk = (v.format.width == 1920 && v.format.height == 1080) || (v.format.width == 3840 && v.format.height == 2160);
                if (!sizeOk)
                {
                    add(errors, ptr + "/width", "video size must be 1920x1080 or 3840x2160");
                }
                if (!isSupportedVideoRate(v.format.rate))
                {
                    add(errors, ptr + "/rate", "unsupported rate " + v.format.rate.toString());
                }
                if (v.format.interlaced() && (!interlaceAllowed(v.format.rate) || v.format.height != 1080))
                {
                    add(errors, ptr + "/interlace", "interlace is only allowed for 1080-line formats at 25/1 or 30000/1001");
                }
            }
            for (std::size_t i = 0; i < g.audio.size(); ++i)
            {
                auto const& a = g.audio[i];
                auto const ptr = gptr + "/audio/" + std::to_string(i);
                checkCommon(a, ptr);
                if (a.format.ptimeUs != 1000 && a.format.ptimeUs != 125)
                {
                    add(errors, ptr + "/ptime_us", "must be 1000 or 125");
                }
                else
                {
                    if (a.format.blockUs < a.format.ptimeUs || a.format.blockUs % a.format.ptimeUs != 0)
                    {
                        add(errors, ptr + "/block_us", "must be a multiple of ptime_us");
                    }
                    if (a.format.channels > AudioFormat::maxChannels(a.format.ptimeUs))
                    {
                        add(errors, ptr + "/channels",
                            "at most " + std::to_string(AudioFormat::maxChannels(a.format.ptimeUs)) + " channels for ptime " +
                                std::to_string(a.format.ptimeUs) + " us (ST 2110-30 levels A/B/C)");
                    }
                }
            }
            for (std::size_t i = 0; i < g.anc.size(); ++i)
            {
                auto const& n = g.anc[i];
                auto const ptr = gptr + "/anc/" + std::to_string(i);
                checkCommon(n, ptr);
                if (n.format.rate.num <= 0)
                {
                    add(errors, ptr + "/rate", "is required when the group has no video essence");
                }
                else if (!isSupportedVideoRate(n.format.rate))
                {
                    add(errors, ptr + "/rate", "unsupported rate " + n.format.rate.toString());
                }
                else if (n.format.interlace != Interlace::Progressive && !interlaceAllowed(n.format.rate))
                {
                    add(errors, ptr + "/interlace", "interlace is only allowed at 25/1 or 30000/1001");
                }
            }

            if (g.direction == Direction::Egress)
            {
                // The group's worker reads one grain of every video/ANC essence per cadence period (§5.7).
                std::optional<util::Rational> groupRate;
                auto sameCadence = [&](util::Rational r, std::string const& ptr)
                {
                    if (r.num <= 0)
                    {
                        return;
                    }
                    if (!groupRate)
                    {
                        groupRate = r;
                    }
                    else if (static_cast<__int128>(r.num) * groupRate->den != static_cast<__int128>(groupRate->num) * r.den)
                    {
                        add(errors, ptr + "/rate",
                            "all video and ANC essences of an egress group must have the same grain rate (" + groupRate->toString() + ")");
                    }
                };
                for (std::size_t i = 0; i < g.video.size(); ++i)
                {
                    sameCadence(g.video[i].format.grainRate(), gptr + "/video/" + std::to_string(i));
                }
                for (std::size_t i = 0; i < g.anc.size(); ++i)
                {
                    sameCadence(g.anc[i].format.grainRate(), gptr + "/anc/" + std::to_string(i));
                }
            }

            if (g.direction == Direction::Egress && g.outputDelayNs)
            {
                auto const minimum = cadence + maxReadOffsetNs + 2'000'000;
                if (*g.outputDelayNs < minimum)
                {
                    add(errors, gptr + "/output_delay_ns", "must be at least one grain + the largest read offset + 2 ms (" + std::to_string(minimum) + " ns)");
                }
            }
        }
        if (anyRedundant)
        {
            bool const hasRedundantPort = !c.nic.portPairs.empty() && c.nic.portPairs.front().redundant.has_value();
            if (!hasRedundantPort && !c.nic.portPairs.empty())
            {
                add(errors, "/nic/port_pairs/0/redundant", "is required because a group has redundancy enabled");
            }
        }
        return errors;
    }

    ParseResult parseAndValidate(json const& effectiveJson)
    {
        ParseResult result;
        result.errors = gatewaySchema().validate(effectiveJson);
        if (!result.errors.empty())
        {
            return result;
        }
        try
        {
            result.config = fromJson(effectiveJson);
        }
        catch (std::exception const& ex)
        {
            result.errors.push_back({"", ex.what()});
            return result;
        }
        result.errors = validateSemantics(*result.config);
        return result;
    }

    std::string formatErrors(ValidationErrors const& errors)
    {
        std::ostringstream out;
        for (auto const& e : errors)
        {
            out << (e.pointer.empty() ? "/" : e.pointer) << ": " << e.message << "\n";
        }
        return out.str();
    }
}
