// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace mxlgw::ops
{
    using Labels = std::vector<std::pair<std::string, std::string>>;

    /// Collects samples during one scrape and renders Prometheus text exposition format 0.0.4.
    class MetricsWriter
    {
    public:
        void gauge(std::string const& name, std::string const& help, Labels const& labels, double value);
        void counter(std::string const& name, std::string const& help, Labels const& labels, double value);
        std::string render() const;

    private:
        struct Family
        {
            std::string type;
            std::string help;
            std::vector<std::pair<Labels, double>> samples;
        };
        void add(std::string const& name, std::string const& type, std::string const& help, Labels const& labels, double value);
        std::vector<std::string> _order;
        std::map<std::string, Family> _families;
    };

    /// Minimal registry (no external Prometheus library, as in mxl-decklink): components register
    /// collectors that export their own atomic counters at scrape time.
    class MetricsRegistry
    {
    public:
        using Collector = std::function<void(MetricsWriter&)>;

        int add(Collector collector);
        void remove(int id);
        std::string scrape() const;

    private:
        mutable std::mutex _mutex;
        int _nextId = 0;
        std::map<int, Collector> _collectors;
    };

    std::string escapeLabelValue(std::string const& value);
}
