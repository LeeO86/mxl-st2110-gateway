// SPDX-License-Identifier: MIT
#include "ops/metrics_export.hpp"

#include "timing/ptp.hpp"
#include "version.hpp"

namespace mxlgw::ops
{
    char const* portLabel(std::size_t index)
    {
        return index == 0 ? "p" : "r";
    }

    namespace
    {
        Labels essenceLabels(group::EssenceSnapshot const& e)
        {
            return {{"group", e.groupLabel},
                    {"essence", e.label},
                    {"uid", e.uid.toString()},
                    {"type", config::toName(e.type)},
                    {"direction", config::toName(e.direction)}};
        }

        Labels with(Labels l, std::string key, std::string value)
        {
            l.emplace_back(std::move(key), std::move(value));
            return l;
        }

        double d(std::uint64_t v)
        {
            return static_cast<double>(v);
        }

        void exportPtp(MetricsWriter& out, media::BackendStatus const& b)
        {
            // ptp.mode = external or the kernel backend: MTL runs no PTP, so a "0 = unlocked" series would mislead.
            if (!b.ptpAvailable)
            {
                return;
            }
            std::string bindMode = b.ports.empty() ? b.backend : b.ports.front().bindMode;
            for (std::size_t i = 0; i < b.ports.size() && i < b.ptp.size(); ++i)
            {
                auto const& p = b.ptp[i];
                Labels const port{{"port", portLabel(i)}};
                out.gauge("mxlgw_ptp_locked", "PTP locked on this port (1/0)", port, p.locked ? 1 : 0);
                out.gauge("mxlgw_ptp_selected", "This port's PTP instance steers the PHC (dual-port BMCA)", port, p.selected ? 1 : 0);
                if (!p.active)
                {
                    continue;
                }
                std::string const gm = p.announce ? timing::formatClockIdentity(p.announce->grandmaster) : std::string();
                std::string const parent = p.parent ? p.parent->toString() : std::string();
                out.gauge("mxlgw_ptp_info", "PTP grandmaster and parent of this port",
                          {{"port", portLabel(i)},
                           {"gm_identity", gm},
                           {"parent_port_identity", parent},
                           {"domain", std::to_string(p.domain)},
                           {"bind_mode", bindMode}},
                          1);
                out.gauge("mxlgw_ptp_offset_ns", "PTP offset from master (ns)", port, static_cast<double>(p.lastDeltaNs));
                out.gauge("mxlgw_ptp_offset_ns", "PTP offset from master (ns)", with(port, "stat", "min"), static_cast<double>(p.minDeltaNs));
                out.gauge("mxlgw_ptp_offset_ns", "PTP offset from master (ns)", with(port, "stat", "max"), static_cast<double>(p.maxDeltaNs));
                out.gauge("mxlgw_ptp_path_delay_ns", "PTP mean path delay (ns)", port, static_cast<double>(p.lastPathDelayNs));
                out.gauge("mxlgw_ptp_path_delay_ns", "PTP mean path delay (ns)", with(port, "stat", "min"), static_cast<double>(p.minPathDelayNs));
                out.gauge("mxlgw_ptp_path_delay_ns", "PTP mean path delay (ns)", with(port, "stat", "max"), static_cast<double>(p.maxPathDelayNs));
                out.gauge("mxlgw_ptp_utc_offset_seconds", "UTC offset announced by the grandmaster (display only)", port, p.utcOffset);
                out.counter("mxlgw_ptp_gm_changes_total", "Grandmaster changes seen in Announce messages", port, d(p.gmChanges));
                out.counter("mxlgw_ptp_sync_total", "PTP Sync messages processed", port, d(p.syncCount));
                out.counter("mxlgw_ptp_errors_total", "PTP errors by kind", with(port, "kind", "rx_sync"), d(p.errorsRxSync));
                out.counter("mxlgw_ptp_errors_total", "PTP errors by kind", with(port, "kind", "tx_sync"), d(p.errorsTxSync));
                out.counter("mxlgw_ptp_errors_total", "PTP errors by kind", with(port, "kind", "result"), d(p.errorsResult));
                out.counter("mxlgw_ptp_errors_total", "PTP errors by kind", with(port, "kind", "timeout"), d(p.errorsTimeout));
            }
            out.counter("mxlgw_ptp_selection_changes_total", "Dual-port BMCA selection changes", {}, d(b.ptpSelectionChanges));
        }

        void exportNic(MetricsWriter& out, media::BackendStatus const& b)
        {
            for (std::size_t i = 0; i < b.ports.size(); ++i)
            {
                auto const& p = b.ports[i];
                Labels const port{{"port", portLabel(i)}};
                out.gauge("mxlgw_nic_link_up", "Media port link state (1/0)", port, p.linkUp ? 1 : 0);
                out.gauge("mxlgw_nic_link_speed_mbps", "Media port link speed", port, p.linkSpeedMbps);
                out.gauge(
                    "mxlgw_nic_info", "Media port identity",
                    {{"port", portLabel(i)}, {"mac", p.mac}, {"pci", p.pci.empty() ? p.ifname : p.pci}, {"driver", p.driver}, {"ddp_package", p.ddpPackage}},
                    1);
                out.counter("mxlgw_nic_rx_packets_total", "Packets received on the port", port, d(p.rxPackets));
                out.counter("mxlgw_nic_tx_packets_total", "Packets sent on the port", port, d(p.txPackets));
                out.counter("mxlgw_nic_rx_bytes_total", "Bytes received on the port", port, d(p.rxBytes));
                out.counter("mxlgw_nic_tx_bytes_total", "Bytes sent on the port", port, d(p.txBytes));
                out.counter("mxlgw_nic_rx_errors_total", "Erroneous packets received", port, d(p.rxErrors));
                out.counter("mxlgw_nic_rx_missed_total", "Packets dropped by the NIC (no RX buffer)", port, d(p.rxMissed));
            }
        }

        void exportEssence(MetricsWriter& out, group::EssenceSnapshot const& e)
        {
            auto const l = essenceLabels(e);
            for (auto const s : group::allEssenceStates)
            {
                out.gauge("mxlgw_essence_state", "1 for the current essence state", with(l, "state", group::toName(s)), e.state.state == s ? 1 : 0);
            }
            if (e.direction == config::Direction::Ingest)
            {
                out.counter("mxlgw_rx_frames_total", "Frames received by result", with(l, "result", "complete"), d(e.rx.framesComplete));
                out.counter("mxlgw_rx_frames_total", "Frames received by result", with(l, "result", "incomplete"), d(e.rx.framesIncomplete));
                out.counter("mxlgw_rx_frames_total", "Frames received by result", with(l, "result", "dropped"), d(e.rx.framesDropped + e.framesDropped));
                for (std::size_t leg = 0; leg < 2; ++leg)
                {
                    out.counter("mxlgw_rx_leg_packets_total", "Packets received per ST 2022-7 leg", with(l, "leg", portLabel(leg)), d(e.rx.legs[leg].packets));
                    out.counter("mxlgw_rx_leg_seq_lost_total", "Sequence gaps per ST 2022-7 leg", with(l, "leg", portLabel(leg)), d(e.rx.legs[leg].lost));
                }
                out.counter("mxlgw_rx_packets_total", "Packets after the ST 2022-7 merge", l, d(e.rx.packets));
                if (e.originAgeNs)
                {
                    out.gauge("mxlgw_ingest_origin_age_ns", "now_tai - origin time of the last frame", l, static_cast<double>(*e.originAgeNs));
                }
                out.counter("mxlgw_mxl_grains_written_total", "MXL grains committed", l, d(e.grainsWritten));
                out.counter("mxlgw_mxl_samples_written_total", "MXL audio samples committed", l, d(e.samplesWritten));
                out.counter("mxlgw_mxl_write_errors_total", "MXL writes rejected", l, d(e.writeErrors));
                return;
            }
            out.counter("mxlgw_mxl_grains_read_total", "MXL grains (or audio blocks) read", l, d(e.grainsRead));
            out.counter("mxlgw_mxl_read_timeouts_total", "Grains/blocks with no data by their deadline", l, d(e.readTimeouts));
            out.counter("mxlgw_mxl_late_reads_total", "Data that arrived after its deadline and was skipped", l, d(e.lateReads));
            out.counter("mxlgw_mxl_grains_invalid_total", "Grains read with MXL_GRAIN_FLAG_INVALID", l, d(e.grainsInvalid));
            out.counter("mxlgw_mxl_flow_not_found_total", "Reader attempts that found no flow or no domain", l, d(e.flowNotFound));
            if (e.readLagGrains)
            {
                out.gauge("mxlgw_mxl_read_lag_grains", "Writer head index - read index (grains)", l, *e.readLagGrains);
            }
            if (e.reader)
            {
                auto info = l;
                info.emplace_back("domain_id", e.reader->domainId.toString());
                info.emplace_back("domain_path", e.reader->domainPath);
                info.emplace_back("domain_kind", e.reader->domainKind);
                info.emplace_back("flow_id", e.reader->flowId.toString());
                out.gauge("mxlgw_mxl_reader_info", "Resolved MXL domain of an enabled MXL Receiver", info, 1);
            }
            out.counter("mxlgw_tx_frames_total", "Frames sent", l, d(e.tx.framesComplete));
            out.counter("mxlgw_tx_late_frames_total", "Frames that missed their transmit time", l, d(e.tx.framesLate + e.txDropped));
            if (e.leadNs)
            {
                out.gauge("mxlgw_egress_lead_ns", "Time from data available to transmit deadline (negative = late)", l, static_cast<double>(*e.leadNs));
            }
        }
    }

    void exportMetrics(MetricsWriter& out, MetricsInput const& in)
    {
        out.gauge("mxlgw_build_info", "Build information",
                  {{"version", version::gateway}, {"mtl", version::mtl}, {"dpdk", version::dpdk}, {"mxl", version::mxl}, {"nmos_cpp", version::nmosCpp}}, 1);
        out.gauge("mxlgw_ready", "1 when /readyz reports ready", {}, in.ready ? 1 : 0);
        out.gauge("mxlgw_restart_required", "1 when persisted changes need a restart", {}, in.restartRequired ? 1 : 0);
        if (in.backend)
        {
            exportPtp(out, *in.backend);
            exportNic(out, *in.backend);
        }
        if (in.clock && in.clock->valid)
        {
            out.gauge("mxlgw_clock_mtl_minus_host_tai_ns", "MTL PTP time - host CLOCK_TAI (ns)", {}, static_cast<double>(in.clock->offsetNs));
            out.gauge("mxlgw_clock_mtl_minus_host_tai_ns", "MTL PTP time - host CLOCK_TAI (ns)", {{"stat", "min"}}, static_cast<double>(in.clock->min60Ns));
            out.gauge("mxlgw_clock_mtl_minus_host_tai_ns", "MTL PTP time - host CLOCK_TAI (ns)", {{"stat", "max"}}, static_cast<double>(in.clock->max60Ns));
        }
        for (auto const& g : in.groups)
        {
            for (auto const& e : g.essences)
            {
                exportEssence(out, e);
            }
        }
        if (in.scan)
        {
            std::size_t discovered = 0;
            std::size_t mirror = 0;
            for (auto const& dom : in.scan->domains)
            {
                discovered += dom.kind == mxlbridge::DomainKind::Discovered ? 1 : 0;
                mirror += dom.kind == mxlbridge::DomainKind::Mirror ? 1 : 0;
            }
            out.gauge("mxlgw_mxl_discovered_domains", "MXL domains found under mxl.scan_path by kind", {{"kind", "discovered"}},
                      static_cast<double>(discovered));
            out.gauge("mxlgw_mxl_discovered_domains", "MXL domains found under mxl.scan_path by kind", {{"kind", "mirror"}}, static_cast<double>(mirror));
            out.gauge("mxlgw_mxl_discovered_domains", "MXL domains found under mxl.scan_path by kind", {{"kind", "conflict"}},
                      static_cast<double>(in.scan->conflicts.size()));
        }
        for (auto const& dom : in.domains)
        {
            out.gauge("mxlgw_mxl_domain_bytes", "Configured MXL domain filesystem usage", {{"domain", dom.name}, {"kind", "used"}}, d(dom.usedBytes));
            out.gauge("mxlgw_mxl_domain_bytes", "Configured MXL domain filesystem usage", {{"domain", dom.name}, {"kind", "free"}}, d(dom.freeBytes));
            out.gauge("mxlgw_mxl_domain_flows", "Flows in a configured MXL domain", {{"domain", dom.name}}, static_cast<double>(dom.flows));
        }
        for (auto const& [node, registered] : in.nmosRegistered)
        {
            out.gauge("mxlgw_nmos_registered", "1 when the NMOS node is registered with its registry", {{"node", node}}, registered ? 1 : 0);
        }
        for (auto const& [key, value] : in.activations)
        {
            out.counter("mxlgw_nmos_activations_total", "IS-05 activations",
                        {{"kind", std::get<0>(key)}, {"transport", std::get<1>(key)}, {"result", std::get<2>(key)}}, d(value));
        }
    }
}
