// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace mxlgw::group
{
    /// Essence states (§12.1 `mxlgw_essence_state`, §5.8).
    enum class EssenceState
    {
        Idle,
        WaitingForFlow,
        NoSignal,
        Running,
        Degraded,
        Error,
    };

    char const* toName(EssenceState state);
    inline constexpr EssenceState allEssenceStates[] = {EssenceState::Idle,    EssenceState::WaitingForFlow, EssenceState::NoSignal,
                                                        EssenceState::Running, EssenceState::Degraded,       EssenceState::Error};

    struct EssenceStatus
    {
        EssenceState state = EssenceState::Idle;
        std::string reason;
        std::int64_t sinceNs = 0;
    };

    /// Thread-safe state holder that logs `essence_state` on every change (§13).
    class StateHolder
    {
    public:
        explicit StateHolder(nlohmann::json logContext = nlohmann::json::object());

        /// Returns true if the state or reason changed.
        bool set(EssenceState state, std::string reason = {});
        EssenceStatus get() const;

    private:
        nlohmann::json _context;
        mutable std::mutex _mutex;
        EssenceStatus _status;
    };
}
