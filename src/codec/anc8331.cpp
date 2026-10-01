// SPDX-License-Identifier: MIT
#include "codec/anc8331.hpp"

namespace mxlgw::codec
{
    namespace
    {
        class BitWriter
        {
        public:
            void put(std::uint32_t value, int bits)
            {
                for (int i = bits - 1; i >= 0; --i)
                {
                    putBit((value >> i) & 1u);
                }
            }

            void alignTo32()
            {
                while (_bitCount % 32 != 0)
                {
                    putBit(0);
                }
            }

            std::vector<std::uint8_t>& bytes() { return _bytes; }
            std::size_t bitCount() const { return _bitCount; }

        private:
            void putBit(std::uint32_t bit)
            {
                if (_bitCount % 8 == 0)
                {
                    _bytes.push_back(0);
                }
                if (bit != 0)
                {
                    _bytes.back() = static_cast<std::uint8_t>(_bytes.back() | (0x80u >> (_bitCount % 8)));
                }
                ++_bitCount;
            }

            std::vector<std::uint8_t> _bytes;
            std::size_t _bitCount = 0;
        };

        class BitReader
        {
        public:
            BitReader(std::uint8_t const* data, std::size_t size)
                : _data(data)
                , _bits(size * 8)
            {}

            bool get(int bits, std::uint32_t& out)
            {
                if (_pos + static_cast<std::size_t>(bits) > _bits)
                {
                    return false;
                }
                std::uint32_t v = 0;
                for (int i = 0; i < bits; ++i)
                {
                    std::size_t const p = _pos++;
                    v = (v << 1) | ((_data[p / 8] >> (7 - (p % 8))) & 1u);
                }
                out = v;
                return true;
            }

            bool alignTo32(std::size_t originBits)
            {
                while ((_pos - originBits) % 32 != 0)
                {
                    if (_pos >= _bits)
                    {
                        return false;
                    }
                    ++_pos;
                }
                return true;
            }

            std::size_t pos() const { return _pos; }

        private:
            std::uint8_t const* _data;
            std::size_t _bits;
            std::size_t _pos = 0;
        };
    }

    bool AncPacket::operator==(AncPacket const& o) const
    {
        return c == o.c && line == o.line && hOffset == o.hOffset && s == o.s && streamNum == o.streamNum && did == o.did && sdid == o.sdid && udw == o.udw;
    }

    std::uint16_t withParity(std::uint8_t value)
    {
        unsigned ones = 0;
        for (unsigned v = value; v != 0; v >>= 1)
        {
            ones += v & 1u;
        }
        std::uint16_t const b8 = (ones % 2 == 1) ? 1 : 0; // even parity over b0..b8
        return static_cast<std::uint16_t>(value | (b8 << 8) | ((b8 ^ 1u) << 9));
    }

    bool parityOk(std::uint16_t word)
    {
        return withParity(static_cast<std::uint8_t>(word & 0xFF)) == (word & 0x3FF);
    }

    std::uint16_t checksum(std::uint16_t did10, std::uint16_t sdid10, std::uint16_t dc10, std::vector<std::uint16_t> const& udw10)
    {
        std::uint32_t sum = (did10 & 0x1FF) + (sdid10 & 0x1FF) + (dc10 & 0x1FF);
        for (auto const w : udw10)
        {
            sum += w & 0x1FF;
        }
        sum &= 0x1FF;
        std::uint16_t const b8 = (sum >> 8) & 1u;
        return static_cast<std::uint16_t>(sum | ((b8 ^ 1u) << 9));
    }

    std::optional<std::vector<std::uint8_t>> serialiseGrain(AncFrame const& frame, std::size_t capacity)
    {
        if (frame.packets.size() > 255)
        {
            return std::nullopt;
        }
        BitWriter body;
        for (auto const& p : frame.packets)
        {
            if (p.udw.size() > 255)
            {
                return std::nullopt;
            }
            std::size_t const start = body.bitCount();
            body.put(p.c ? 1 : 0, 1);
            body.put(p.line & 0x7FF, 11);
            body.put(p.hOffset & 0xFFF, 12);
            body.put(p.s ? 1 : 0, 1);
            body.put(p.streamNum & 0x7F, 7);
            std::uint16_t const did = withParity(p.did);
            std::uint16_t const sdid = withParity(p.sdid);
            std::uint16_t const dc = withParity(static_cast<std::uint8_t>(p.udw.size()));
            std::vector<std::uint16_t> udw10;
            udw10.reserve(p.udw.size());
            for (auto const b : p.udw)
            {
                udw10.push_back(withParity(b));
            }
            body.put(did, 10);
            body.put(sdid, 10);
            body.put(dc, 10);
            for (auto const w : udw10)
            {
                body.put(w, 10);
            }
            body.put(checksum(did, sdid, dc, udw10), 10);
            (void)start;
            body.alignTo32();
        }
        auto const& bytes = body.bytes();
        if (ancHeaderBytes + bytes.size() > capacity || bytes.size() > 0xFFFF)
        {
            return std::nullopt;
        }
        std::vector<std::uint8_t> out;
        out.reserve(ancHeaderBytes + bytes.size());
        auto const length = static_cast<std::uint16_t>(bytes.size());
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(length & 0xFF));
        out.push_back(static_cast<std::uint8_t>(frame.packets.size()));
        out.push_back(static_cast<std::uint8_t>(static_cast<std::uint8_t>(frame.field) << 6)); // F (2 bits) + reserved
        out.push_back(0);
        out.push_back(0);
        out.insert(out.end(), bytes.begin(), bytes.end());
        return out;
    }

    AncParseResult parseGrain(std::uint8_t const* data, std::size_t size)
    {
        AncParseResult result;
        if (data == nullptr || size < ancHeaderBytes)
        {
            result.error = "truncated header";
            return result;
        }
        std::size_t const length = (static_cast<std::size_t>(data[0]) << 8) | data[1];
        std::size_t const count = data[2];
        auto const field = static_cast<std::uint8_t>(data[3] >> 6);
        if (ancHeaderBytes + length > size)
        {
            result.error = "length field exceeds grain";
            return result;
        }
        AncFrame frame;
        frame.field = field == 2 ? AncField::Field1 : field == 3 ? AncField::Field2 : AncField::Unspecified;
        BitReader reader(data + ancHeaderBytes, length);
        for (std::size_t i = 0; i < count; ++i)
        {
            std::size_t const origin = reader.pos();
            std::uint32_t c = 0, line = 0, hOffset = 0, s = 0, stream = 0, did = 0, sdid = 0, dc = 0;
            if (!reader.get(1, c) || !reader.get(11, line) || !reader.get(12, hOffset) || !reader.get(1, s) || !reader.get(7, stream) || !reader.get(10, did) ||
                !reader.get(10, sdid) || !reader.get(10, dc))
            {
                result.error = "truncated ANC packet " + std::to_string(i);
                return result;
            }
            AncPacket p;
            p.c = c != 0;
            p.line = static_cast<std::uint16_t>(line);
            p.hOffset = static_cast<std::uint16_t>(hOffset);
            p.s = s != 0;
            p.streamNum = static_cast<std::uint8_t>(stream);
            p.did = static_cast<std::uint8_t>(did & 0xFF);
            p.sdid = static_cast<std::uint8_t>(sdid & 0xFF);
            for (auto const w : {did, sdid, dc})
            {
                if (!parityOk(static_cast<std::uint16_t>(w)))
                {
                    ++result.parityErrors;
                }
            }
            std::size_t const n = dc & 0xFF;
            std::vector<std::uint16_t> udw10;
            udw10.reserve(n);
            for (std::size_t k = 0; k < n; ++k)
            {
                std::uint32_t w = 0;
                if (!reader.get(10, w))
                {
                    result.error = "truncated user data in ANC packet " + std::to_string(i);
                    return result;
                }
                udw10.push_back(static_cast<std::uint16_t>(w));
                p.udw.push_back(static_cast<std::uint8_t>(w & 0xFF));
                if (!parityOk(static_cast<std::uint16_t>(w)))
                {
                    ++result.parityErrors;
                }
            }
            std::uint32_t cs = 0;
            if (!reader.get(10, cs))
            {
                result.error = "truncated checksum in ANC packet " + std::to_string(i);
                return result;
            }
            if (checksum(static_cast<std::uint16_t>(did), static_cast<std::uint16_t>(sdid), static_cast<std::uint16_t>(dc), udw10) != (cs & 0x3FF))
            {
                ++result.checksumErrors;
            }
            if (!reader.alignTo32(origin))
            {
                result.error = "truncated word_align in ANC packet " + std::to_string(i);
                return result;
            }
            frame.packets.push_back(std::move(p));
        }
        result.frame = std::move(frame);
        return result;
    }
}
