// SPDX-License-Identifier: MIT
#include "timing/ptp.hpp"

#include <cstdio>

namespace mxlgw::timing
{
    std::string formatClockIdentity(ClockIdentity const& id)
    {
        char buf[3 * 8];
        std::snprintf(buf, sizeof(buf), "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x", id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7]);
        return buf;
    }

    std::string PortIdentity::toString() const
    {
        return formatClockIdentity(clock) + "/" + std::to_string(port);
    }

    int compareAnnounce(AnnounceInfo const& a, AnnounceInfo const& b)
    {
        auto cmp = [](auto x, auto y) { return x < y ? -1 : (x > y ? 1 : 0); };
        if (a.grandmaster != b.grandmaster)
        {
            if (int r = cmp(a.priority1, b.priority1))
            {
                return r;
            }
            if (int r = cmp(a.clockClass, b.clockClass))
            {
                return r;
            }
            if (int r = cmp(a.clockAccuracy, b.clockAccuracy))
            {
                return r;
            }
            if (int r = cmp(a.offsetScaledLogVariance, b.offsetScaledLogVariance))
            {
                return r;
            }
            if (int r = cmp(a.priority2, b.priority2))
            {
                return r;
            }
            return a.grandmaster < b.grandmaster ? -1 : 1;
        }
        // Same grandmaster: fewer steps removed, then lower sender port identity.
        if (int r = cmp(a.stepsRemoved, b.stepsRemoved))
        {
            return r;
        }
        if (a.sender.clock != b.sender.clock)
        {
            return a.sender.clock < b.sender.clock ? -1 : 1;
        }
        return cmp(a.sender.port, b.sender.port);
    }

    void DualPortSelector::onAnnounce(int port, AnnounceInfo const& info, std::int64_t nowNs, std::int64_t announceIntervalNs)
    {
        if (port < 0 || port >= portCount)
        {
            return;
        }
        auto& state = _ports[static_cast<std::size_t>(port)];
        bool const fromParent = state.best && state.best->sender == info.sender;
        if (!state.best || fromParent || compareAnnounce(info, *state.best) < 0)
        {
            state.best = info;
            state.lastAnnounceNs = nowNs;
            state.intervalNs = announceIntervalNs > 0 ? announceIntervalNs : state.intervalNs;
        }
        reselect();
    }

    bool DualPortSelector::tick(std::int64_t nowNs)
    {
        for (auto& state : _ports)
        {
            if (state.best && nowNs - state.lastAnnounceNs > announceTimeoutIntervals * state.intervalNs)
            {
                state.best.reset();
            }
        }
        return reselect();
    }

    std::optional<AnnounceInfo> DualPortSelector::parent(int port) const
    {
        if (port < 0 || port >= portCount)
        {
            return std::nullopt;
        }
        return _ports[static_cast<std::size_t>(port)].best;
    }

    bool DualPortSelector::reselect()
    {
        std::optional<int> best;
        for (int p = 0; p < portCount; ++p)
        {
            auto const& s = _ports[static_cast<std::size_t>(p)];
            if (!s.best)
            {
                continue;
            }
            if (!best)
            {
                best = p;
                continue;
            }
            int const r = compareAnnounce(*s.best, *_ports[static_cast<std::size_t>(*best)].best);
            // Keep the current selection on a tie to avoid flapping between equal legs.
            if (r < 0 || (r == 0 && _selected == p))
            {
                best = p;
            }
        }
        bool const changed = best != _selected;
        if (changed)
        {
            _selected = best;
            ++_changes;
        }
        if (_selected)
        {
            auto const gm = _ports[static_cast<std::size_t>(*_selected)].best->grandmaster;
            if (_lastGrandmaster && *_lastGrandmaster != gm)
            {
                ++_gmChanges;
            }
            _lastGrandmaster = gm;
        }
        return changed;
    }
}
