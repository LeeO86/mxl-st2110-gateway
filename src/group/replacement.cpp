// SPDX-License-Identifier: MIT
#include "group/replacement.hpp"

#include <cstring>

#include "codec/v210.hpp"

namespace mxlgw::group
{
    VideoReplacement::VideoReplacement(config::VideoFormat const& format, config::MissingData mode)
        : _mode(mode)
        , _black(format.grainBytes())
    {
        codec::v210FillBlack(_black.data(), format.width, format.linesPerGrain(), format.v210Stride());
        if (_mode == config::MissingData::Repeat)
        {
            _last.resize(_black.size());
        }
    }

    void VideoReplacement::remember(std::uint8_t const* grain)
    {
        if (_mode != config::MissingData::Repeat || grain == nullptr)
        {
            return;
        }
        std::memcpy(_last.data(), grain, _last.size());
        _haveLast = true;
    }

    std::uint8_t const* VideoReplacement::frame() const
    {
        return repeating() ? _last.data() : _black.data();
    }

    codec::AncFrame emptyAnc()
    {
        return codec::AncFrame{};
    }
}
