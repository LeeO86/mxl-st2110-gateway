// SPDX-License-Identifier: MIT
#include "ops/statusz.hpp"

#include <sstream>

namespace mxlgw::ops
{
    namespace
    {
        std::string s(nlohmann::json const& j, char const* key)
        {
            if (!j.is_object() || !j.contains(key) || j[key].is_null())
            {
                return "-";
            }
            return j[key].is_string() ? j[key].get<std::string>() : j[key].dump();
        }
    }

    std::string renderStatusz(nlohmann::json const& st)
    {
        std::ostringstream out;
        auto const& node = st.value("node", nlohmann::json::object());
        out << "mxl-st2110-gateway " << s(st.value("versions", nlohmann::json::object()), "gateway") << "\n";
        out << "node        " << s(node, "label") << " (" << s(node, "id") << ")\n";
        auto const& ready = st.value("readiness", nlohmann::json::object());
        out << "ready       " << (ready.value("ready", false) ? "yes" : "no");
        if (ready.contains("reasons") && !ready["reasons"].empty())
        {
            out << "  reasons: " << ready["reasons"].dump();
        }
        out << "\n";
        out << "mode        " << (st.value("setup_mode", false) ? "setup" : "running") << ", backend "
            << s(st.value("nic", nlohmann::json::object()), "backend") << "\n";
        if (st.contains("ptp"))
        {
            auto const& p = st["ptp"];
            out << "ptp         mode " << s(p, "mode") << ", selected port " << s(p, "selected_port") << ", clock offset " << s(p, "mtl_minus_host_tai_ns")
                << " ns\n";
        }
        if (st.contains("domains"))
        {
            out << "domains\n";
            for (auto const& d : st["domains"])
            {
                out << "  " << s(d, "kind") << " " << s(d, "id") << " " << s(d, "path") << " flows=" << s(d, "flow_count") << "\n";
            }
        }
        if (st.contains("groups"))
        {
            out << "groups\n";
            for (auto const& g : st["groups"])
            {
                out << "  " << s(g, "label") << " [" << s(g, "direction") << "] " << (g.value("enabled", true) ? "" : "(disabled)") << "\n";
                if (g.contains("essences"))
                {
                    for (auto const& e : g["essences"])
                    {
                        out << "    " << s(e, "type") << " " << s(e, "label") << ": " << s(e, "state");
                        if (e.contains("reason") && e["reason"].is_string() && !e["reason"].get<std::string>().empty())
                        {
                            out << " (" << e["reason"].get<std::string>() << ")";
                        }
                        out << "\n";
                    }
                }
            }
        }
        return out.str();
    }
}
