// SPDX-License-Identifier: MIT
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <type_traits>

namespace mxlgw::app
{
    /// The single serialized control thread (§3.6): NMOS activations and admin changes are applied
    /// one after the other, never on an HTTP handler thread.
    class ControlQueue
    {
    public:
        ControlQueue()
            : _thread([this] { run(); })
        {}

        ~ControlQueue() { stop(); }
        ControlQueue(ControlQueue const&) = delete;
        ControlQueue& operator=(ControlQueue const&) = delete;

        void post(std::function<void()> job)
        {
            {
                std::lock_guard const lock{_mutex};
                if (_stop)
                {
                    return;
                }
                _jobs.push_back(std::move(job));
            }
            _cv.notify_one();
        }

        /// Runs `fn` on the control thread and waits for its result (inline if already on it).
        template <typename F>
        auto call(F fn) -> std::invoke_result_t<F>
        {
            using R = std::invoke_result_t<F>;
            if (std::this_thread::get_id() == _thread.get_id())
            {
                return fn();
            }
            auto task = std::make_shared<std::packaged_task<R()>>(std::move(fn));
            auto future = task->get_future();
            post([task] { (*task)(); });
            return future.get();
        }

        bool onControlThread() const { return std::this_thread::get_id() == _thread.get_id(); }

        void stop()
        {
            {
                std::lock_guard const lock{_mutex};
                if (_stop)
                {
                    return;
                }
                _stop = true;
            }
            _cv.notify_all();
            if (_thread.joinable())
            {
                _thread.join();
            }
        }

    private:
        void run()
        {
            std::unique_lock lock{_mutex};
            while (true)
            {
                _cv.wait(lock, [&] { return _stop || !_jobs.empty(); });
                if (_jobs.empty())
                {
                    return;
                }
                auto job = std::move(_jobs.front());
                _jobs.pop_front();
                lock.unlock();
                job();
                lock.lock();
            }
        }

        std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<std::function<void()>> _jobs;
        bool _stop = false;
        std::thread _thread;
    };
}
