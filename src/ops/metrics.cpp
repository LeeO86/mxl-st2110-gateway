// SPDX-License-Identifier: MIT
#include "ops/metrics.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace mxlgw::ops
{
    namespace
    {
        std::string formatValue(double v)
        {
            if (std::isnan(v))
            {
                return "NaN";
            }
            if (std::isinf(v))
            {
                return v > 0 ? "+Inf" : "-Inf";
            }
            if (v == std::floor(v) && std::fabs(v) < 1e15)
            {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.0f", v);
                return buf;
            }
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.9g", v);
            return buf;
        }

        std::string escapeHelp(std::string const& help)
        {
            std::string out;
            for (char const c : help)
            {
                if (c == '\\')
                {
                    out += "\\\\";
                }
                else if (c == '\n')
                {
                    out += "\\n";
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }
    }

    std::string escapeLabelValue(std::string const& value)
    {
        std::string out;
        for (char const c : value)
        {
            switch (c)
            {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                default: out += c;
            }
        }
        return out;
    }

    void MetricsWriter::add(std::string const& name, std::string const& type, std::string const& help, char const* suffix, Labels const& labels, double value)
    {
        auto it = _families.find(name);
        if (it == _families.end())
        {
            it = _families.emplace(name, Family{type, help, {}}).first;
            _order.push_back(name);
        }
        it->second.samples.push_back({suffix, labels, value});
    }

    void MetricsWriter::gauge(std::string const& name, std::string const& help, Labels const& labels, double value)
    {
        add(name, "gauge", help, "", labels, value);
    }

    void MetricsWriter::counter(std::string const& name, std::string const& help, Labels const& labels, double value)
    {
        add(name, "counter", help, "", labels, value);
    }

    void MetricsWriter::histogram(std::string const& name, std::string const& help, Labels const& labels, std::span<double const> bounds,
                                  std::span<std::uint64_t const> buckets, double sum)
    {
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < buckets.size(); ++i)
        {
            count += buckets[i];
            auto l = labels;
            l.emplace_back("le", i < bounds.size() ? formatValue(bounds[i]) : "+Inf");
            add(name, "histogram", help, "_bucket", l, static_cast<double>(count));
        }
        add(name, "histogram", help, "_sum", labels, sum);
        add(name, "histogram", help, "_count", labels, static_cast<double>(count));
    }

    std::string MetricsWriter::render() const
    {
        std::ostringstream out;
        for (auto const& name : _order)
        {
            auto const& f = _families.at(name);
            out << "# HELP " << name << ' ' << escapeHelp(f.help) << '\n';
            out << "# TYPE " << name << ' ' << f.type << '\n';
            for (auto const& [suffix, labels, value] : f.samples)
            {
                out << name << suffix;
                if (!labels.empty())
                {
                    out << '{';
                    for (std::size_t i = 0; i < labels.size(); ++i)
                    {
                        if (i > 0)
                        {
                            out << ',';
                        }
                        out << labels[i].first << "=\"" << escapeLabelValue(labels[i].second) << '"';
                    }
                    out << '}';
                }
                out << ' ' << formatValue(value) << '\n';
            }
        }
        return out.str();
    }

    int MetricsRegistry::add(Collector collector)
    {
        std::lock_guard const lock{_mutex};
        int const id = ++_nextId;
        _collectors[id] = std::move(collector);
        return id;
    }

    void MetricsRegistry::remove(int id)
    {
        std::lock_guard const lock{_mutex};
        _collectors.erase(id);
    }

    std::string MetricsRegistry::scrape() const
    {
        MetricsWriter writer;
        std::lock_guard const lock{_mutex};
        for (auto const& [id, collector] : _collectors)
        {
            collector(writer);
        }
        return writer.render();
    }
}
