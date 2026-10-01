// SPDX-License-Identifier: MIT
#include "config/group_factory.hpp"

namespace mxlgw::config
{
    GroupRequest parseGroupRequest(nlohmann::json const& body, ValidationErrors& errors)
    {
        GroupRequest r;
        if (!body.is_object())
        {
            errors.push_back({"", "request body must be a JSON object"});
            return r;
        }
        auto text = [&](char const* key, std::string& out, bool required)
        {
            if (body.contains(key) && body[key].is_string())
            {
                out = body[key].get<std::string>();
            }
            else if (required)
            {
                errors.push_back({std::string("/") + key, "is required"});
            }
        };
        text("label", r.label, true);
        text("direction", r.direction, false);
        text("domain", r.domain, true);
        if (r.direction != "ingest" && r.direction != "egress")
        {
            errors.push_back({"/direction", "must be ingest or egress"});
        }
        if (body.contains("redundancy"))
        {
            if (!body["redundancy"].is_boolean())
            {
                errors.push_back({"/redundancy", "must be a boolean"});
            }
            else
            {
                r.redundancy = body["redundancy"].get<bool>();
            }
        }
        if (body.contains("counts"))
        {
            auto const& c = body["counts"];
            for (auto const& [key, target] : std::initializer_list<std::pair<char const*, int*>>{{"video", &r.video}, {"audio", &r.audio}, {"anc", &r.anc}})
            {
                if (!c.contains(key))
                {
                    *target = 0;
                    continue;
                }
                if (!c[key].is_number_integer() || c[key].get<int>() < 0 || c[key].get<int>() > 32)
                {
                    errors.push_back({std::string("/counts/") + key, "must be an integer 0..32"});
                    continue;
                }
                *target = c[key].get<int>();
            }
        }
        return r;
    }

    nlohmann::ordered_json makeGroup(GroupRequest const& request, nlohmann::json const& body, std::function<util::Uuid()> newUid)
    {
        nlohmann::ordered_json g;
        g["uid"] = newUid().toString();
        g["label"] = request.label;
        g["direction"] = request.direction;
        g["domain"] = request.domain;
        g["redundancy"] = request.redundancy;
        g["enabled"] = true;
        if (request.direction == "egress")
        {
            g["output_delay_ns"] = nullptr;
            g["missing_data"] = "black";
        }
        auto list = [&](char const* key, int count, auto&& make)
        {
            nlohmann::ordered_json arr = nlohmann::ordered_json::array();
            if (body.contains(key) && body[key].is_array())
            {
                for (auto e : body[key])
                {
                    nlohmann::ordered_json oe = e;
                    if (!oe.contains("uid"))
                    {
                        oe["uid"] = newUid().toString();
                    }
                    arr.push_back(oe);
                }
            }
            else
            {
                for (int i = 1; i <= count; ++i)
                {
                    arr.push_back(make(i));
                }
            }
            g[key] = arr;
        };
        list("video", request.video,
             [&](int i)
             {
                 nlohmann::ordered_json e;
                 e["uid"] = newUid().toString();
                 e["label"] = request.label + " V" + (request.video > 1 ? std::to_string(i) : std::string());
                 e["width"] = 1920;
                 e["height"] = 1080;
                 e["rate"] = "50/1";
                 e["interlace"] = "progressive";
                 e["colorimetry"] = "BT709";
                 e["tcs"] = "SDR";
                 e["payload_type"] = 96;
                 e["packing"] = "BPM";
                 if (request.direction == "egress")
                 {
                     e["pacing"] = "narrow";
                 }
                 return e;
             });
        list("audio", request.audio,
             [&](int i)
             {
                 nlohmann::ordered_json e;
                 e["uid"] = newUid().toString();
                 e["label"] = request.label + " A" + std::to_string(8 * i - 7) + "-" + std::to_string(8 * i);
                 e["channels"] = 8;
                 e["bit_depth"] = 24;
                 e["sample_rate"] = 48000;
                 e["ptime_us"] = 1000;
                 e["block_us"] = 1000;
                 e["payload_type"] = 97;
                 return e;
             });
        list("anc", request.anc,
             [&](int i)
             {
                 nlohmann::ordered_json e;
                 e["uid"] = newUid().toString();
                 e["label"] = request.label + " ANC" + (request.anc > 1 ? std::to_string(i) : std::string());
                 e["payload_type"] = 100;
                 return e;
             });
        return g;
    }

    nlohmann::ordered_json duplicateGroup(nlohmann::ordered_json group, std::function<util::Uuid()> newUid)
    {
        group["uid"] = newUid().toString();
        if (group.contains("label") && group["label"].is_string())
        {
            group["label"] = group["label"].get<std::string>() + " copy";
        }
        for (auto const* key : {"video", "audio", "anc"})
        {
            if (group.contains(key) && group[key].is_array())
            {
                for (auto& e : group[key])
                {
                    e["uid"] = newUid().toString();
                }
            }
        }
        return group;
    }
}
