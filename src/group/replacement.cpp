// SPDX-License-Identifier: MIT
#include "group/replacement.hpp"

#include <cstring>

#include "codec/v210.hpp"

namespace mxlgw::group
{
    VideoReplacement::VideoReplacement(config::VideoFormat const& format, config::MissingData mode)
        : _mode(mode)
        , _interlaced(format.interlaced())
        , _black(format.grainBytes())
    {
        codec::v210FillBlack(_black.data(), format.width, format.linesPerGrain(), format.v210Stride());
        if (_mode == config::MissingData::Repeat)
        {
            _last[0].resize(_black.size());
            if (_interlaced)
            {
                _last[1].resize(_black.size());
            }
        }
    }

    void VideoReplacement::remember(std::uint8_t const* grain, bool secondField)
    {
        if (_mode != config::MissingData::Repeat || grain == nullptr)
        {
            return;
        }
        auto const s = slot(secondField);
        std::memcpy(_last[s].data(), grain, _last[s].size());
        _haveLast[s] = true;
    }

    std::uint8_t const* VideoReplacement::frame(bool secondField) const
    {
        auto const s = slot(secondField);
        return _mode == config::MissingData::Repeat && _haveLast[s] ? _last[s].data() : _black.data();
    }

    codec::AncFrame emptyAnc()
    {
        return codec::AncFrame{};
    }
}
