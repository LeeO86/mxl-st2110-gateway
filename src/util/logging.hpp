// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace mxlgw::log
{
    enum class Level
    {
        Trace = 0,
        Debug,
        Info,
        Warn,
        Error,
    };

    enum class Format
    {
        Json,
        Text,
    };

    using Fields = nlohmann::json;

    std::optional<Level> parseLevel(std::string_view text);
    char const* levelName(Level level);

    void configure(Level level, Format format);
    void setLevel(Level level);
    Level level();
    bool enabled(Level level);

    /// Writes one structured line (stdout, JSON or text) and keeps it in the in-memory history.
    void write(Level level, std::string_view event, Fields fields = Fields::object());

    inline void trace(std::string_view event, Fields fields = Fields::object())
    {
        write(Level::Trace, event, std::move(fields));
    }
    inline void debug(std::string_view event, Fields fields = Fields::object())
    {
        write(Level::Debug, event, std::move(fields));
    }
    inline void info(std::string_view event, Fields fields = Fields::object())
    {
        write(Level::Info, event, std::move(fields));
    }
    inline void warn(std::string_view event, Fields fields = Fields::object())
    {
        write(Level::Warn, event, std::move(fields));
    }
    inline void error(std::string_view event, Fields fields = Fields::object())
    {
        write(Level::Error, event, std::move(fields));
    }

    /// Last lines written (newest last), at most 500 (§13).
    std::vector<std::string> recent(std::size_t maxLines = 500);

    /// Lock-free, allocation-free entry point for real-time contexts (MTL lcores, MTL log printer):
    /// formats into a fixed slot of a bounded ring; the drain thread emits it with `component`.
    /// Messages are dropped (and counted) when the ring is full.
    void enqueueRealtime(Level level, char const* component, char const* format, va_list args);
    void enqueueRealtimeF(Level level, char const* component, char const* format, ...) __attribute__((format(printf, 3, 4)));

    /// Starts/stops the thread draining the real-time ring (idempotent).
    void startDrain();
    void stopDrain();
    /// Drains synchronously (used by tests and on shutdown).
    void drainNow();
    std::uint64_t droppedRealtime();

    /// Optional observer for drained real-time lines (e.g. to parse the DDP package version).
    using RealtimeObserver = void (*)(Level, std::string_view component, std::string_view message);
    void setRealtimeObserver(RealtimeObserver observer);

    /// Rate limiter for repeated events: allow() is true on the first call for a key and then at most
    /// once per interval.
    class RateLimiter
    {
    public:
        explicit RateLimiter(std::chrono::steady_clock::duration interval)
            : _interval(interval)
        {}

        bool allow(std::string const& key);
        void reset(std::string const& key);

    private:
        std::chrono::steady_clock::duration _interval;
        std::mutex _mutex;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> _last;
    };
}
