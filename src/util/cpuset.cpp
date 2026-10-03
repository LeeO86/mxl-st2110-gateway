// SPDX-License-Identifier: MIT
#include "util/cpuset.hpp"

#include <sched.h>

#include "util/fs.hpp"
#include "util/strings.hpp"

namespace mxlgw::util
{
    std::optional<std::set<int>> parseCpuList(std::string_view text)
    {
        std::set<int> cpus;
        auto const parts = split(text, ',');
        if (parts.empty())
        {
            return std::nullopt;
        }
        for (auto const& part : parts)
        {
            auto const dash = part.find('-');
            if (dash == std::string::npos)
            {
                auto const v = parseInt(part);
                if (!v || *v < 0 || *v > 4095)
                {
                    return std::nullopt;
                }
                cpus.insert(static_cast<int>(*v));
                continue;
            }
            auto const lo = parseInt(part.substr(0, dash));
            auto const hi = parseInt(part.substr(dash + 1));
            if (!lo || !hi || *lo < 0 || *hi < *lo || *hi > 4095)
            {
                return std::nullopt;
            }
            for (auto c = *lo; c <= *hi; ++c)
            {
                cpus.insert(static_cast<int>(c));
            }
        }
        return cpus;
    }

    std::string formatCpuList(std::set<int> const& cpus)
    {
        std::string out;
        auto it = cpus.begin();
        while (it != cpus.end())
        {
            int const start = *it;
            int end = start;
            auto next = std::next(it);
            while (next != cpus.end() && *next == end + 1)
            {
                end = *next;
                ++next;
            }
            if (!out.empty())
            {
                out += ',';
            }
            out += std::to_string(start);
            if (end != start)
            {
                out += '-' + std::to_string(end);
            }
            it = next;
        }
        return out;
    }

    bool disjoint(std::set<int> const& a, std::set<int> const& b)
    {
        for (auto const c : a)
        {
            if (b.count(c) != 0)
            {
                return false;
            }
        }
        return true;
    }

    std::set<int> presentCpus()
    {
        auto const text = readFile("/sys/devices/system/cpu/present");
        if (!text)
        {
            return {};
        }
        auto const parsed = parseCpuList(trim(*text));
        return parsed ? *parsed : std::set<int>{};
    }

    std::set<int> allowedCpus()
    {
        std::set<int> out;
        cpu_set_t set;
        CPU_ZERO(&set);
        if (::sched_getaffinity(0, sizeof(set), &set) != 0)
        {
            return out;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        {
            if (CPU_ISSET(cpu, &set))
            {
                out.insert(cpu);
            }
        }
        return out;
    }
}
