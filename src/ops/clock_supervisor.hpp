// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace mxlgw::ops
{
    struct ClockSample
    {
        bool valid = false;
        std::int64_t offsetNs = 0; // MTL PTP time − host CLOCK_TAI
        std::int64_t min60Ns = 0;
        std::int64_t max60Ns = 0;
    };

    /// §5.3: samples d = mtl_ptp_read_time − CLOCK_TAI every second (min-delay of several reads, like
    /// MTL phc2sys_adjust) and keeps the 60 s min/max for /metrics and /readyz.
    class ClockSupervisor
    {
    public:
        using Clock = std::function<std::int64_t()>;

        ClockSupervisor(Clock ptpNow, Clock hostTai, std::chrono::milliseconds interval = std::chrono::seconds(1));
        ~ClockSupervisor();
        ClockSupervisor(ClockSupervisor const&) = delete;
        ClockSupervisor& operator=(ClockSupervisor const&) = delete;

        void start();
        void stop();
        ClockSample latest() const;
        /// One measurement: the sample with the smallest host-read bracket of `reads` attempts.
        static std::int64_t measure(Clock const& ptpNow, Clock const& hostTai, int reads = 5);
        /// Adds a sample (exposed for tests).
        void addSample(std::int64_t offsetNs, std::chrono::steady_clock::time_point at);

    private:
        void run();

        Clock _ptpNow;
        Clock _hostTai;
        std::chrono::milliseconds _interval;
        mutable std::mutex _mutex;
        std::condition_variable _cv;
        bool _stop = false;
        std::deque<std::pair<std::chrono::steady_clock::time_point, std::int64_t>> _window;
        ClockSample _latest;
        std::thread _thread;
    };
}
