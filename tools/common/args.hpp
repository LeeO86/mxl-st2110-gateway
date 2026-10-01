// SPDX-License-Identifier: MIT
// Minimal "--key value" / "--flag" parser shared by the test tools.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>

#include "config/formats.hpp"
#include "util/strings.hpp"
#include "util/uuid.hpp"

namespace mxlgw::tools
{
    class Args
    {
    public:
        Args(int argc, char** argv)
        {
            for (int i = 1; i < argc; ++i)
            {
                std::string key = argv[i];
                if (key.rfind("--", 0) != 0)
                {
                    continue;
                }
                key = key.substr(2);
                if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                {
                    _values[key] = argv[++i];
                }
                else
                {
                    _values[key] = "true";
                }
            }
        }

        bool has(std::string const& key) const { return _values.count(key) != 0; }
        std::string get(std::string const& key, std::string const& fallback = {}) const
        {
            auto const it = _values.find(key);
            return it == _values.end() ? fallback : it->second;
        }
        long long integer(std::string const& key, long long fallback) const
        {
            auto const v = util::parseInt(get(key));
            return v ? *v : fallback;
        }
        std::optional<util::Uuid> uuid(std::string const& key) const
        {
            if (!has(key))
            {
                return std::nullopt;
            }
            auto const id = util::parseUuid(get(key));
            if (!id)
            {
                std::fprintf(stderr, "--%s: not a UUID: %s\n", key.c_str(), get(key).c_str());
                std::exit(2);
            }
            return id;
        }

        config::VideoFormat video() const
        {
            config::VideoFormat f;
            f.width = static_cast<int>(integer("width", 1920));
            f.height = static_cast<int>(integer("height", 1080));
            if (auto const r = util::parseRational(get("rate", "50/1")))
            {
                f.rate = *r;
            }
            if (auto const il = config::parseInterlace(get("interlace", "progressive")))
            {
                f.interlace = *il;
            }
            return f;
        }

        config::AudioFormat audio() const
        {
            config::AudioFormat f;
            f.channels = static_cast<int>(integer("channels", 2));
            f.blockUs = static_cast<int>(integer("block-us", 1000));
            f.ptimeUs = f.blockUs;
            return f;
        }

        config::AncFormat anc() const
        {
            auto const v = video();
            config::AncFormat f;
            f.rate = v.rate;
            f.interlace = v.interlace;
            return f;
        }

    private:
        std::map<std::string, std::string> _values;
    };
}
