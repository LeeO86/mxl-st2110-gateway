// SPDX-License-Identifier: MIT
#include "util/threading.hpp"

#include <pthread.h>
#include <sched.h>

namespace mxlgw::util
{
    void setThreadName(std::string const& name)
    {
        ::pthread_setname_np(::pthread_self(), name.substr(0, 15).c_str());
    }

    bool tryRealtime(int priority)
    {
        sched_param param{};
        param.sched_priority = priority;
        return ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &param) == 0;
    }

    bool pinToCpus(std::set<int> const& cpus)
    {
        if (cpus.empty())
        {
            return true;
        }
        cpu_set_t set;
        CPU_ZERO(&set);
        for (auto const cpu : cpus)
        {
            if (cpu >= 0 && cpu < CPU_SETSIZE)
            {
                CPU_SET(cpu, &set);
            }
        }
        return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
    }
}
