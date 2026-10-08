// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "config/formats.hpp"
#include "config/schema.hpp"
#include "util/uuid.hpp"

namespace mxlgw::config
{
    enum class Direction
    {
        Ingest,
        Egress,
    };
    enum class Backend
    {
        Dpdk,
        Kernel,
        Mock,
    };
    enum class PtpMode
    {
        Builtin,
        BuiltinPhc2sys,
        External,
    };
    // MTL's TX pacing (mtl_init_params.pacing): auto = rate limit where the driver has it, else TSC.
    enum class TxPacing
    {
        Auto,
        Rl,
        Tsc,
    };
    enum class RegistryMode
    {
        DnsSd,
        Static,
    };
    enum class Pacing
    {
        Narrow,
        Linear,
        Wide,
    };
    enum class Packing
    {
        Bpm,
        Gpm,
        GpmSl,
    };
    enum class MissingData
    {
        Black,
        Repeat,
    };
    enum class EssenceType
    {
        Video,
        Audio,
        Anc,
    };

    char const* toName(Direction d);
    char const* toName(Backend b);
    char const* toName(PtpMode m);
    char const* toName(TxPacing p);
    char const* toName(EssenceType t);
    char const* toName(MissingData m);
    char const* toName(Pacing p);
    char const* toName(Packing p);

    struct Leg
    {
        std::string multicast; // ingest: group to join; egress: destination
        std::string source;    // ingest: SSM source (optional)
        int port = 0;
    };

    struct ReadOffset
    {
        std::optional<std::int64_t> grains;
        std::optional<std::int64_t> ns;
        bool isSet() const { return grains.has_value() || ns.has_value(); }
        /// Effective offset in ns for a grain duration.
        std::int64_t toNs(std::int64_t grainDurationNs) const;
    };

    struct EssenceCommon
    {
        util::Uuid uid;
        /// UUIDv5 namespace of the essence's NMOS/MXL ids: `uid`, or UUIDv5(seed namespace, uid) with node.seed (§7.3).
        util::Uuid idNamespace;
        std::string label;
        int payloadType = 96;
        ReadOffset readOffset;
        std::vector<Leg> legs;
    };

    struct VideoEssence : EssenceCommon
    {
        VideoFormat format;
        Packing packing = Packing::Bpm;
        Pacing pacing = Pacing::Narrow;
    };

    struct AudioEssence : EssenceCommon
    {
        AudioFormat format;
    };

    struct AncEssence : EssenceCommon
    {
        AncFormat format;
    };

    struct Group
    {
        util::Uuid uid;
        std::string label;
        Direction direction = Direction::Ingest;
        std::string domain;
        bool redundancy = false;
        bool enabled = true;
        std::optional<std::int64_t> outputDelayNs;
        MissingData missingData = MissingData::Black;
        std::vector<VideoEssence> video;
        std::vector<AudioEssence> audio;
        std::vector<AncEssence> anc;

        std::size_t essenceCount() const { return video.size() + audio.size() + anc.size(); }
        /// Grain duration that drives the group's cadence (first video, else ANC, else audio block).
        std::int64_t cadenceNs() const;
        /// output_delay_ns, or the default: two grains, at least one grain + the largest read offset
        /// + 2 ms (§5.7), so a read offset alone does not make every grain late.
        std::int64_t effectiveOutputDelayNs(std::int64_t maxReadOffsetNs) const;
    };

    struct NicPort
    {
        std::string name;
        std::string pci;
        std::string ifname;
        std::string ip;
        std::string netmask;
        std::string gateway;
    };

    struct PortPair
    {
        std::string name = "media";
        NicPort primary;
        std::optional<NicPort> redundant;
    };

    struct Nic
    {
        Backend backend = Backend::Dpdk;
        std::string lcores;
        int lcoreCount = 4;
        TxPacing txPacing = TxPacing::Auto;
        std::string appCpus;
        std::optional<int> hugepageSocket; // nullopt = auto
        std::vector<PortPair> portPairs;
        // Set at start, not configured: the CPU of DPDK's main lcore (mainLcoreFor), 0 = MTL's default.
        int mainLcore = 0;
    };

    struct Ptp
    {
        PtpMode mode = PtpMode::Builtin;
        int domain = 127;
        bool requireLock = true;
        std::int64_t warnOffsetNs = 10'000;
        std::int64_t maxOffsetNs = 1'000'000;
    };

    struct Domain
    {
        std::string name;
        std::string path;
        std::optional<util::Uuid> id;
        std::string label;
        std::string description;
        std::optional<std::int64_t> historyDurationNs;
        bool gcOnStart = false;
    };

    struct Mxl
    {
        std::optional<std::string> scanPath = std::string("/Volumes/mxl");
        ReadOffset defaultReadOffset;
        std::vector<Domain> domains;
        bool cleanupOnExit = false;

        Domain const* findDomain(std::string const& name) const;
    };

    inline constexpr int defaultRegistrationPort = 3210;

    struct Registry
    {
        bool dnsSd = false;
        std::optional<RegistryMode> legacyMode; // deprecated `mode`, kept for round trips
        std::string address;
        int port = defaultRegistrationPort;
        std::string queryAddress; // "" = address
        std::optional<int> queryPort;

        /// A registry the node is expected to register with (readiness, §10).
        bool configured() const { return dnsSd || !address.empty(); }
        std::string effectiveQueryAddress() const { return queryAddress.empty() ? address : queryAddress; }
        int effectiveQueryPort() const { return queryPort ? *queryPort : port + 1; }
    };

    struct Tls
    {
        bool enabled = false;
        std::string certificate;
        std::string privateKey;
    };

    using Tags = std::map<std::string, std::vector<std::string>>;

    struct St2110Node
    {
        bool enabled = true;
        std::optional<std::string> label;
        std::optional<int> httpPort;
        std::optional<std::string> hostAddress;
        Registry registry;
    };

    struct Node
    {
        std::optional<util::Uuid> id;
        std::optional<std::string> seed;
        std::string label = "mxl-st2110-gateway";
        std::string description = "ST 2110 <-> MXL gateway";
        Tags tags;
        int httpPort = 8080;
        std::optional<int> webPort;
        std::optional<std::string> hostAddress; // node.host_address or the deprecated node.public_address
        std::optional<int> publicPort;
        std::vector<std::string> managementAddresses;
        Registry registry;
        St2110Node st2110;
        Tls tls;
        bool resumeConnections = true;
        std::string logLevel = "info";
        int shutdownTimeoutS = 10;

        int effectiveWebPort() const { return webPort ? *webPort : httpPort; }
        int st2110HttpPort() const { return st2110.httpPort ? *st2110.httpPort : httpPort + 1; }
        std::string st2110Label() const { return st2110.label ? *st2110.label : label + " ST 2110"; }
    };

    struct Config
    {
        int schemaVersion = 1;
        Node node;
        Nic nic;
        Ptp ptp;
        Mxl mxl;
        std::vector<Group> groups;

        Group const* findGroup(util::Uuid const& uid) const;
        /// True when the NIC is not configured yet (setup mode, §9.1).
        bool unconfigured() const;
        /// Read offset of an egress essence, falling back to mxl.default_read_offset_*.
        ReadOffset effectiveReadOffset(EssenceCommon const& essence) const;

        /// UUIDv5 namespace derived from node.seed (§7.3), if set.
        std::optional<util::Uuid> seedNamespace() const;
        /// MXL node id: UUIDv5(seed namespace, "node") with a seed, else node.id (nil while unset).
        util::Uuid mxlNodeId() const;
        /// ST 2110 node id: UUIDv5(seed namespace, "st2110-node") with a seed, else UUIDv5(MXL node id, "st2110-node").
        util::Uuid st2110NodeId() const;
        /// Id a configured domain gets when neither the config nor domain_def.json has one: seed-derived or nullopt.
        std::optional<util::Uuid> seedDomainId(std::string const& domainName) const;
    };

    /// UUIDv5(URL namespace, "urn:x-mxl-st2110-gateway:seed:" + seed).
    util::Uuid seedNamespaceOf(std::string const& seed);

    /// True for an IPv4 literal that may be announced in NMOS (not 0.0.0.0, 127/8, link-local, multicast or broadcast).
    bool isAnnounceableIpv4(std::string const& text);

    /// MTL lcores and worker CPUs after applying the CPU affinity (§4, Kubernetes cpuset).
    struct CpuPlacement
    {
        std::string lcores;
        std::string appCpus;
        bool lcoresDerived = false;
        bool appCpusDerived = false;
    };
    CpuPlacement resolveCpuPlacement(Nic const& nic, std::set<int> const& allowedCpus);

    /// The CPU for DPDK's main lcore. MTL always puts its main lcore first in the EAL core list,
    /// CPU 0 unless set, and EAL pins its init thread there. Without CPU 0 in the affinity (a
    /// Kubernetes Guaranteed pod, CPU 0 reserved) that fails ("EAL: Cannot set affinity"). Returns
    /// 0 (MTL's default) when CPU 0 is allowed or the affinity is unknown, else the first app CPU,
    /// else the first allowed CPU that is no lcore, else the first lcore.
    int mainLcoreFor(std::set<int> const& lcores, std::set<int> const& appCpus, std::set<int> const& allowedCpus);

    /// Converts a schema-valid configuration JSON into the typed model, filling defaults.
    /// Throws std::invalid_argument for type errors (callers validate with the schema first).
    Config fromJson(nlohmann::json const& json);

    /// Serialises the typed model (used by tests, examples and group creation).
    nlohmann::ordered_json toJson(Config const& config);
    nlohmann::ordered_json toJson(Group const& group);
    Group groupFromJson(nlohmann::json const& json);

    /// Semantic rules of §9.5 beyond the JSON Schema. `runtime` adds checks that need the host
    /// (none today; PCI existence is a preflight check).
    ValidationErrors validateSemantics(Config const& config);

    /// Schema + typed conversion + semantic rules.
    struct ParseResult
    {
        std::optional<Config> config;
        ValidationErrors errors;
        bool ok() const { return config.has_value() && errors.empty(); }
    };
    ParseResult parseAndValidate(nlohmann::json const& effectiveJson);

    /// Formats errors as "pointer: message" lines.
    std::string formatErrors(ValidationErrors const& errors);
}
