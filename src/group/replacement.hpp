// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <vector>

#include "codec/anc8331.hpp"
#include "config/config.hpp"

namespace mxlgw::group
{
    /// Replacement video for egress when no grain is available in time (§5.7, owner decision Q3):
    /// black by default, or the last good grain with `missing_data = repeat`.
    class VideoReplacement
    {
    public:
        VideoReplacement(config::VideoFormat const& format, config::MissingData mode);

        /// Remembers a good grain (only copied in repeat mode).
        void remember(std::uint8_t const* grain);
        /// The frame to send instead of a missing grain.
        std::uint8_t const* frame() const;
        bool repeating() const { return _mode == config::MissingData::Repeat && _haveLast; }
        std::uint8_t const* black() const { return _black.data(); }

    private:
        config::MissingData _mode;
        std::vector<std::uint8_t> _black;
        std::vector<std::uint8_t> _last;
        bool _haveLast = false;
    };

    /// Empty ANC frame (ANC_Count = 0) used as replacement.
    codec::AncFrame emptyAnc();
}
