# Changelog

All notable changes to this project are documented here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [Semantic Versioning](https://semver.org/). Metric names and labels (`docs/metrics.md`), the configuration schema (`schema/gateway-config.schema.json`), the environment variables (`docs/configuration.md`, README "Settings"), the REST API (`/api/v1`) and the exit codes are public interfaces: every change to them is listed here, breaking changes bump the schema version or the major version.

## [Unreleased]

## [1.0.10] - 2026-10-08

### Fixed

- The gateway starts in a Kubernetes Guaranteed pod with exclusive CPUs (completes 1.0.9). MTL v26.09 passes `--remap-lcore-ids` to DPDK 25.11, which numbers the CPUs of the core list from 0, but gave `--main-lcore` the CPU number; the main lcore that 1.0.9 moves into the pod's CPUs (for example CPU 10) therefore failed with "Main lcore is not enabled for DPDK". MTL patch `0005-main-lcore-remap.patch` passes the renumbered id. With CPU 0 allowed (Burstable pod, bare host) nothing changes.

## [1.0.9] - 2026-10-08

### Fixed

- The gateway starts in a Kubernetes Guaranteed pod with exclusive CPUs (static CPU manager). MTL always puts its DPDK main lcore first in EAL's core list, CPU 0 unless set, and EAL pins its init thread there; with CPU 0 reserved and not in the pod's CPUs, EAL failed with `Cannot set affinity` and the gateway exited (`media_backend_failed`, CrashLoopBackOff). The main lcore is now the first app CPU of the affinity (else the first allowed CPU that is no MTL lcore); with CPU 0 allowed (Burstable pod, bare host) nothing changes. `mtl_init` logs `main_lcore`.

## [1.0.8] - 2026-10-08

### Fixed

- Egress audio sends exactly one packet per packet time. On the platform the four audio essences of a TX group sent ~850–900 packets/s instead of 1000 (fewer with more active MXL sources), and the receiving gateway wrote only 41.5–44 k samples/s with gaps. The group's single worker read all essences once per grain period, after the video, and only after the end of the period plus the group's largest read offset; with a read offset close to the output delay (a mirrored flow, 1.0.5 default output delay = one grain + read offset + 2 ms) the first audio blocks of every period reached MTL less than one block before their transmit time, behind the video conversion and the other essences' reads, and `st30p` drops a block that its transport picks up one block period after that time. Audio now has its own worker per egress group: once per block it hands each block to MTL when its data is due (block end + the essence's own read offset), at the latest with silence one block + 1 ms before its transmit time; reads never wait, so a late source only delays its own blocks.
- An audio sender no longer waits up to 1 ms per block for a free frame when MTL's queue is full; the block is counted as late and the worker goes on.
- The media workers no longer call MTL's session statistics. They spin on a lock that MTL's scheduler holds while it serves the session; a real-time worker on the scheduler's CPU could spin until the kernel's real-time throttling stopped it (lab: audio down to 160 packets/s). The status API and `/metrics` read them now.
- Late frames are counted once and no longer touch MTL's memory. MTL v26.09 also hands the `notify_frame_late` callback to its transport sessions, with the pipeline's context as argument: a frame the transport sent late incremented a field inside MTL's pipeline context, and a frame dropped as late was counted twice in `late_frames` / `mxl_st2110_gateway_tx_late_frames_total`. The callback is not registered any more; MTL's session statistics count the drops.

### Added

- `mxl-pattern-writer --audio-delay-us`: commits each audio block that much after its end; with `--block-us 20000` it behaves like a frame-based source that delivers 20 ms chunks late (tests).

### Changed

- The Compose and Kubernetes examples use the `1.0.8` image.

## [1.0.7] - 2026-10-07

### Fixed

- An RTP Receiver receives the payload type of its staged SDP (`a=rtpmap`). It listened for the essence's configured `payload_type`, and MTL drops packets with another payload type, so an SDP with another one received nothing: on the platform every routed VideoIPath audio stream (payload type 98, essences configured with 97) stayed `no_signal` / `no_packets` with 0 packets on both legs while the video of the same senders ran. The configured `payload_type` still applies to senders and to an activation without an SDP. A new payload type in an activation restarts the receiver; `ingest_receiver_started` logs the payload type in use.

### Changed

- The Compose and Kubernetes examples use the `1.0.7` image.

## [1.0.6] - 2026-10-06

### Fixed

- An ST 2110-20 sender is created with at most 8 frames, MTL's limit. The frame queue was the output delay in grains + 3 (up to 16), so an output delay above 100 ms at 50p (for example 120 ms, or the 1.0.5 default of 102 ms with `read_offset_grains` 4) made `st20p_tx_create` fail ("invalid framebuff_cnt 9, should in range [2:8]") and no video was sent; seen on the platform hardware.
- An egress essence whose ST 2110 sender cannot be created is `error` with reason `egress_sender_failed`. It showed `running` (the MXL read side) while no video was sent. The creation is retried after 1 s, doubling up to 30 s, instead of on every grain period.

### Changed

- Validation: in an egress group with video, the effective `output_delay_ns` must be at most the largest read offset + 5 grains (100 ms at 50p beyond the read offset); a grain waits that long in the sender. The default (two grains, at least one grain + the largest read offset + 2 ms) always fits; a configuration with a larger explicit value is rejected instead of sending no video.
- The Compose and Kubernetes examples use the `1.0.6` image.

## [1.0.5] - 2026-10-06

### Changed

- An egress group without `output_delay_ns` uses at least one grain + its largest read offset + 2 ms (default still two grains). A read offset alone, as mirrored flows need (§5.7), made every grain late: on the platform a TX essence reading a mirror of another gateway's ingest flow counted only read timeouts until `output_delay_ns` was also set. A read offset change that changes this default rebuilds the group instead of applying live.
- The Compose and Kubernetes examples use the `1.0.5` image.

### Fixed

- A disable (`master_enable: false`) is always accepted. The MXL flow format check and the SDP check ran on the staged flow or SDP also when the receiver was switched off, so a route that no longer fit (for example a 16-channel flow on a 2-channel essence routed under 1.0.2) could only be disabled by clearing its transport parameters.
- A saved Receiver activation that the current staging checks reject comes up staged disabled after a restart, logged as `restored_activation_disabled` with the reason and shown as essence state `idle` with reason `restore_rejected: …`. It came up enabled, with the essence in `error`.

## [1.0.4] - 2026-10-06

### Changed

- The Compose and Kubernetes examples use the `1.0.4` image.

### Fixed

- A connection restored after a restart (`node.resume_connections`) keeps its `sender_id` (receivers) or `receiver_id` (senders) in IS-05 `/active`. The saved activation had it, but only `master_enable` and the transport parameters were staged again, so `/active` said `null` and a controller no longer showed the route (seen on the platform after a gateway restart).

## [1.0.3] - 2026-10-06

### Changed

- The Compose and Kubernetes examples use the `1.0.3` image.

### Fixed

- Domain discovery accepts a `domain_def.json` with only an `id`. `label`, `description` and `tags` (required by BCP-007-03) are optional when reading: absent they are `""` and `{}`, present they must have the schema's type. mxl-test-player 1.0.3, mxl-color-corrector 1.0.4 and mxl-fabrics-agent 1.0.2 mirrors write no `description` or `tags`; the gateway skipped those domains (`mxl_domain_skipped`, "missing required field 'description'") and their egress stayed in `waiting_for_flow` with `domain_not_found`. A configured domain whose existing file lacks these fields is adopted instead of failing with `mxl_domain_def_invalid`.

## [1.0.2] - 2026-10-06

Checked on the MXL PoC platform's hosts (Dell Precision 3930, E810-XXV on vfio-pci, NVM 5.01, DDP 1.3.59.0): with `nic.tx_pacing = tsc` the host whose E810 failed the rate-limiter test starts without `Failed to add lan txq` or a stack dump and is ready; the other host is ready with `auto`. Both run `ptp.mode = external`.

### Changed

- The Compose and Kubernetes examples use the `1.0.2` image.

### Added

- `nic.tx_pacing` (`MXLGW_NIC_TX_PACING`): MTL's TX pacing, `auto` (default, unchanged behaviour), `rl` or `tsc`. On an E810 PF (E810-XXV, NVM 5.01, 8 TX queues) the rate-limiter test of `auto` restarts the port, the restart fails with ice `Failed to add lan txq` and a stack dump, and MTL falls back to TSC; in one of two runs `mtl_init` still failed. `tsc` skips the rate limiter. Ignored by the kernel backend. The `mtl_init` log line shows the value.

### Fixed

- The entrypoint no longer warns `entrypoint_memlock` when the process has `CAP_IPC_LOCK`, which lifts the memlock limit for DPDK (mlock and VFIO DMA mappings). Without the capability the warning stays.
- README troubleshooting: `Failed to add lan txq` (use `nic.tx_pacing = tsc`) and `PTP(0): t3 tx timestamp timeout` (use `ptp.mode = external` with a TAI-disciplined host clock).

## [1.0.1] - 2026-10-04

### Changed

- The Compose and Kubernetes examples use the `1.0.1` image.

### Fixed

- Container workflow: `git-<sha7>` is written only by the push to `main`, so the release build and the nightly or manual rebuilds of the same commit no longer move it (the `v1.0.0` build had moved `git-151df45`); a release build fails when its `X.Y.Z` tag already exists.
- `mxl_st2110_gateway_rx_frames_total{result="dropped"}` now includes the video frames and audio blocks MTL discards when the ingest worker falls behind and no frame buffer is free (`stat_slot_get_frame_fail`); before, such drops reached only MTL's log ("back-pressure: framebuff pool empty"). Name and labels are unchanged.
- Integration tests (`loopback.sh`, `late-flow.sh`): bad audio blocks are also explained by ingest audio blocks the gateway counted as dropped in the same window (a stall on a loaded CI runner had dropped 28 blocks that no counter showed).

## [1.0.0] - 2026-10-03

First stable release: SPECIFICATION.md 1.3 — the implementation of Draft 1.2 plus the MXL PoC platform guideline G1–G14. The settings, APIs, metrics and behaviour below are a stable contract; a breaking change needs 2.0.0.

### Platform guideline G1–G14

| # | Requirement | Status | How |
|---|---|---|---|
| G1 | configuration: env > file > defaults, invalid → 78, one settings table, state under `/config`, no secrets logged | met | standard names canonical, `MXLGW_*` aliases, alias conflicts exit 78; README "Settings"; state only beside `MXLGW_CONFIG`; no secrets in the file |
| G2 | MXL domains: `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`/`_ID` created if missing, id mismatch → error, own domain only, `history_duration` | met | output domain = first configured domain; `domain_id_mismatch` error, file never overwritten; `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS` |
| G3 | NMOS identity: `NMOS_SEED` → UUIDv5 ids, `NMOS_LABEL`, `NMOS_TAGS`, BCP-002 hints | met | seed namespace for nodes, devices, sources, flows, senders, receivers and the output domain id; tags on nodes and devices |
| G4 | registry: `NMOS_REGISTRY_*`, `NMOS_QUERY_*`, `NMOS_DNS_SD` default false, no Avahi/D-Bus | met | static registry, query address/port reported; DNS-SD off → nmos-cpp `pri`/`highest_pri` = `no_priority` |
| G5 | IP literals only from `NMOS_HOST_ADDRESS` (default first non-loopback IPv4) | met | `href_mode` addresses; validated IPv4; default-route interface, else first non-loopback; old names as aliases |
| G6 | every port by env, two instances per host, bind failure → 75 | met | `NMOS_PORT`, `WEB_PORT`, `MXLGW_NODE_ST2110_HTTP_PORT`; listeners verified after open → exit 75 |
| G7 | `/livez`, `/readyz` (registered when a registry is configured), `/metrics` prefix `mxl_st2110_gateway_` | met | per-node registration readiness, `shutting_down`; metric prefix renamed |
| G8 | SIGTERM: media stop, MXL release, deregister, optional own-domain cleanup, exit 143 within `SHUTDOWN_TIMEOUT_S` | met | ordered shutdown, DELETE of every resource (node last), `MXL_CLEANUP_ON_EXIT`, watchdog |
| G9 | IS-05/BCP-007-03: active `mxl_domain_id`/`mxl_flow_id`, staged PATCH, `master_enable=false`, connection survives restart | met | nmos-testing BCP-007-03-01; restart persistence in `lifecycle.sh` |
| G10 | `GET /api/v1/config/export`, `POST /api/v1/config/import` restore the configuration | met | `/api/v1/*` aliases of every route; import `?restart=true`; no secrets in the file |
| G11 | image: GHCR tags, uid 1000, OCI labels + `io.dmf.mxl.revision`, examples on existing tags | met | `USER 1000:1000` (DPDK deployments `0:1000`, documented); pinned `MXL_REVISION`; examples use `1.0.0` |
| G12 | k8s example: standard env, probes, grace period > timeout, MXL hostPath, writable `/config`, no `hostIPC`, minimal caps | met | `deploy/k8s/deployment.yaml` (host network allowed for the gateway) |
| G13 | docs: settings table, ports, exit codes, API list, platform section; CHANGELOG; SPEC matches the code | met | README, SPECIFICATION.md 1.3, this file |
| G14 | unit tests; integration test start → ready → SIGTERM → deregistered, own domain removed; CI green | met | new unit/app tests; `tests/integration/lifecycle.sh` in the CI matrix |

### Added

- **Gateway:** one process with the MTL instance, one MXL instance per domain and two nmos-cpp nodes — the **MXL node** (MXL Senders/Receivers, `NMOS_PORT`, registers with the MXL registry) and the **ST 2110 node** (RTP Senders/Receivers, `NMOS_PORT + 1`, own optional registry); the admin UI, REST API, metrics and health on the MXL node's port or on `WEB_PORT`.
- **Ingest (ST 2110 → MXL):** ST 2110-20 video converted into real `video/v210` grains (never RFC 4175 wire data), ST 2110-30 L16/L24 → `audio/float32` (1–64 channels, 1 ms / 125 µs), ST 2110-40 → `video/smpte291` (RFC 8331), ST 2022-7 merge with per-leg counters. MXL grain index = RTP timestamp mapped to TAI.
- **Egress (MXL → ST 2110):** one worker and MXL sync group per group, `output_delay_ns` (default two grains), read offsets per receiver, black / silence / empty ANC or `repeat` when data is missing, narrow/linear/wide pacing, ST 2022-7 sending.
- **MXL domains:** bootstrap with the tmpfs check, `domain_def.json` adopt/create with id write-back, `options.json` only when missing, own-flow garbage collection; discovery of sibling and mxl-fabrics-agent mirror domains under `mxl.scan_path`; MXL Receivers that wait for flows and domains with backoff (`waiting_for_flow`, `no_signal`, `flow_removed`) and resume automatically; optional removal of the own domains on SIGTERM.
- **NMOS:** IS-04 v1.3, IS-05 v1.1/v1.2, BCP-002-01 group hints, BCP-004-01 receiver capabilities, BCP-007-03 MXL Senders/Receivers (unknown domains accepted and waited for, owner decision C3), SDP generation and validation (format mismatch → 400), connection persistence (`state/connections.json`), live group add/edit/remove; seed-derived ids, node/device tags, IP-literal hrefs, static registry or DNS-SD, deregistration on shutdown.
- **PTP:** MTL patches 0001 (status API with per-Announce GM tracking), 0002 (domain filter) and 0003 (BMCA, PTP on both 2022-7 ports with selection); clock supervision MTL − host `CLOCK_TAI`.
- **Configuration schema v1** (`schema_version: 1`) with descriptions and defaults; precedence environment > file > default. Backends `dpdk`, `kernel` (test-only) and `mock` (test-only). With the dpdk backend and `nic.lcores` unset, MTL lcores and worker CPUs come from the CPU affinity (Kubernetes cpuset).
- **Environment variables** — platform standard names (canonical) and their aliases:

  | Variable | Setting | Aliases (still valid) |
  |---|---|---|
  | `NMOS_SEED` | `node.seed` | `MXLGW_NODE_SEED` |
  | `NMOS_LABEL` | `node.label` | `MXLGW_NODE_LABEL` |
  | `NMOS_TAGS` | `node.tags` (JSON object) | `MXLGW_NODE_TAGS` |
  | `NMOS_PORT` | `node.http_port` | `MXLGW_NODE_HTTP_PORT`, `MXLGW_HTTP_PORT` |
  | `WEB_PORT` | `node.web_port` | `MXLGW_NODE_WEB_PORT` |
  | `NMOS_HOST_ADDRESS` | `node.host_address` | `MXLGW_NODE_HOST_ADDRESS`, `MXLGW_NODE_PUBLIC_ADDRESS` |
  | `NMOS_REGISTRY_ADDRESS`, `NMOS_REGISTRY_PORT` | `node.registry.address`, `.port` | `MXLGW_NODE_REGISTRY_ADDRESS`, `MXLGW_NODE_REGISTRY_PORT` |
  | `NMOS_QUERY_ADDRESS`, `NMOS_QUERY_PORT` | `node.registry.query_address`, `.query_port` | `MXLGW_NODE_REGISTRY_QUERY_ADDRESS`, `_QUERY_PORT` |
  | `NMOS_DNS_SD` | `node.registry.dns_sd` | `MXLGW_NODE_REGISTRY_DNS_SD` |
  | `SHUTDOWN_TIMEOUT_S` | `node.shutdown_timeout_s` | `MXLGW_NODE_SHUTDOWN_TIMEOUT_S` |
  | `MXL_DOMAIN_SCAN_PATH` | `mxl.scan_path` | `MXLGW_MXL_SCAN_PATH` |
  | `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`, `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS` | first configured domain | `MXLGW_MXL_DOMAIN_<NAME>_PATH`, `_ID`, `_HISTORY_DURATION_NS` |
  | `MXL_CLEANUP_ON_EXIT` | `mxl.cleanup_on_exit` | `MXLGW_MXL_CLEANUP_ON_EXIT` |

  Every other scalar setting is `MXLGW_` + the upper-snake JSON path, e.g. `MXLGW_NODE_ST2110_HTTP_PORT`, `MXLGW_NODE_ST2110_REGISTRY_ADDRESS`, `MXLGW_NIC_LCORE_COUNT` (README "Settings"). The aliases `MXLGW_LOG_LEVEL`, `MXL_READ_OFFSET_GRAINS`, `MXL_READ_OFFSET_MS` remain.
- **New configuration keys:** `node.seed`, `node.tags`, `node.web_port`, `node.host_address`, `node.shutdown_timeout_s`, `node.registry.dns_sd`, `node.registry.query_address`, `node.registry.query_port`, `node.st2110.{enabled,label,http_port,host_address,registry}`, `nic.lcore_count`, `mxl.cleanup_on_exit`. Deprecated but valid: `node.public_address` (alias of `node.host_address`), `node.management_addresses`, `node.registry.mode`.
- **REST API** (every route also under `/api/v1/…`): `/api/status`, `/api/config` with ETag/If-Match, `/api/config/export`, `/api/config/import` (`keep_ids`, `restart`), `/api/config/validate`, `/api/schema`, `/api/groups`, `/api/nic`, `/api/ptp`, `/api/domains`, `/api/flows`, `/api/nmos` (both nodes), `/api/preflight`, `/api/logs`, `/api/restart`; health endpoints `/livez`, `/readyz`, `/statusz`.
- **Exit codes:** 0 (normal, restart requested), 1 (runtime failure), 75 (a port cannot be bound), 78 (configuration or environment error), 130/143 (SIGINT/SIGTERM after the graceful shutdown).
- **Admin web UI** (Vue 3, single embedded file): dashboard, groups (create by counts, edit, duplicate, delete), NMOS (both nodes), network, PTP, MXL (domains, flow browser, receivers), configuration (export/import, changed-on-disk resolution, preflight, logs).
- **Metrics** (`docs/metrics.md`, prefix `mxl_st2110_gateway_`): `build_info`, `ready`, `restart_required`, `ptp_*`, `clock_mtl_minus_host_tai_ns`, `nic_*`, `essence_state`, `rx_*`, `ingest_origin_age_ns`, `mxl_*`, `tx_*`, `egress_lead_ns`, `nmos_registered{node}`, `nmos_activations_total`. PTP series exist only while MTL runs PTP.
- **Monitoring:** generated Grafana 11 dashboard, Prometheus scrape example, Kubernetes `ServiceMonitor`.
- **Logging:** JSON lines (or text), stable event names, MTL/DPDK output redirected through a lock-free ring, last 500 lines in `/api/logs`.
- **Preflight** (`--preflight`, at start and `/api/preflight`): hugepages, vfio, PCI binding, capabilities, MXL domains, scan path, TAI offset, ports, lcores.
- **Tools:** `mxl-pattern-writer` (v210 bars with frame counter, tones, timecode ANC) and `mxl-verify`.
- **Image and deployment:** Dockerfile (webui, deps, build, fuzz, runtime stages; E810 DDP package bundled), runs as uid/gid 1000, labels `org.opencontainers.image.source`/`revision`/`licenses` and `io.dmf.mxl.revision`; Compose files (single host, mxl-fabrics-agent host A/B) and Kubernetes manifests with the standard variables, image `1.0.0`, no DNS-SD mounts.
- **CI:** lint, unit tests in the image build, MTL patch check, web tests, 60 s libFuzzer per parser, integration tests (smoke, platform lifecycle with a mock registry, kernel-backend loopback with leg loss, late flow with a mirror domain, AMWA nmos-testing on both nodes); container publishing to GHCR (`git-<sha7>`, `nightly-dev`, `X.Y.Z`, `X.Y`, `X`, `latest`) and GitHub releases.

### Changed (compared with the pre-release `nightly-dev` / `git-*` builds)

- **Two NMOS nodes:** the RTP Senders/Receivers moved to the ST 2110 node on `NMOS_PORT + 1` (disable with `MXLGW_NODE_ST2110_ENABLED=false`); it has no registry unless `MXLGW_NODE_ST2110_REGISTRY_ADDRESS` (or DNS-SD) is set. Receiver group hints lost the ` Input` suffix.
- **Metrics:** the prefix is `mxl_st2110_gateway_` instead of `mxlgw_`; `nmos_registered` has a `node` label.
- **Registry:** DNS-SD is off unless `NMOS_DNS_SD=true` (or the deprecated `registry.mode: "dns-sd"` in the file); the registration port defaults to 3210; `mode: "static"` still requires `address`. The minimal configuration written on first start no longer contains `registry.mode`. D-Bus/Avahi mounts are only needed with DNS-SD.
- **Readiness:** registration is required only for a node whose registry is configured; new reasons `st2110_nmos_not_registered`, `shutting_down`.
- **Addresses:** hrefs and `api.endpoints` carry the host address only (before: every interface address when `management_addresses` was empty); `node.public_address`/`MXLGW_NODE_PUBLIC_ADDRESS` and `management_addresses` must be announceable IPv4 literals (a host name now exits 78).
- **Environment:** two variables of one setting with different values exit 78 (before, the canonical name won silently).
- **Shutdown:** SIGTERM exits 143 (SIGINT 130) instead of 0, after deregistering; a port that cannot be bound exits 75 instead of 1 (the preflight only warns about a port in use).
- **MXL domains:** a `domain_def.json` id mismatch is logged as an error (was a warning); the gateway sets `umask 002`.
- **Image:** runs as uid/gid 1000; DPDK deployments run the container as `0:1000`.

### Fixed

- MTL patch 0004: lost wakeup in MTL's scheduler sleep (1 s stalls under load).
- Kernel backend: MTL's scheduler runs as a sleeping thread instead of a pinned busy-polling lcore, so the CI loopback works on small runners; audio RX keeps 100 ms of frame buffers.
- nmos-cpp swallows listener errors: a port in use no longer looks open (exit 75).
- Integration tests: Avahi start on systemd hosts, promtool image pull output no longer counted as lint findings.

### Platform notes

- Run the container as root (uid 0, gid 1000) with the dpdk backend; see README "Users and permissions".
- Reserve `NMOS_PORT` and `NMOS_PORT + 1` per gateway; expose `NMOS_PORT + 1` to the facility's ST 2110 controller.
- Point the ST 2110 node at the facility's ST 2110 registry with `MXLGW_NODE_ST2110_REGISTRY_ADDRESS`/`_PORT` if it should register.
