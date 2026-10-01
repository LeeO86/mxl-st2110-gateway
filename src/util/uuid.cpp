// SPDX-License-Identifier: MIT
#include "util/uuid.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <random>
#include <stdexcept>

namespace mxlgw::util
{
    namespace
    {
        int hexValue(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }

        void setVersion(Uuid& uuid, std::uint8_t version)
        {
            uuid.bytes[6] = static_cast<std::uint8_t>((uuid.bytes[6] & 0x0F) | (version << 4));
            uuid.bytes[8] = static_cast<std::uint8_t>((uuid.bytes[8] & 0x3F) | 0x80);
        }
    }

    bool Uuid::isNil() const
    {
        for (auto const b : bytes)
        {
            if (b != 0)
            {
                return false;
            }
        }
        return true;
    }

    std::string Uuid::toString() const
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string out;
        out.reserve(36);
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10)
            {
                out.push_back('-');
            }
            out.push_back(digits[bytes[i] >> 4]);
            out.push_back(digits[bytes[i] & 0x0F]);
        }
        return out;
    }

    std::optional<Uuid> parseUuid(std::string_view text)
    {
        if (text.size() != 36)
        {
            return std::nullopt;
        }
        Uuid uuid;
        std::size_t byte = 0;
        for (std::size_t i = 0; i < text.size();)
        {
            if (i == 8 || i == 13 || i == 18 || i == 23)
            {
                if (text[i] != '-')
                {
                    return std::nullopt;
                }
                ++i;
                continue;
            }
            int const hi = hexValue(text[i]);
            int const lo = hexValue(text[i + 1]);
            if (hi < 0 || lo < 0)
            {
                return std::nullopt;
            }
            uuid.bytes[byte++] = static_cast<std::uint8_t>((hi << 4) | lo);
            i += 2;
        }
        return uuid;
    }

    bool isUuid(std::string_view text)
    {
        return parseUuid(text).has_value();
    }

    Uuid uuidV4()
    {
        Uuid uuid;
        if (RAND_bytes(uuid.bytes.data(), static_cast<int>(uuid.bytes.size())) != 1)
        {
            std::random_device rd;
            for (auto& b : uuid.bytes)
            {
                b = static_cast<std::uint8_t>(rd());
            }
        }
        setVersion(uuid, 4);
        return uuid;
    }

    Uuid uuidV5(Uuid const& ns, std::string_view name)
    {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digestLength = 0;
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        if (ctx == nullptr)
        {
            throw std::runtime_error("EVP_MD_CTX_new failed");
        }
        bool const ok = EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr) == 1 && EVP_DigestUpdate(ctx, ns.bytes.data(), ns.bytes.size()) == 1 &&
                        EVP_DigestUpdate(ctx, name.data(), name.size()) == 1 && EVP_DigestFinal_ex(ctx, digest, &digestLength) == 1;
        EVP_MD_CTX_free(ctx);
        if (!ok || digestLength < 16)
        {
            throw std::runtime_error("SHA-1 digest failed");
        }
        Uuid uuid;
        for (std::size_t i = 0; i < uuid.bytes.size(); ++i)
        {
            uuid.bytes[i] = digest[i];
        }
        setVersion(uuid, 5);
        return uuid;
    }
}
