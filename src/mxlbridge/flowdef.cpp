// SPDX-License-Identifier: MIT
#include "mxlbridge/flowdef.hpp"

namespace mxlgw::mxlbridge
{
    namespace
    {
        using json = nlohmann::json;

        json common(FlowIdentity const& id, char const* format, char const* mediaType)
        {
            json j;
            j["id"] = id.flowId.toString();
            j["version"] = id.version;
            j["label"] = id.label;
            j["description"] = id.description;
            j["format"] = format;
            j["media_type"] = mediaType;
            j["tags"] = json::object();
            j["tags"]["urn:x-nmos:tag:grouphint/v1.0"] = json::array({id.groupHint});
            j["source_id"] = id.sourceId.toString();
            j["device_id"] = id.deviceId.toString();
            j["parents"] = json::array();
            return j;
        }

        json rational(util::Rational r)
        {
            return json{{"numerator", r.num}, {"denominator", r.den}};
        }

        std::optional<util::Rational> readRational(json const& j, char const* key)
        {
            if (!j.contains(key) || !j[key].is_object())
            {
                return std::nullopt;
            }
            auto const& r = j[key];
            if (!r.contains("numerator") || !r["numerator"].is_number())
            {
                return std::nullopt;
            }
            util::Rational out;
            out.num = r["numerator"].get<std::int64_t>();
            out.den = r.contains("denominator") && r["denominator"].is_number() ? r["denominator"].get<std::int64_t>() : 1;
            return out;
        }

        bool sameRate(util::Rational a, util::Rational b)
        {
            return static_cast<__int128>(a.num) * b.den == static_cast<__int128>(b.num) * a.den;
        }

        std::string text(json const& j, char const* key)
        {
            return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
        }

        std::int64_t integer(json const& j, char const* key)
        {
            return j.contains(key) && j[key].is_number() ? j[key].get<std::int64_t>() : -1;
        }
    }

    std::string nmosVersion(std::int64_t taiNs)
    {
        return std::to_string(taiNs / 1'000'000'000) + ":" + std::to_string(taiNs % 1'000'000'000);
    }

    nlohmann::json videoFlowDef(FlowIdentity const& id, config::VideoFormat const& f)
    {
        auto j = common(id, "urn:x-nmos:format:video", "video/v210");
        j["grain_rate"] = rational(f.rate);
        j["frame_width"] = f.width;
        j["frame_height"] = f.height;
        j["interlace_mode"] = config::toName(f.interlace);
        j["colorspace"] = f.colorimetry;
        j["transfer_characteristic"] = f.tcs;
        j["components"] = json::array({
            json{{"name", "Y"}, {"width", f.width}, {"height", f.height}, {"bit_depth", 10}},
            json{{"name", "Cb"}, {"width", f.width / 2}, {"height", f.height}, {"bit_depth", 10}},
            json{{"name", "Cr"}, {"width", f.width / 2}, {"height", f.height}, {"bit_depth", 10}},
        });
        return j;
    }

    nlohmann::json audioFlowDef(FlowIdentity const& id, config::AudioFormat const& f)
    {
        auto j = common(id, "urn:x-nmos:format:audio", "audio/float32");
        j["sample_rate"] = rational({f.sampleRate, 1});
        j["channel_count"] = f.channels;
        j["bit_depth"] = 32;
        return j;
    }

    nlohmann::json ancFlowDef(FlowIdentity const& id, config::AncFormat const& f)
    {
        auto j = common(id, "urn:x-nmos:format:data", "video/smpte291");
        j["grain_rate"] = rational(f.grainRate());
        j["DID_SDID"] = json::array();
        return j;
    }

    std::vector<std::string> compareVideo(nlohmann::json const& d, config::VideoFormat const& f)
    {
        std::vector<std::string> out;
        if (text(d, "media_type") != "video/v210")
        {
            out.push_back("media_type " + text(d, "media_type") + " is not video/v210");
            return out;
        }
        if (integer(d, "frame_width") != f.width || integer(d, "frame_height") != f.height)
        {
            out.push_back("frame size " + std::to_string(integer(d, "frame_width")) + "x" + std::to_string(integer(d, "frame_height")) + " does not match " +
                          std::to_string(f.width) + "x" + std::to_string(f.height));
        }
        auto const rate = readRational(d, "grain_rate");
        if (!rate || !sameRate(*rate, f.rate))
        {
            out.push_back("grain_rate does not match " + f.rate.toString());
        }
        auto const il = text(d, "interlace_mode").empty() ? std::string("progressive") : text(d, "interlace_mode");
        bool const flowInterlaced = il != "progressive";
        if (flowInterlaced != f.interlaced())
        {
            out.push_back("interlace_mode " + il + " does not match " + config::toName(f.interlace));
        }
        return out;
    }

    std::vector<std::string> compareAudio(nlohmann::json const& d, config::AudioFormat const& f)
    {
        std::vector<std::string> out;
        auto const mt = text(d, "media_type");
        if (!mt.empty() && mt != "audio/float32")
        {
            out.push_back("media_type " + mt + " is not audio/float32");
        }
        auto const rate = readRational(d, "sample_rate");
        if (!rate || !sameRate(*rate, {f.sampleRate, 1}))
        {
            out.push_back("sample_rate does not match " + std::to_string(f.sampleRate));
        }
        auto const channels = d.contains("channel_count") ? integer(d, "channel_count") : 1;
        if (channels != f.channels)
        {
            out.push_back("channel_count " + std::to_string(channels) + " does not match " + std::to_string(f.channels));
        }
        return out;
    }

    std::vector<std::string> compareAnc(nlohmann::json const& d, config::AncFormat const& f)
    {
        std::vector<std::string> out;
        if (text(d, "media_type") != "video/smpte291")
        {
            out.push_back("media_type " + text(d, "media_type") + " is not video/smpte291");
        }
        auto const rate = readRational(d, "grain_rate");
        if (!rate || !sameRate(*rate, f.grainRate()))
        {
            out.push_back("grain_rate does not match " + f.grainRate().toString());
        }
        return out;
    }

    std::string writerOptions(std::uint32_t commitBatchHint, std::uint32_t syncBatchHint)
    {
        json j;
        j["maxCommitBatchSizeHint"] = commitBatchHint;
        j["maxSyncBatchSizeHint"] = syncBatchHint;
        return j.dump();
    }
}
