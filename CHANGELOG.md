# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/). Metric names and labels (`docs/metrics.md`), the configuration schema (`schema/gateway-config.schema.json`) and the environment variables (`docs/configuration.md`) are public interfaces: every change to them is listed here, breaking changes bump the schema version or the major version.

## [Unreleased]

First full implementation of SPECIFICATION.md Draft 1.2.

### Added

- **Gateway:** one process with the MTL instance, one MXL instance per domain and the nmos-cpp node, all HTTP routes on one port (`node.http_port`, default 8080).
- **Ingest (ST 2110 → MXL):** ST 2110-20 video converted into real `video/v210` grains (never RFC 4175 wire data), ST 2110-30 L16/L24 → `audio/float32` (1–64 channels, 1 ms / 125 µs), ST 2110-40 → `video/smpte291` (RFC 8331), ST 2022-7 merge with per-leg counters. MXL grain index = RTP timestamp mapped to TAI.
- **Egress (MXL → ST 2110):** one worker and MXL sync group per group, `output_delay_ns` (default two grains), read offsets per receiver, black / silence / empty ANC or `repeat` when data is missing, narrow/linear/wide pacing, ST 2022-7 sending.
- **MXL domains:** bootstrap with the tmpfs check, `domain_def.json` adopt/create with id write-back, `options.json` only when missing, own-flow garbage collection; discovery of sibling and mxl-fabrics-agent mirror domains under `mxl.scan_path`; MXL Receivers that wait for flows and domains with backoff (`waiting_for_flow`, `no_signal`, `flow_removed`) and resume automatically.
- **NMOS:** IS-04 v1.3, IS-05 v1.1/v1.2, BCP-002-01 group hints, BCP-004-01 receiver capabilities, BCP-007-03 MXL Senders/Receivers (unknown domains accepted and waited for, owner decision C3), SDP generation and validation (format mismatch → 400), connection persistence (`state/connections.json`), live group add/edit/remove.
- **PTP:** MTL patches 0001 (status API with per-Announce GM tracking), 0002 (domain filter) and 0003 (BMCA, PTP on both 2022-7 ports with selection); clock supervision MTL − host `CLOCK_TAI`.
- **Configuration schema v1** (`schema_version: 1`) with descriptions and defaults; precedence environment > file > default; environment variables `MXLGW_*` and the aliases `MXLGW_HTTP_PORT`, `MXLGW_LOG_LEVEL`, `MXL_DOMAIN_SCAN_PATH`, `MXL_READ_OFFSET_GRAINS`, `MXL_READ_OFFSET_MS` (`docs/configuration.md`). Backends `dpdk`, `kernel` (test-only) and `mock` (test-only).
- **REST API** (`/api/status`, `/api/config` with ETag/If-Match, export/import with `keep_ids`, validate, schema, groups, nic, ptp, domains, flows, nmos, preflight, logs, restart), health endpoints `/livez`, `/readyz`, `/statusz`.
- **Admin web UI** (Vue 3, single embedded file): dashboard, groups (create by counts, edit, duplicate, delete), NMOS, network, PTP, MXL (domains, flow browser, receivers), configuration (export/import, changed-on-disk resolution, preflight, logs).
- **Metrics** (`docs/metrics.md`): `mxlgw_build_info`, `mxlgw_ready`, `mxlgw_restart_required`, `mxlgw_ptp_*`, `mxlgw_clock_mtl_minus_host_tai_ns`, `mxlgw_nic_*`, `mxlgw_essence_state`, `mxlgw_rx_*`, `mxlgw_ingest_origin_age_ns`, `mxlgw_mxl_*`, `mxlgw_tx_*`, `mxlgw_egress_lead_ns`, `mxlgw_nmos_*`. PTP series exist only while MTL runs PTP.
- **Monitoring:** generated Grafana 11 dashboard, Prometheus scrape example, Kubernetes `ServiceMonitor`.
- **Logging:** JSON lines (or text), stable event names, MTL/DPDK output redirected through a lock-free ring, last 500 lines in `/api/logs`.
- **Preflight** (`--preflight`, at start and `/api/preflight`): hugepages, vfio, PCI binding, capabilities, MXL domains, scan path, TAI offset, HTTP port, lcores.
- **Tools:** `mxl-pattern-writer` (v210 bars with frame counter, tones, timecode ANC) and `mxl-verify`.
- **Deployment:** Dockerfile (webui, deps, build, fuzz, runtime stages; E810 DDP package bundled), Compose files (single host, mxl-fabrics-agent host A/B), Kubernetes manifests with an mxl-fabrics-agent two-node kustomization.
- **CI:** lint, unit tests in the image build, MTL patch check, web tests, 60 s libFuzzer per parser, integration tests (smoke, kernel-backend loopback with leg loss, late flow with a mirror domain, AMWA nmos-testing); container publishing to GHCR and releases.

### Fixed

- MTL patch 0004: lost wakeup in MTL's scheduler sleep (1 s stalls under load).
- Kernel backend: MTL's scheduler runs as a sleeping thread instead of a pinned busy-polling lcore, so the CI loopback works on small runners; audio RX keeps 100 ms of frame buffers.
- Integration tests: Avahi start on systemd hosts, promtool image pull output no longer counted as lint findings.
