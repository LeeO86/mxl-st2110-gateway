// SPDX-License-Identifier: MIT
#include "util/logging.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <thread>

namespace mxlgw::log
{
    namespace
    {
        constexpr std::size_t historyCapacity = 500;
        constexpr std::size_t ringCapacity = 4096; // power of two; MTL init logs a burst of several hundred lines
        constexpr std::size_t slotTextSize = 400;

        struct State
        {
            std::atomic<int> level{static_cast<int>(Level::Info)};
            std::atomic<int> format{static_cast<int>(Format::Json)};
            std::mutex outMutex;
            std::deque<std::string> history;
        };

        State& state()
        {
            static State s;
            return s;
        }

        // Bounded MPMC queue (D. Vyukov) with fixed-size text slots: no allocation, no locks.
        struct Slot
        {
            std::atomic<std::size_t> sequence{0};
            int level = 0;
            char component[16]{};
            char text[slotTextSize]{};
        };

        struct Ring
        {
            std::array<Slot, ringCapacity> slots;
            alignas(64) std::atomic<std::size_t> enqueuePos{0};
            alignas(64) std::atomic<std::size_t> dequeuePos{0};
            std::atomic<std::uint64_t> dropped{0};

            Ring()
            {
                for (std::size_t i = 0; i < ringCapacity; ++i)
                {
                    slots[i].sequence.store(i, std::memory_order_relaxed);
                }
            }
        };

        Ring& ring()
        {
            static Ring r;
            return r;
        }

        struct Drain
        {
            std::mutex mutex;
            std::condition_variable cv;
            std::thread thread;
            bool running = false;
            bool stop = false;
        };

        Drain& drain()
        {
            static Drain d;
            return d;
        }

        std::atomic<RealtimeObserver> realtimeObserver{nullptr};

        std::string timestampUtc()
        {
            auto const now = std::chrono::system_clock::now();
            auto const secs = std::chrono::system_clock::to_time_t(now);
            auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
            std::tm tm{};
            gmtime_r(&secs, &tm);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
                          tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100, static_cast<int>(ms % 1000));
            return buf;
        }

        std::string render(Level lvl, std::string_view event, Fields const& fields, Format fmt)
        {
            if (fmt == Format::Json)
            {
                nlohmann::ordered_json line;
                line["ts"] = timestampUtc();
                line["level"] = levelName(lvl);
                line["event"] = std::string(event);
                if (fields.is_object())
                {
                    for (auto it = fields.begin(); it != fields.end(); ++it)
                    {
                        line[it.key()] = it.value();
                    }
                }
                return line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            }
            std::string out = timestampUtc();
            out += ' ';
            out += levelName(lvl);
            out += ' ';
            out += event;
            if (fields.is_object())
            {
                for (auto it = fields.begin(); it != fields.end(); ++it)
                {
                    out += ' ';
                    out += it.key();
                    out += '=';
                    out += it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                }
            }
            return out;
        }

        void emit(std::string line)
        {
            auto& s = state();
            std::lock_guard const lock{s.outMutex};
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fputc('\n', stdout);
            std::fflush(stdout);
            s.history.push_back(std::move(line));
            while (s.history.size() > historyCapacity)
            {
                s.history.pop_front();
            }
        }

        bool tryDequeue(Slot& out)
        {
            auto& r = ring();
            std::size_t pos = r.dequeuePos.load(std::memory_order_relaxed);
            for (;;)
            {
                Slot& slot = r.slots[pos & (ringCapacity - 1)];
                std::size_t const seq = slot.sequence.load(std::memory_order_acquire);
                auto const diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos + 1);
                if (diff == 0)
                {
                    if (r.dequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                    {
                        out.level = slot.level;
                        std::memcpy(out.component, slot.component, sizeof(out.component));
                        std::memcpy(out.text, slot.text, sizeof(out.text));
                        slot.sequence.store(pos + ringCapacity, std::memory_order_release);
                        return true;
                    }
                }
                else if (diff < 0)
                {
                    return false;
                }
                else
                {
                    pos = r.dequeuePos.load(std::memory_order_relaxed);
                }
            }
        }
    }

    std::optional<Level> parseLevel(std::string_view text)
    {
        if (text == "trace")
        {
            return Level::Trace;
        }
        if (text == "debug")
        {
            return Level::Debug;
        }
        if (text == "info")
        {
            return Level::Info;
        }
        if (text == "warn" || text == "warning")
        {
            return Level::Warn;
        }
        if (text == "error")
        {
            return Level::Error;
        }
        return std::nullopt;
    }

    char const* levelName(Level level)
    {
        switch (level)
        {
            case Level::Trace: return "trace";
            case Level::Debug: return "debug";
            case Level::Info: return "info";
            case Level::Warn: return "warn";
            case Level::Error: return "error";
        }
        return "info";
    }

    void configure(Level level, Format format)
    {
        state().level.store(static_cast<int>(level));
        state().format.store(static_cast<int>(format));
    }

    void setLevel(Level level)
    {
        state().level.store(static_cast<int>(level));
    }

    Level level()
    {
        return static_cast<Level>(state().level.load());
    }

    bool enabled(Level lvl)
    {
        return static_cast<int>(lvl) >= state().level.load(std::memory_order_relaxed);
    }

    void write(Level lvl, std::string_view event, Fields fields)
    {
        if (!enabled(lvl))
        {
            return;
        }
        emit(render(lvl, event, fields, static_cast<Format>(state().format.load())));
    }

    std::vector<std::string> recent(std::size_t maxLines)
    {
        auto& s = state();
        std::lock_guard const lock{s.outMutex};
        std::size_t const n = std::min(maxLines, s.history.size());
        return {s.history.end() - static_cast<std::ptrdiff_t>(n), s.history.end()};
    }

    void enqueueRealtime(Level lvl, char const* component, char const* format, va_list args)
    {
        if (!enabled(lvl))
        {
            return;
        }
        auto& r = ring();
        std::size_t pos = r.enqueuePos.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        for (;;)
        {
            slot = &r.slots[pos & (ringCapacity - 1)];
            std::size_t const seq = slot->sequence.load(std::memory_order_acquire);
            auto const diff = static_cast<std::intptr_t>(seq) - static_cast<std::intptr_t>(pos);
            if (diff == 0)
            {
                if (r.enqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                r.dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            else
            {
                pos = r.enqueuePos.load(std::memory_order_relaxed);
            }
        }
        slot->level = static_cast<int>(lvl);
        std::snprintf(slot->component, sizeof(slot->component), "%s", component != nullptr ? component : "rt");
        std::vsnprintf(slot->text, sizeof(slot->text), format, args);
        slot->sequence.store(pos + 1, std::memory_order_release);
    }

    void enqueueRealtimeF(Level lvl, char const* component, char const* format, ...)
    {
        va_list args;
        va_start(args, format);
        enqueueRealtime(lvl, component, format, args);
        va_end(args);
    }

    void drainNow()
    {
        Slot slot;
        while (tryDequeue(slot))
        {
            std::string_view text{slot.text};
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            {
                text.remove_suffix(1);
            }
            if (text.rfind("MTL: ", 0) == 0)
            {
                text.remove_prefix(5);
            }
            auto const lvl = static_cast<Level>(slot.level);
            if (auto const observer = realtimeObserver.load())
            {
                observer(lvl, slot.component, text);
            }
            write(lvl, "external_log", Fields{{"component", slot.component}, {"message", std::string(text)}});
        }
    }

    void startDrain()
    {
        auto& d = drain();
        std::lock_guard const lock{d.mutex};
        if (d.running)
        {
            return;
        }
        d.stop = false;
        d.running = true;
        d.thread = std::thread(
            []
            {
                auto& dr = drain();
                std::unique_lock lk{dr.mutex};
                while (!dr.stop)
                {
                    lk.unlock();
                    drainNow();
                    lk.lock();
                    dr.cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return dr.stop; });
                }
            });
    }

    void stopDrain()
    {
        auto& d = drain();
        {
            std::lock_guard const lock{d.mutex};
            if (!d.running)
            {
                return;
            }
            d.stop = true;
        }
        d.cv.notify_all();
        if (d.thread.joinable())
        {
            d.thread.join();
        }
        drainNow();
        std::lock_guard const lock{d.mutex};
        d.running = false;
    }

    std::uint64_t droppedRealtime()
    {
        return ring().dropped.load();
    }

    void setRealtimeObserver(RealtimeObserver observer)
    {
        realtimeObserver.store(observer);
    }

    bool RateLimiter::allow(std::string const& key)
    {
        auto const now = std::chrono::steady_clock::now();
        std::lock_guard const lock{_mutex};
        auto const it = _last.find(key);
        if (it == _last.end() || now - it->second >= _interval)
        {
            if (it == _last.end() && _last.size() >= 4096)
            {
                _last.clear(); // bounded memory for keys derived from free text
            }
            _last[key] = now;
            return true;
        }
        return false;
    }

    void RateLimiter::reset(std::string const& key)
    {
        std::lock_guard const lock{_mutex};
        _last.erase(key);
    }
}
