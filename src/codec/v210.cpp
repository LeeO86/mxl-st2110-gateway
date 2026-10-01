// SPDX-License-Identifier: MIT
#include "codec/v210.hpp"

#include <cstdlib>
#include <cstring>

namespace mxlgw::codec
{
    namespace
    {
        // v210: 6 pixels per 16-byte block, 4 little-endian 32-bit words:
        // w0 = Cb0 | Y0<<10 | Cr0<<20, w1 = Y1 | Cb1<<10 | Y2<<20,
        // w2 = Cr1 | Y3<<10 | Cb2<<20, w3 = Y4 | Cr2<<10 | Y5<<20.
        inline std::uint32_t word(std::uint32_t a, std::uint32_t b, std::uint32_t c)
        {
            return (a & 0x3FF) | ((b & 0x3FF) << 10) | ((c & 0x3FF) << 20);
        }

        inline void store(std::uint8_t* p, std::uint32_t w)
        {
            p[0] = static_cast<std::uint8_t>(w);
            p[1] = static_cast<std::uint8_t>(w >> 8);
            p[2] = static_cast<std::uint8_t>(w >> 16);
            p[3] = static_cast<std::uint8_t>(w >> 24);
        }

        inline std::uint32_t load(std::uint8_t const* p)
        {
            return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
                   (static_cast<std::uint32_t>(p[3]) << 24);
        }

        // Writes one block of 6 pixels; colours per pixel pair (3 pairs).
        void writeBlock(std::uint8_t* p, YCbCr const pairs[3])
        {
            store(p + 0, word(pairs[0].cb, pairs[0].y, pairs[0].cr));
            store(p + 4, word(pairs[0].y, pairs[1].cb, pairs[1].y));
            store(p + 8, word(pairs[1].cr, pairs[1].y, pairs[2].cb));
            store(p + 12, word(pairs[2].y, pairs[2].cr, pairs[2].y));
        }

        // BT.709 75 % colour bars, 10-bit narrow range.
        constexpr YCbCr bars75[8] = {
            {721, 512, 512}, // white
            {674, 176, 543}, // yellow
            {581, 589, 176}, // cyan
            {534, 253, 207}, // green
            {251, 771, 817}, // magenta
            {204, 435, 848}, // red
            {111, 848, 481}, // blue
            {64, 512, 512},  // black
        };

        constexpr YCbCr black{64, 512, 512};
        constexpr YCbCr white{940, 512, 512};
    }

    std::size_t v210Stride(int width)
    {
        return static_cast<std::size_t>((width + 47) / 48) * 128;
    }

    void v210Fill(std::uint8_t* data, int width, int lines, std::size_t stride, YCbCr colour)
    {
        YCbCr const pairs[3] = {colour, colour, colour};
        std::uint8_t line[16];
        writeBlock(line, pairs);
        int const blocks = (width + 5) / 6;
        for (int l = 0; l < lines; ++l)
        {
            std::uint8_t* row = data + static_cast<std::size_t>(l) * stride;
            for (int b = 0; b < blocks; ++b)
            {
                std::memcpy(row + static_cast<std::size_t>(b) * 16, line, 16);
            }
            std::size_t const used = static_cast<std::size_t>(blocks) * 16;
            if (stride > used)
            {
                std::memset(row + used, 0, stride - used);
            }
        }
    }

    void v210FillBlack(std::uint8_t* data, int width, int lines, std::size_t stride)
    {
        v210Fill(data, width, lines, stride, black);
    }

    YCbCr v210Pixel(std::uint8_t const* data, std::size_t stride, int x, int line)
    {
        std::uint8_t const* block = data + static_cast<std::size_t>(line) * stride + static_cast<std::size_t>(x / 6) * 16;
        std::uint32_t const w0 = load(block), w1 = load(block + 4), w2 = load(block + 8), w3 = load(block + 12);
        auto f = [](std::uint32_t w, int i) { return static_cast<std::uint16_t>((w >> (10 * i)) & 0x3FF); };
        std::uint16_t const ys[6] = {f(w0, 1), f(w1, 0), f(w1, 2), f(w2, 1), f(w3, 0), f(w3, 2)};
        std::uint16_t const cbs[3] = {f(w0, 0), f(w1, 1), f(w2, 2)};
        std::uint16_t const crs[3] = {f(w0, 2), f(w2, 0), f(w3, 1)};
        int const i = x % 6;
        return {ys[i], cbs[i / 2], crs[i / 2]};
    }

    YCbCr colourBar(int index)
    {
        return bars75[index & 7];
    }

    void v210ColourBars(std::uint8_t* data, int width, int lines, std::size_t stride, std::uint32_t frameCounter, int counterLines)
    {
        int const blocks = (width + 5) / 6;
        int const blocksPerBar = (blocks + 7) / 8;
        // 32 counter bits, each `bitBlocks` blocks wide (6 px per block).
        int const bitBlocks = blocks / 32 > 0 ? blocks / 32 : 1;
        for (int l = 0; l < lines; ++l)
        {
            std::uint8_t* row = data + static_cast<std::size_t>(l) * stride;
            for (int b = 0; b < blocks; ++b)
            {
                YCbCr c;
                if (l < counterLines)
                {
                    int const bit = b / bitBlocks;
                    c = (bit < 32 && ((frameCounter >> (31 - bit)) & 1u)) ? white : black;
                }
                else
                {
                    c = bars75[b / blocksPerBar < 8 ? b / blocksPerBar : 7];
                }
                YCbCr const pairs[3] = {c, c, c};
                writeBlock(row + static_cast<std::size_t>(b) * 16, pairs);
            }
            std::size_t const used = static_cast<std::size_t>(blocks) * 16;
            if (stride > used)
            {
                std::memset(row + used, 0, stride - used);
            }
        }
    }

    std::optional<std::uint32_t> v210ReadCounter(std::uint8_t const* data, int width, std::size_t stride, int counterLines)
    {
        int const blocks = (width + 5) / 6;
        int const bitBlocks = blocks / 32 > 0 ? blocks / 32 : 1;
        int const line = counterLines / 2;
        std::uint32_t value = 0;
        for (int bit = 0; bit < 32; ++bit)
        {
            int const x = (bit * bitBlocks + bitBlocks / 2) * 6 + 2;
            auto const px = v210Pixel(data, stride, x, line);
            if (std::abs(px.y - white.y) <= 8)
            {
                value |= 1u << (31 - bit);
            }
            else if (std::abs(px.y - black.y) > 8)
            {
                return std::nullopt;
            }
        }
        return value;
    }

    bool v210CheckBars(std::uint8_t const* data, int width, int lines, std::size_t stride, int counterLines, int tolerance)
    {
        int const blocks = (width + 5) / 6;
        int const blocksPerBar = (blocks + 7) / 8;
        for (int bar = 0; bar < 8; ++bar)
        {
            int const block = bar * blocksPerBar + blocksPerBar / 2;
            if (block >= blocks)
            {
                break;
            }
            int const x = block * 6 + 2;
            for (int l : {counterLines + 1, (counterLines + lines) / 2, lines - 1})
            {
                if (l <= counterLines || l >= lines)
                {
                    continue;
                }
                auto const px = v210Pixel(data, stride, x, l);
                auto const exp = bars75[bar];
                if (std::abs(px.y - exp.y) > tolerance || std::abs(px.cb - exp.cb) > tolerance || std::abs(px.cr - exp.cr) > tolerance)
                {
                    return false;
                }
            }
        }
        return true;
    }
}
