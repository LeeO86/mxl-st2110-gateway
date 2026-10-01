// SPDX-License-Identifier: MIT
#include "group/essence_state.hpp"

#include <chrono>

#include "util/logging.hpp"

namespace mxlgw::group
{
    char const* toName(EssenceState state)
    {
        switch (state)
        {
            case EssenceState::Idle: return "idle";
            case EssenceState::WaitingForFlow: return "waiting_for_flow";
            case EssenceState::NoSignal: return "no_signal";
            case EssenceState::Running: return "running";
            case EssenceState::Degraded: return "degraded";
            case EssenceState::Error: return "error";
        }
        return "idle";
    }

    StateHolder::StateHolder(nlohmann::json logContext)
        : _context(std::move(logContext))
    {
        _status.sinceNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool StateHolder::set(EssenceState state, std::string reason)
    {
        EssenceStatus previous;
        {
            std::lock_guard const lock{_mutex};
            if (_status.state == state && _status.reason == reason)
            {
                return false;
            }
            previous = _status;
            _status.state = state;
            _status.reason = std::move(reason);
            _status.sinceNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }
        auto fields = _context;
        fields["from"] = toName(previous.state);
        fields["state"] = toName(state);
        auto const current = get();
        if (!current.reason.empty())
        {
            fields["reason"] = current.reason;
        }
        auto const level = state == EssenceState::Error ? log::Level::Warn : log::Level::Info;
        log::write(level, "essence_state", fields);
        return true;
    }

    EssenceStatus StateHolder::get() const
    {
        std::lock_guard const lock{_mutex};
        return _status;
    }
}
