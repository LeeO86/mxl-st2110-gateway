// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace mxlgw::timing
{
    using ClockIdentity = std::array<std::uint8_t, 8>;

    /// "08-00-11-ff-fe-21-e1-b0" (NMOS IS-04 clock `gmid` format).
    std::string formatClockIdentity(ClockIdentity const& id);

    struct PortIdentity
    {
        ClockIdentity clock{};
        std::uint16_t port = 0;

        bool operator==(PortIdentity const& o) const { return clock == o.clock && port == o.port; }
        bool operator!=(PortIdentity const& o) const { return !(*this == o); }
        std::string toString() const; // "<clock>/<port>"
    };

    /// Announce data set (IEEE 1588-2008 §13.5).
    struct AnnounceInfo
    {
        ClockIdentity grandmaster{};
        std::uint8_t priority1 = 128;
        std::uint8_t clockClass = 248;
        std::uint8_t clockAccuracy = 0xFE;
        std::uint16_t offsetScaledLogVariance = 0xFFFF;
        std::uint8_t priority2 = 128;
        std::uint16_t stepsRemoved = 0;
        PortIdentity sender;
        std::uint8_t timeSource = 0xA0;
        std::int16_t utcOffset = 37;
        std::uint8_t domain = 127;
    };

    /// Data-set comparison (§5.5): priority1, clockClass, clockAccuracy, offsetScaledLogVariance,
    /// priority2, grandmasterIdentity, stepsRemoved, sender port identity. <0: a better, >0: b better, 0: equal.
    int compareAnnounce(AnnounceInfo const& a, AnnounceInfo const& b);

    /// BMCA across the two ports of a 2022-7 pair (MTL patch 0003 mirrors this logic).
    class DualPortSelector
    {
    public:
        static constexpr int portCount = 2;
        static constexpr int announceTimeoutIntervals = 3;

        /// Records an Announce received on `port` (0 = P, 1 = R). Within a port the best data set wins;
        /// an Announce from the current parent always refreshes it.
        void onAnnounce(int port, AnnounceInfo const& info, std::int64_t nowNs, std::int64_t announceIntervalNs);

        /// Expires parents that sent no Announce for 3 intervals and re-selects. Returns true if the selection changed.
        bool tick(std::int64_t nowNs);

        /// Port that steers the PHC, if any.
        std::optional<int> selected() const { return _selected; }
        std::optional<AnnounceInfo> parent(int port) const;
        std::uint64_t selectionChanges() const { return _changes; }
        std::uint64_t grandmasterChanges() const { return _gmChanges; }

    private:
        struct PortState
        {
            std::optional<AnnounceInfo> best;
            std::int64_t lastAnnounceNs = 0;
            std::int64_t intervalNs = 1'000'000'000;
        };

        bool reselect();

        std::array<PortState, portCount> _ports{};
        std::optional<int> _selected;
        std::optional<ClockIdentity> _lastGrandmaster;
        std::uint64_t _changes = 0;
        std::uint64_t _gmChanges = 0;
    };
}
