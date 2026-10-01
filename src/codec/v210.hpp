// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace mxlgw::codec
{
    /// MXL v210 line length: ((width + 47) / 48) * 128 (MXL MediaUtils.cpp getV210LineLength).
    std::size_t v210Stride(int width);

    struct YCbCr
    {
        std::uint16_t y;
        std::uint16_t cb;
        std::uint16_t cr;
    };

    /// Fills `lines` lines of v210 with one colour (10-bit values).
    void v210Fill(std::uint8_t* data, int width, int lines, std::size_t stride, YCbCr colour);

    /// Black: Y = 64, Cb = Cr = 512 (§5.7 replacement frames).
    void v210FillBlack(std::uint8_t* data, int width, int lines, std::size_t stride);

    /// Reads one pixel (luma + the chroma pair it shares) from v210.
    YCbCr v210Pixel(std::uint8_t const* data, std::size_t stride, int x, int line);

    /// Test pattern for tools/mxl-pattern-writer: 75 % colour bars (8 bars) with a 32-bit frame
    /// counter encoded in the top `counterLines` lines as black/white luma blocks.
    void v210ColourBars(std::uint8_t* data, int width, int lines, std::size_t stride, std::uint32_t frameCounter, int counterLines = 16);

    /// Expected 10-bit values of bar `index` (0..7) of v210ColourBars.
    YCbCr colourBar(int index);

    /// Decodes the frame counter written by v210ColourBars; nullopt if the blocks are not black/white.
    std::optional<std::uint32_t> v210ReadCounter(std::uint8_t const* data, int width, std::size_t stride, int counterLines = 16);

    /// Checks that the bars area (below the counter) matches the expected bar values within `tolerance`.
    bool v210CheckBars(std::uint8_t const* data, int width, int lines, std::size_t stride, int counterLines = 16, int tolerance = 2);
}
