// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <map>
#include <optional>
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
        /// output_delay_ns or the default of two grains (§5.7).
        std::int64_t effectiveOutputDelayNs() const;
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
        std::string appCpus;
        std::optional<int> hugepageSocket; // nullopt = auto
        std::vector<PortPair> portPairs;
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

        Domain const* findDomain(std::string const& name) const;
    };

    struct Registry
    {
        RegistryMode mode = RegistryMode::DnsSd;
        std::string address;
        int port = 0;
    };

    struct Tls
    {
        bool enabled = false;
        std::string certificate;
        std::string privateKey;
    };

    struct Node
    {
        std::optional<util::Uuid> id;
        std::string label = "mxl-st2110-gateway";
        std::string description = "ST 2110 <-> MXL gateway";
        int httpPort = 8080;
        std::optional<std::string> publicAddress;
        std::optional<int> publicPort;
        std::vector<std::string> managementAddresses;
        Registry registry;
        Tls tls;
        bool resumeConnections = true;
        std::string logLevel = "info";
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
    };

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
