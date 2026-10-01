// SPDX-License-Identifier: MIT
#include "ops/clock_supervisor.hpp"

#include <algorithm>
#include <limits>

#include "util/threading.hpp"

namespace mxlgw::ops
{
    ClockSupervisor::ClockSupervisor(Clock ptpNow, Clock hostTai, std::chrono::milliseconds interval)
        : _ptpNow(std::move(ptpNow))
        , _hostTai(std::move(hostTai))
        , _interval(interval)
    {}

    ClockSupervisor::~ClockSupervisor()
    {
        stop();
    }

    void ClockSupervisor::start()
    {
        if (_thread.joinable())
        {
            return;
        }
        {
            std::lock_guard const lock{_mutex};
            _stop = false;
        }
        _thread = std::thread([this] { run(); });
    }

    void ClockSupervisor::stop()
    {
        {
            std::lock_guard const lock{_mutex};
            _stop = true;
        }
        _cv.notify_all();
        if (_thread.joinable())
        {
            _thread.join();
        }
    }

    std::int64_t ClockSupervisor::measure(Clock const& ptpNow, Clock const& hostTai, int reads)
    {
        std::int64_t best = 0;
        std::int64_t bestBracket = std::numeric_limits<std::int64_t>::max();
        for (int i = 0; i < std::max(1, reads); ++i)
        {
            auto const before = hostTai();
            auto const ptp = ptpNow();
            auto const after = hostTai();
            auto const bracket = after - before;
            if (bracket < bestBracket)
            {
                bestBracket = bracket;
                best = ptp - (before + bracket / 2);
            }
        }
        return best;
    }

    void ClockSupervisor::addSample(std::int64_t offsetNs, std::chrono::steady_clock::time_point at)
    {
        std::lock_guard const lock{_mutex};
        _window.emplace_back(at, offsetNs);
        while (!_window.empty() && at - _window.front().first > std::chrono::seconds(60))
        {
            _window.pop_front();
        }
        _latest.valid = true;
        _latest.offsetNs = offsetNs;
        _latest.min60Ns = offsetNs;
        _latest.max60Ns = offsetNs;
        for (auto const& [t, v] : _window)
        {
            _latest.min60Ns = std::min(_latest.min60Ns, v);
            _latest.max60Ns = std::max(_latest.max60Ns, v);
        }
    }

    ClockSample ClockSupervisor::latest() const
    {
        std::lock_guard const lock{_mutex};
        return _latest;
    }

    void ClockSupervisor::run()
    {
        util::setThreadName("clock-supervisor");
        std::unique_lock lock{_mutex};
        while (!_stop)
        {
            lock.unlock();
            auto const offset = measure(_ptpNow, _hostTai);
            addSample(offset, std::chrono::steady_clock::now());
            lock.lock();
            _cv.wait_for(lock, _interval, [&] { return _stop; });
        }
    }
}
