// SPDX-License-Identifier: MIT
#include "mtl/mtl_backend.hpp"

#include <arpa/inet.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>

#include "mtl/ethdev.hpp"
#include "mtl/mtl_internal.hpp"
#include "mtl/mtl_sessions.hpp"
#include "util/fs.hpp"
#include "util/logging.hpp"
#include "util/strings.hpp"

namespace mxlgw::media
{
    namespace mtlimpl
    {
        bool parseIp(std::string const& text, std::uint8_t out[MTL_IP_ADDR_LEN])
        {
            in_addr a{};
            if (text.empty() || ::inet_pton(AF_INET, text.c_str(), &a) != 1)
            {
                std::memset(out, 0, MTL_IP_ADDR_LEN);
                return false;
            }
            std::memcpy(out, &a.s_addr, MTL_IP_ADDR_LEN);
            return true;
        }

        std::string ipText(std::uint8_t const ip[MTL_IP_ADDR_LEN])
        {
            char buf[INET_ADDRSTRLEN];
            in_addr a{};
            std::memcpy(&a.s_addr, ip, MTL_IP_ADDR_LEN);
            return ::inet_ntop(AF_INET, &a, buf, sizeof(buf)) != nullptr ? std::string(buf) : std::string();
        }

        SessionLegs sessionLegs(Context const& ctx, std::vector<LegAddress> const& legs)
        {
            SessionLegs s;
            s.count = (ctx.redundantPort && legs.size() >= 2) ? 2 : 1;
            for (int i = 0; i < s.count && i < static_cast<int>(legs.size()); ++i)
            {
                s.legs[static_cast<std::size_t>(i)] = legs[static_cast<std::size_t>(i)];
            }
            return s;
        }

        void fillRxPort(Context const& ctx, st_rx_port& port, std::vector<LegAddress> const& legs, int payloadType)
        {
            auto const s = sessionLegs(ctx, legs);
            port.num_port = static_cast<std::uint8_t>(s.count);
            for (int i = 0; i < s.count; ++i)
            {
                auto const& leg = s.legs[static_cast<std::size_t>(i)];
                std::snprintf(port.port[i], MTL_PORT_MAX_LEN, "%s", ctx.portNames[static_cast<std::size_t>(i)].c_str());
                // A disabled leg (rtp_enabled=false) keeps a zero address: MTL joins nothing on it.
                parseIp(leg.enabled ? leg.destination : std::string(), port.ip_addr[i]);
                parseIp(leg.enabled ? leg.source : std::string(), port.mcast_sip_addr[i]);
                port.udp_port[i] = static_cast<std::uint16_t>(leg.port > 0 ? leg.port : 5004);
            }
            port.payload_type = static_cast<std::uint8_t>(payloadType);
        }

        void fillTxPort(Context const& ctx, st_tx_port& port, std::vector<LegAddress> const& legs, int payloadType)
        {
            auto const s = sessionLegs(ctx, legs);
            port.num_port = static_cast<std::uint8_t>(s.count);
            for (int i = 0; i < s.count; ++i)
            {
                auto const& leg = s.legs[static_cast<std::size_t>(i)];
                std::snprintf(port.port[i], MTL_PORT_MAX_LEN, "%s", ctx.portNames[static_cast<std::size_t>(i)].c_str());
                parseIp(leg.destination, port.dip_addr[i]);
                port.udp_port[i] = static_cast<std::uint16_t>(leg.port > 0 ? leg.port : 5004);
                port.udp_src_port[i] = port.udp_port[i];
            }
            port.payload_type = static_cast<std::uint8_t>(payloadType);
        }

        st_rx_source_info rxSource(Context const& ctx, std::vector<LegAddress> const& legs)
        {
            st_rx_source_info src{};
            auto const s = sessionLegs(ctx, legs);
            for (int i = 0; i < s.count; ++i)
            {
                auto const& leg = s.legs[static_cast<std::size_t>(i)];
                parseIp(leg.enabled ? leg.destination : std::string(), src.ip_addr[i]);
                parseIp(leg.enabled ? leg.source : std::string(), src.mcast_sip_addr[i]);
                src.udp_port[i] = static_cast<std::uint16_t>(leg.port > 0 ? leg.port : 5004);
            }
            return src;
        }

        st_tx_dest_info txDestination(Context const& ctx, std::vector<LegAddress> const& legs)
        {
            st_tx_dest_info dst{};
            auto const s = sessionLegs(ctx, legs);
            for (int i = 0; i < s.count; ++i)
            {
                auto const& leg = s.legs[static_cast<std::size_t>(i)];
                parseIp(leg.destination, dst.dip_addr[i]);
                dst.udp_port[i] = static_cast<std::uint16_t>(leg.port > 0 ? leg.port : 5004);
            }
            return dst;
        }

        enum st_fps fpsOf(util::Rational r)
        {
            auto is = [&](std::int64_t n, std::int64_t d) { return r.num * d == n * r.den; };
            if (is(60000, 1001))
            {
                return ST_FPS_P59_94;
            }
            if (is(50, 1))
            {
                return ST_FPS_P50;
            }
            if (is(30000, 1001))
            {
                return ST_FPS_P29_97;
            }
            if (is(25, 1))
            {
                return ST_FPS_P25;
            }
            if (is(60, 1))
            {
                return ST_FPS_P60;
            }
            if (is(30, 1))
            {
                return ST_FPS_P30;
            }
            if (is(24, 1))
            {
                return ST_FPS_P24;
            }
            if (is(24000, 1001))
            {
                return ST_FPS_P23_98;
            }
            if (is(100, 1))
            {
                return ST_FPS_P100;
            }
            if (is(120, 1))
            {
                return ST_FPS_P120;
            }
            if (is(120000, 1001))
            {
                return ST_FPS_P119_88;
            }
            throw std::runtime_error("rate " + r.toString() + " is not supported by MTL");
        }

        SessionStats rxStats(st_rx_user_stats const& c, std::uint64_t incomplete)
        {
            SessionStats s;
            for (std::size_t i = 0; i < 2; ++i)
            {
                s.legs[i].packets = c.port[i].packets;
                s.legs[i].bytes = c.port[i].bytes;
                s.legs[i].lost = c.port[i].lost_packets;
            }
            s.packets = c.stat_pkts_received;
            s.framesIncomplete = incomplete + c.stat_frames_corrupted;
            s.framesComplete = c.stat_frames_received > s.framesIncomplete ? c.stat_frames_received - s.framesIncomplete : 0;
            s.framesDropped = c.stat_frames_dropped;
            return s;
        }

        SessionStats txStats(st_tx_user_stats const& c)
        {
            SessionStats s;
            for (std::size_t i = 0; i < 2; ++i)
            {
                s.legs[i].packets = c.port[i].packets;
                s.legs[i].bytes = c.port[i].bytes;
            }
            s.packets = c.port[0].packets;
            s.framesComplete = c.stat_frames_sent;
            s.framesDropped = c.stat_frames_dropped;
            s.framesLate = c.stat_epoch_drop + c.stat_frames_dropped;
            return s;
        }

        std::uint16_t queueDepth(int requested, int fallback, int maximum)
        {
            auto const v = requested > 0 ? requested : fallback;
            return static_cast<std::uint16_t>(std::clamp(v, 2, maximum));
        }
    }

    namespace
    {
        using namespace mtlimpl;

        // ---------------------------------------------------------------- logging (§13)
        log::Level levelOf(enum mtl_log_level level)
        {
            switch (level)
            {
                case MTL_LOG_LEVEL_DEBUG: return log::Level::Debug;
                case MTL_LOG_LEVEL_INFO:
                case MTL_LOG_LEVEL_NOTICE: return log::Level::Info;
                case MTL_LOG_LEVEL_WARNING: return log::Level::Warn;
                default: return log::Level::Error;
            }
        }

        /// MTL logs from lcores: only enqueue into the lock-free ring (drained by the logging thread).
        void mtlPrinter(enum mtl_log_level level, char const* format, ...)
        {
            va_list args;
            va_start(args, format);
            log::enqueueRealtime(levelOf(level), "mtl", format, args);
            va_end(args);
        }

        /// No MTL timestamp prefix: our log lines carry their own `ts`.
        void noPrefix(char* buf, size_t size)
        {
            if (size > 0)
            {
                buf[0] = '\0';
            }
        }

        /// Queue budget from the configured essences plus headroom for groups added live (§9.3).
        struct QueueBudget
        {
            std::uint16_t tx = 0;
            std::uint16_t rx = 0;
        };

        QueueBudget queueBudget(config::Config const& cfg, bool kernel)
        {
            std::uint16_t v[2]{};
            std::uint16_t a[2]{};
            std::uint16_t n[2]{};
            for (auto const& g : cfg.groups)
            {
                auto const tx = g.direction == config::Direction::Egress ? 1 : 0;
                v[tx] = static_cast<std::uint16_t>(v[tx] + g.video.size());
                a[tx] = static_cast<std::uint16_t>(a[tx] + g.audio.size());
                n[tx] = static_cast<std::uint16_t>(n[tx] + g.anc.size());
            }
            std::uint16_t const extra = kernel ? 1 : 4;
            QueueBudget b;
            b.rx = st_rx_sessions_queue_cnt(static_cast<std::uint16_t>(v[0] + extra), static_cast<std::uint16_t>(a[0] + extra),
                                            static_cast<std::uint16_t>(n[0] + extra), 0);
            b.tx = st_tx_sessions_queue_cnt(static_cast<std::uint16_t>(v[1] + extra), static_cast<std::uint16_t>(a[1] + extra),
                                            static_cast<std::uint16_t>(n[1] + extra), 0);
            return b;
        }

        ssize_t dpdkWrite(void*, char const* buf, size_t size)
        {
            log::enqueueRealtimeF(log::Level::Info, "dpdk", "%.*s", static_cast<int>(size), buf);
            return static_cast<ssize_t>(size);
        }

        std::mutex ddpMutex;
        std::string ddpPackage;

        void observeDdp(log::Level, std::string_view, std::string_view message)
        {
            // ice PMD: "Active package is: 1.3.35.0, ICE OS Default Package (single VLAN mode)"
            auto const pos = message.find("Active package is");
            if (pos == std::string_view::npos)
            {
                return;
            }
            auto text = std::string(message.substr(pos + 17));
            auto const start = text.find_first_not_of(": ");
            text = start == std::string::npos ? std::string() : text.substr(start);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            {
                text.pop_back();
            }
            std::lock_guard const lock{ddpMutex};
            ddpPackage = text;
        }

        std::string currentDdp()
        {
            std::lock_guard const lock{ddpMutex};
            return ddpPackage;
        }

        std::string resolvePci(std::string const& pci)
        {
            if (pci.rfind("env:", 0) != 0)
            {
                return pci;
            }
            auto const var = pci.substr(4);
            auto const* value = std::getenv(var.c_str());
            if (value == nullptr || *value == '\0')
            {
                throw std::runtime_error("PCI address variable " + var + " is not set (Kubernetes device plugin, §15.2)");
            }
            // The SR-IOV device plugin may inject a comma list; one resource per port means one entry.
            auto const list = util::split(value, ',');
            return util::trim(list.front());
        }

        std::string sysfsRead(std::string const& path)
        {
            auto const text = util::readFile(path);
            return text ? util::trim(*text) : std::string();
        }

        std::uint64_t taiNowCallback(void*)
        {
            timespec ts{};
            ::clock_gettime(CLOCK_TAI, &ts);
            return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<std::uint64_t>(ts.tv_nsec);
        }

        class MtlBackend final : public MediaBackend
        {
        public:
            explicit MtlBackend(config::Config const& cfg)
                : _cfg(cfg)
            {
                if (cfg.nic.portPairs.empty())
                {
                    throw std::runtime_error("nic.port_pairs is empty");
                }
                auto const& pair = cfg.nic.portPairs.front();
                _ctx.kernel = cfg.nic.backend == config::Backend::Kernel;
                _ctx.redundantPort = pair.redundant.has_value();
                _ports.push_back(pair.primary);
                if (pair.redundant)
                {
                    _ports.push_back(*pair.redundant);
                }

                static std::once_flag logOnce;
                std::call_once(logOnce,
                               []
                               {
                                   mtl_set_log_printer(&mtlPrinter);
                                   mtl_set_log_prefix_formatter(&noPrefix);
                                   cookie_io_functions_t io{nullptr, &dpdkWrite, nullptr, nullptr};
                                   if (FILE* stream = fopencookie(nullptr, "w", io); stream != nullptr)
                                   {
                                       setvbuf(stream, nullptr, _IOLBF, 0);
                                       mtl_openlog_stream(stream);
                                   }
                                   log::setRealtimeObserver(&observeDdp);
                               });

                mtl_init_params p{};
                p.num_ports = static_cast<std::uint8_t>(_ports.size());
                for (std::size_t i = 0; i < _ports.size(); ++i)
                {
                    auto const& port = _ports[i];
                    if (_ctx.kernel)
                    {
                        _ctx.portNames[i] = "kernel:" + port.ifname;
                        p.pmd[i] = MTL_PMD_KERNEL_SOCKET;
                        // The interface's kernel IP is used (§17.2).
                        std::uint8_t ip[MTL_IP_ADDR_LEN]{};
                        std::uint8_t mask[MTL_IP_ADDR_LEN]{};
                        std::string ifname = port.ifname;
                        if (mtl_get_if_ip(ifname.data(), ip, mask) == 0)
                        {
                            std::memcpy(p.sip_addr[i], ip, MTL_IP_ADDR_LEN);
                            std::memcpy(p.netmask[i], mask, MTL_IP_ADDR_LEN);
                            _ctx.portIps[i] = ipText(ip);
                        }
                        else
                        {
                            parseIp(port.ip, p.sip_addr[i]);
                            parseIp(port.netmask, p.netmask[i]);
                            _ctx.portIps[i] = port.ip;
                        }
                    }
                    else
                    {
                        _ctx.portNames[i] = resolvePci(port.pci);
                        p.pmd[i] = MTL_PMD_DPDK_USER;
                        parseIp(port.ip, p.sip_addr[i]);
                        parseIp(port.netmask, p.netmask[i]);
                        parseIp(port.gateway, p.gateway[i]);
                        _ctx.portIps[i] = port.ip;
                    }
                    std::snprintf(p.port[i], MTL_PORT_MAX_LEN, "%s", _ctx.portNames[i].c_str());
                    auto const budget = queueBudget(cfg, _ctx.kernel);
                    p.tx_queues_cnt[i] = budget.tx;
                    p.rx_queues_cnt[i] = budget.rx;
                }
                p.flags = MTL_FLAG_BIND_NUMA;
                if (_ctx.kernel)
                {
                    // Test-only kernel backend (§17.2): the scheduler is a normal thread that sleeps when its
                    // tasklets are idle, instead of a pinned busy-polling lcore that starves small CI hosts.
                    p.flags |= MTL_FLAG_TASKLET_THREAD | MTL_FLAG_TASKLET_SLEEP;
                }
                if (!cfg.nic.lcores.empty())
                {
                    _lcores = cfg.nic.lcores;
                    p.lcores = _lcores.data();
                }
                // auto tries the NIC rate limiter first. On E810 that restarts the port, and when the restart fails
                // (ice "Failed to add lan txq") MTL's fallback to TSC did not always recover; tsc never tries it.
                // The kernel backend has no rate limiter (rl would fail): it keeps auto, which is TSC there.
                if (!_ctx.kernel)
                {
                    p.pacing = cfg.nic.txPacing == config::TxPacing::Rl    ? ST21_TX_PACING_WAY_RL
                               : cfg.nic.txPacing == config::TxPacing::Tsc ? ST21_TX_PACING_WAY_TSC
                                                                           : ST21_TX_PACING_WAY_AUTO;
                }
                p.log_level = MTL_LOG_LEVEL_INFO; // DPDK INFO carries the ice DDP version; lowered after start
                bool const builtinPtp = !_ctx.kernel && cfg.ptp.mode != config::PtpMode::External;
                if (builtinPtp)
                {
                    p.flags |= MTL_FLAG_PTP_ENABLE;
                    if (bindMode(0) == "pf")
                    {
                        p.flags |= MTL_FLAG_PTP_PI;
                    }
                    if (cfg.ptp.mode == config::PtpMode::BuiltinPhc2sys)
                    {
                        p.flags |= MTL_FLAG_PHC2SYS_ENABLE;
                    }
#if defined(MTL_HAS_PTP_DOMAIN_FILTER)
                    p.flags |= MTL_FLAG_PTP_DOMAIN_FILTER;
                    p.ptp_domain = static_cast<std::uint8_t>(cfg.ptp.domain);
#endif
#if defined(MTL_HAS_PTP_DUAL_PORT)
                    // Patch 0003: best-master selection and announce timeout (parent re-selection), and with a
                    // redundant port PTP on both legs with the BMCA across them (§4.3, §5.5).
                    p.flags |= MTL_FLAG_PTP_BMCA;
                    if (_ctx.redundantPort)
                    {
                        p.flags |= MTL_FLAG_PTP_DUAL_PORT;
                    }
#endif
                }
                else
                {
                    // ptp.mode = external (and always for the kernel backend): the host is the time authority.
                    p.ptp_get_time_fn = &taiNowCallback;
                }
                _ptpBuiltin = builtinPtp;
                log::info("mtl_init", {{"version", mtl_version()},
                                       {"backend", config::toName(cfg.nic.backend)},
                                       {"ports", _ctx.portNames[0] + (_ctx.redundantPort ? "," + _ctx.portNames[1] : std::string())},
                                       {"ptp", builtinPtp ? config::toName(cfg.ptp.mode) : "external"},
                                       {"tx_pacing", config::toName(cfg.nic.txPacing)},
                                       {"lcores", cfg.nic.lcores}});
                _ctx.mt = mtl_init(&p);
                if (_ctx.mt == nullptr)
                {
                    log::drainNow();
                    throw std::runtime_error("mtl_init failed (see component=mtl log lines)");
                }
                if (mtl_start(_ctx.mt) < 0)
                {
                    mtl_uninit(_ctx.mt);
                    _ctx.mt = nullptr;
                    throw std::runtime_error("mtl_start failed");
                }
                log::drainNow();
                mtl_set_log_level(_ctx.mt, log::enabled(log::Level::Debug) ? MTL_LOG_LEVEL_INFO : MTL_LOG_LEVEL_WARNING);
                log::info("mtl_started", {{"bind_mode", bindMode(0)}, {"ddp_package", currentDdp()}});
                if (!_ctx.kernel && currentDdp().find("Safe") != std::string::npos)
                {
                    log::error("ddp_safe_mode", {{"details", "the ice PMD runs in safe mode: install the E810 DDP package (R6, §14.1)"}});
                }
            }

            ~MtlBackend() override
            {
                if (_ctx.mt != nullptr)
                {
                    mtl_stop(_ctx.mt);
                    mtl_uninit(_ctx.mt);
                    _ctx.mt = nullptr;
                    log::info("mtl_stopped", {});
                }
            }

            std::string name() const override { return config::toName(_cfg.nic.backend); }

            std::int64_t ptpTimeNs() const override
            {
                if (!_ptpBuiltin)
                {
                    return static_cast<std::int64_t>(taiNowCallback(nullptr));
                }
                return static_cast<std::int64_t>(mtl_ptp_read_time(_ctx.mt));
            }

            BackendStatus status() const override
            {
                BackendStatus st;
                st.backend = name();
                st.testBackend = _ctx.kernel;
                st.mtlVersion = mtl_version();
                for (std::size_t i = 0; i < _ports.size(); ++i)
                {
                    auto const& cp = _ports[i];
                    PortStatus ps;
                    ps.name = cp.name;
                    ps.pci = _ctx.kernel ? std::string() : _ctx.portNames[i];
                    ps.ifname = cp.ifname;
                    ps.ip = _ctx.portIps[i];
                    ps.bindMode = bindMode(i);
                    if (_ctx.kernel)
                    {
                        auto const base = "/sys/class/net/" + cp.ifname;
                        ps.mac = sysfsRead(base + "/address");
                        ps.linkUp = sysfsRead(base + "/operstate") == "up" || sysfsRead(base + "/carrier") == "1";
                        if (auto const speed = util::parseInt(sysfsRead(base + "/speed")); speed && *speed > 0)
                        {
                            ps.linkSpeedMbps = static_cast<std::uint32_t>(*speed);
                        }
                        ps.driver = "kernel-socket";
                    }
                    else
                    {
                        auto const info = ethdev::query(_ctx.portNames[i]);
                        ps.mac = info.mac;
                        ps.linkUp = info.linkUp;
                        ps.linkSpeedMbps = info.speedMbps;
                        ps.driver = info.driver;
                        ps.ddpPackage = currentDdp();
                    }
                    mtl_port_status stats{};
                    if (mtl_get_port_stats(_ctx.mt, static_cast<mtl_port>(i), &stats) == 0)
                    {
                        ps.rxPackets = stats.rx_packets;
                        ps.txPackets = stats.tx_packets;
                        ps.rxBytes = stats.rx_bytes;
                        ps.txBytes = stats.tx_bytes;
                        ps.rxErrors = stats.rx_err_packets;
                        ps.rxMissed = stats.rx_hw_dropped_packets + stats.rx_nombuf_packets;
                    }
                    st.ports.push_back(ps);
                }
#if defined(MTL_HAS_PTP_STATUS)
                if (_ptpBuiltin)
                {
                    st.ptpAvailable = true;
                    for (std::size_t i = 0; i < _ports.size() && i < st.ptp.size(); ++i)
                    {
                        mtl_ptp_status s{};
                        if (mtl_ptp_get_status(_ctx.mt, static_cast<mtl_port>(i), &s) != 0)
                        {
                            continue;
                        }
                        auto& out = st.ptp[i];
                        out.active = s.active;
                        out.locked = s.locked;
                        out.selected = s.selected;
                        if (s.master_initialized)
                        {
                            timing::PortIdentity parent;
                            std::memcpy(parent.clock.data(), s.parent_clock_identity, 8);
                            parent.port = s.parent_port_number;
                            out.parent = parent;
                            timing::AnnounceInfo a;
                            std::memcpy(a.grandmaster.data(), s.gm_identity, 8);
                            a.priority1 = s.gm_priority1;
                            a.priority2 = s.gm_priority2;
                            a.clockClass = s.gm_clock_class;
                            a.clockAccuracy = s.gm_clock_accuracy;
                            a.offsetScaledLogVariance = s.gm_offset_scaled_log_variance;
                            a.stepsRemoved = s.steps_removed;
                            a.timeSource = s.time_source;
                            a.utcOffset = s.utc_offset;
                            a.domain = s.domain_number;
                            a.sender = parent;
                            out.announce = a;
                        }
                        out.domain = s.domain_number;
                        out.utcOffset = s.utc_offset;
                        out.lastDeltaNs = s.delta_last;
                        out.minDeltaNs = s.delta_min;
                        out.maxDeltaNs = s.delta_max;
                        out.avgDeltaNs = s.delta_avg;
                        out.lastPathDelayNs = s.path_delay_last;
                        out.minPathDelayNs = s.path_delay_min;
                        out.maxPathDelayNs = s.path_delay_max;
                        out.avgPathDelayNs = s.path_delay_avg;
                        out.syncCount = s.sync_count;
                        out.gmChanges = s.gm_change_count;
                        out.errorsRxSync = s.err_rx_sync;
                        out.errorsTxSync = s.err_tx_sync;
                        out.errorsResult = s.err_result;
                        out.errorsTimeout = s.err_timeout;
                        st.ptpSelectionChanges = s.selection_changes;
                        st.phc2sysLocked = s.phc2sys_locked;
                    }
                }
#endif
                return st;
            }

            std::unique_ptr<VideoRxSession> createVideoRx(VideoRxParams const& params, VideoRxHandler& handler) override
            {
                return mtlimpl::createVideoRx(_ctx, params, handler);
            }
            std::unique_ptr<VideoTxSession> createVideoTx(VideoTxParams const& params) override { return mtlimpl::createVideoTx(_ctx, params); }
            std::unique_ptr<AudioRxSession> createAudioRx(AudioParams const& params) override { return mtlimpl::createAudioRx(_ctx, params); }
            std::unique_ptr<AudioTxSession> createAudioTx(AudioParams const& params) override { return mtlimpl::createAudioTx(_ctx, params); }
            std::unique_ptr<AncRxSession> createAncRx(AncParams const& params) override { return mtlimpl::createAncRx(_ctx, params); }
            std::unique_ptr<AncTxSession> createAncTx(AncParams const& params) override { return mtlimpl::createAncTx(_ctx, params); }

        private:
            /// "pf" / "vf" from sysfs (§4.2), "kernel" for the test backend.
            std::string bindMode(std::size_t port) const
            {
                if (_ctx.kernel)
                {
                    return "kernel";
                }
                auto const pci = port < _ctx.portNames.size() && !_ctx.portNames[port].empty() ? _ctx.portNames[port] : resolvePci(_ports.at(port).pci);
                std::error_code ec;
                return std::filesystem::exists("/sys/bus/pci/devices/" + pci + "/physfn", ec) ? "vf" : "pf";
            }

            config::Config _cfg;
            Context _ctx;
            std::vector<config::NicPort> _ports;
            std::string _lcores;
            bool _ptpBuiltin = false;
        };
    }

    std::unique_ptr<MediaBackend> createMtlBackend(config::Config const& config)
    {
        return std::make_unique<MtlBackend>(config);
    }
}
