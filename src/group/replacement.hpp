// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "codec/anc8331.hpp"
#include "config/config.hpp"

namespace mxlgw::group
{
    /// Replacement video for egress when no grain is available in time (§5.7, owner decision Q3):
    /// black by default, or the last good grain with `missing_data = repeat`. An interlaced format keeps one
    /// grain per field parity (the last good frame's two fields), so a repeat sends each field in its own F slot.
    class VideoReplacement
    {
    public:
        VideoReplacement(config::VideoFormat const& format, config::MissingData mode);

        /// Remembers a good grain (only copied in repeat mode); `secondField` is its parity (interlaced only).
        void remember(std::uint8_t const* grain, bool secondField = false);
        /// The frame (interlaced: the field of that parity) to send instead of a missing grain.
        std::uint8_t const* frame(bool secondField = false) const;
        bool repeating() const { return _mode == config::MissingData::Repeat && (_haveLast[0] || _haveLast[1]); }
        std::uint8_t const* black() const { return _black.data(); }

    private:
        std::size_t slot(bool secondField) const { return _interlaced && secondField ? 1 : 0; }

        config::MissingData _mode;
        bool _interlaced = false;
        std::vector<std::uint8_t> _black;
        std::array<std::vector<std::uint8_t>, 2> _last;
        std::array<bool, 2> _haveLast{};
    };

    /// Empty ANC frame (ANC_Count = 0) used as replacement.
    codec::AncFrame emptyAnc();
}
