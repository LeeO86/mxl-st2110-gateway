// SPDX-License-Identifier: MIT
#include "mtl/backend.hpp"

#include <cerrno>
#include <ctime>

namespace mxlgw::media
{
    std::vector<LegAddress> legsFromConfig(std::vector<config::Leg> const& legs, bool redundant)
    {
        std::vector<LegAddress> out;
        for (std::size_t i = 0; i < legs.size() && i < (redundant ? 2u : 1u); ++i)
        {
            LegAddress leg;
            leg.destination = legs[i].multicast;
            leg.source = legs[i].source;
            leg.port = legs[i].port;
            leg.enabled = true;
            out.push_back(leg);
        }
        return out;
    }

    std::int64_t hostTaiNs()
    {
        timespec ts{};
        ::clock_gettime(CLOCK_TAI, &ts);
        return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    void sleepUntilTai(std::int64_t tai)
    {
        timespec ts{};
        ts.tv_sec = tai / 1'000'000'000LL;
        ts.tv_nsec = tai % 1'000'000'000LL;
        while (::clock_nanosleep(CLOCK_TAI, TIMER_ABSTIME, &ts, nullptr) == EINTR)
        {
        }
    }
}
