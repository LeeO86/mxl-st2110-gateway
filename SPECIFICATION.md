# mxl-st2110-gateway — Technical Specification

| | |
|---|---|
| Status | 1.3 — the v1.0.0 contract: Draft 1.2 plus the MXL PoC platform guideline G1–G14 (see Changelog) |
| Date | 2026-10-03 |
| Repository | `mxl-st2110-gateway` (new, empty repository) |
| Sibling project | [`LeeO86/mxl-decklink`](https://github.com/LeeO86/mxl-decklink) — reuse its conventions (layout, CI, web UI stack, health/metrics, NMOS integration, `mxlbridge/` module shapes) wherever this document does not say otherwise |
| Related project | [`LeeO86/mxl-fabrics-agent`](https://github.com/LeeO86/mxl-fabrics-agent) — per-host container that replicates MXL flows between hosts via the MXL 1.1 Fabrics API; its `SPECIFICATION.md` (commit `3b981c6`, Draft v0.1, §7 mirror domains, §11 requirements on media functions) is normative for §8.5–§8.6 |
| Companion documents | `IMPLEMENTATION_PLAN.md` (phases + acceptance criteria), `AGENTS.md` (agent instructions) |

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Anything marked **VERIFY** is a fact that was checked against the pinned sources on 2026-10-01 but that the implementer MUST re-confirm in code before relying on it (write a unit test or a source comment citing file and line).

## Changelog

### 1.3 (2026-10-03) — MXL PoC platform guideline G1–G14, the v1.0.0 contract

Rationale and dates in `docs/decisions.md`; audit in `IMPLEMENTATION_PLAN.md` ("Platform guideline G1–G14").

- §3.1, §7.1, §7.2: **two NMOS nodes** in one process — the MXL node (MXL Senders/Receivers, `node.http_port` = `NMOS_PORT`, registers with the MXL registry) and the ST 2110 node (RTP Senders/Receivers, `node.st2110.http_port`, default `NMOS_PORT + 1`, own optional registry); one device per node; group hints `<group>:<Role> <n>` for Senders and Receivers again.
- §7.1: hrefs and `api.endpoints` carry IPv4 literals only (`node.host_address` = `NMOS_HOST_ADDRESS`, default the default-route interface's address); DNS-SD is off unless `node.registry.dns_sd`; static registry `node.registry.address`/`port` (default port 3210), query address/port reported; node and device tags (`node.tags` = `NMOS_TAGS`).
- §7.3: `node.seed` (`NMOS_SEED`) derives every NMOS id and the default domain ids.
- §8.2–§8.4: `MXL_OUTPUT_DOMAIN_DIR`/`_ID`/`_HISTORY_DURATION_NS` configure the first configured domain; a `domain_def.json` id mismatch is an error (file kept); `mxl.cleanup_on_exit` removes the own domains on SIGTERM.
- §9.1: the platform's standard environment names are canonical, the `MXLGW_*` names aliases; two variables of one setting with different values exit 78.
- §9.4, §11.3: every `/api` route also under `/api/v1`; import `?restart=true`.
- §10: `/readyz` requires registration only with a configured registry, per node; `node.web_port` (`WEB_PORT`) optionally moves the gateway routes to their own listener.
- §12.1: metric prefix `mxl_st2110_gateway_` (was `mxlgw_`); `nmos_registered` has a `node` label.
- §14: image user uid/gid 1000 (DPDK deployments as `0:1000`), `umask 002`, labels `org.opencontainers.image.source`/`revision`/`licenses` and `io.dmf.mxl.revision`; exit codes 0/1/75/78/130/143; shutdown sequence bounded by `node.shutdown_timeout_s` (`SHUTDOWN_TIMEOUT_S`).
- §4, §9.5: `nic.lcores`/`app_cpus` unset → derived from the CPU affinity (dpdk backend, `nic.lcore_count`).
- §15: deployments use the standard variables, no DNS-SD mounts, image `1.0.0`.
- §17.3: `tests/integration/lifecycle.sh` (mock registry, SIGTERM, cleanup); nmos-testing against both nodes.

### Draft 1.2 (2026-10-01) — owner decisions, verification against the pinned sources

Owner decisions (rationale and dates in `docs/decisions.md`):

- C1: `node.http_port` stays `8080`; co-location is solved at deployment level (host-networking port override, Compose port mapping or a reverse proxy). New optional `node.public_address` / `node.public_port` for advertising a proxy (§7.1, §9.5, §15.4).
- C2: configuration hierarchy **environment > config file > default** for every scalar setting, as in mxl-decklink; env-set keys are read-only in the UI (§9.1, §9.3, §11.2).
- C3: an MXL Receiver accepts an unknown `mxl_domain_id`, logs `mxl_domain_unknown` and waits for the domain (§7.4, §8.5; deviation from BCP-007-03 recorded).
- C4: missing flows are retried with backoff 500 ms → 5 s (§5.8).
- C5 / research Q8: the Flow id is derived from the essence `uid` **and** the canonical format, so a format change mints a new flow UUID (§7.3).
- C6: an adopted `domain_def.json` id is always written back to the config file (§8.3).
- C7: examples use the host MXL root `/Volumes/mxl` like the sibling projects; each container maps it to its own path, identity comes from `domain_def.json` (§8.2, §14.2, §15).
- Q1: egress RTP timestamps are the transmit time `T(i) + output_delay` for every essence (lip-sync kept by the common delay); may be revisited (§5.4, §5.7).
- Q2: `output_delay_ns` default two grains; minimum one grain + largest read offset + 2 ms, maximum largest read offset + 5 grains for groups with video (§5.7, §9.5).
- Q3: missing egress data → black video, silent audio, empty ANC by default; per group `missing_data: "repeat"` repeats the last good video grain (§5.7).
- Q4: 1280×720 dropped from v1 (MTL's v210 converters cannot produce MXL's padded v210 for 1280 px); primary format 1080p50 (§6.1).
- Q5: PTP patches 0002 (domain filter) and 0003 (PTP on both 2022-7 ports with BMCA across them, parent re-selection); `ptp.port` removed (§4.3, §5.5).
- Q6: port MAC, link state/speed via the DPDK ethdev API, DDP version from the PMD log (§4.5, §12.1).
- Q7: `groups[].enabled` semantics and first-start activation from `defaults` (§7.6).
- Q9: start-up garbage collection only removes the gateway's own stale flows; domain-wide GC is opt-in per domain (`gc_on_start`) (§8.3).
- Q10: ANC of interlaced formats at field rate; MTL's 20-packet / 8-bit-UDW limits accepted (§6.3).
- Q13: IS-04-01 in CI uses nmos-testing's mock registry over multicast DNS-SD; no registry container (§7.7, §17.3).
- Q14 (no owner answer; conservative choice): an SDP whose format does not match → 400 from the staging validator; a malformed SDP → 500 as nmos-cpp does (§6.4).

Corrections from the verification against the pinned sources:

- MXL v1.1.0 marks a complete grain with `validSlices == totalSlices` (not `committedSize`); `mxlFlowWriterOpenSamples` / `mxlFlowReaderGetSamples` take the **end** index; the discrete writer rejects any index ≤ the last committed one (§5.6, §6.1–§6.3).
- MTL: `query_ext_frame` runs on the lcore when a frame is complete and works with conversion; `st30p` TX has no user-timestamp flag (its RTP timestamp follows the pacing time); no public MAC/link API; built-in PTP locks onto the first Announce and has no domain filter (§5.4, §5.5, §6.1).
- nmos-cpp: negative ports are skipped when listeners open (`server.cpp`), Settings/Logging default to `http_port`, the static registry setting is `registry_version`, routes mounted after `make_node_server` need the catch-all handler moved (§7.1).
- Egress uses one worker thread per group (MXL sync groups are not thread-safe) and audio blocks of `block_us` independent of the video cadence (§3.6, §5.7).
- DPDK is built with `-Dplatform=generic` (§14.1).

### Draft 1.1 (2026-10-01) — interoperability with mxl-fabrics-agent

Changes:

- §1.1, §1.2 (goal 9), §1.3: the gateway cooperates with mxl-fabrics-agent; host-to-host replication is delegated to the agent, the gateway itself still has no Fabrics code.
- §2: MXL pin shared with mxl-decklink; all MXL users on one host use the same v1.1 line; mxl-fabrics-agent specification referenced.
- §3.2, §3.3, §3.5, §3.6: new module `mxlbridge/domainscan`, configured/discovered/mirror domains, domain resolution step in the egress data flow, scanning off the media threads.
- §5.1: every host that writes, replicates or reads a flow MUST be TAI-disciplined.
- §5.7: per-receiver read offset; output when no data arrives in time. New §5.8: reader bring-up and resilience (retry with backoff on `MXL_ERR_FLOW_NOT_FOUND`, `no_signal` is not an error, flow re-creation).
- §6.4: flow format check repeated whenever an egress flow appears or is re-created.
- §7.2, §7.3: Flow id = MXL flow id rationale; stable domain ids; behaviour when a new flow UUID is minted (mxl-decklink sequence).
- §7.1, §10: nmos-cpp Settings/Logging APIs and Events WebSocket disabled by default (no extra host ports); `/readyz` considers configured domains only, and `waiting_for_flow` / `no_signal` do not affect readiness.
- §7.4: MXL Receivers resolve `mxl_domain_id` over configured and discovered domains (including mirror domains), re-scan before resolving or rejecting, no negative caching; MXL Senders write only into their configured domain.
- §8.1 (domain identity, mirror domains and their marker, MXL reader status codes), §8.2 (`mxl.scan_path`), §8.3 (mirror write protection, id write-back, GC only on configured domains), §8.4 (re-opened flows are compared with the descriptor), new §8.5 (domain discovery), new §8.6 (host-to-host replication with mxl-fabrics-agent, mapping of the agent's §11 requirements).
- §9.3, §9.5: `mxl.scan_path`, `read_offset_grains` / `read_offset_ns`, new semantic rules, port-collision note on `http_port`.
- §11.2, §11.3: UI and REST API show discovered/mirror domains, the resolved domain per MXL Receiver and the new states.
- §12.1, §12.2: new essence state `no_signal`; `mxl_st2110_gateway_mxl_read_timeouts_total` defined as "no data by the deadline"; new MXL reader metrics (flow-not-found retries, read lag, late and invalid reads, resolved domain info, discovered domains) and matching Grafana panels.
- §13: new log events.
- §14.1, §14.2, §14.3: Fabrics OFF and same MXL pin as mxl-decklink; MXL root mount; preflight checks for mirror domains, the scan path, a zero kernel TAI offset and port collisions.
- §15.1, §15.2, §15.3, new §15.4: MXL root mount (instead of a single-domain mount) in Compose/Kubernetes, optional multi-host scenario with mxl-fabrics-agent, TAI on all hosts, port table.
- §16.1, §17.1, §17.3: late-flow integration test, discovery/resilience unit tests.
- §18, §19 (items 13, 14), §20 (R11–R14), §21, §22: robustness, acceptance, risks, later stage, glossary.

Conflicts with existing decisions raised in Draft 1.1 (all resolved in Draft 1.2, see above):

- **C1 — default HTTP port.** `node.http_port` defaults to `8080` (§3.1, §9.5, §10, §15.1), which collides with mxl-decklink (`8080`) under host networking. Kept `8080`; proposed new default `8090` (free in the §15.4 port table).
- **C2 — environment variables.** The requested `MXL_DOMAIN_SCAN_PATH` and `MXL_READ_OFFSET_GRAINS` / `_MS` conflict with §9.1 (environment variables are bootstrap-only, everything else lives in the config file). Kept §9.1; added config keys `mxl.scan_path` and per-receiver `read_offset_grains` / `read_offset_ns` (nanoseconds instead of milliseconds, following the spec's `_ns` convention). Open: should environment overrides exist as well?
- **C3 — unknown `mxl_domain_id` on MXL Receivers.** §7.4 rejects a domain id that is not accessible with HTTP 400 at staging (BCP-007-03 "MUST reject … an MXL Domain the Node is not capable of accessing"). With mxl-fabrics-agent `MIRROR_MODE=on-demand` the mirror domain is only created after a receiver is activated with that id, so such activations would be rejected. Kept 400 after a fresh re-scan; this works with the agent's default `MIRROR_MODE=eager`. Open: accept any domain UUID and wait for the domain instead?
- **C4 — flow polling interval.** §6.4 polled for missing flows every 500 ms; the new requirement asks for exponential backoff. Kept 500 ms as the first retry interval; backoff doubles up to 5 s (§5.8). Open: confirm the 5 s cap, or keep a fixed 500 ms poll?
- **C5 — new flow UUID on format change.** §7.3 derives the Flow id from the essence `uid` only, so a format edit keeps the Flow id and no new UUID is minted. Kept §7.3; §7.3 now specifies the update sequence that applies whenever a new UUID is minted. Open: mint a new flow UUID on format changes (needed for clean mirroring by id)?
- **C6 — domain id mismatch.** §8.3: if the config `id` differs from an existing `domain_def.json`, the file wins and the config is left unchanged. After a tmpfs wipe the config id would then be used, so remote mirrors keyed by the previous id would break. Kept §8.3. Open: write the adopted id back to the config in that case too?
- **C7 — host MXL root path.** This spec's examples use the host tmpfs `/run/mxl`; mxl-decklink and mxl-fabrics-agent use `/Volumes/mxl`. Containers on one host MUST share the same root. Kept `/run/mxl`. Open: align the examples to `/Volumes/mxl`?

---

## 1. Overview, Goals, Non-Goals

### 1.1 Purpose

`mxl-st2110-gateway` is a single container that bridges **SMPTE ST 2110** networks and a host-local **MXL** (Media eXchange Layer) domain, in **both directions**, for **video, audio and ancillary data**:

- **Ingest** — ST 2110-20 / -30 / -40 multicast → MXL flows (`video/v210`, `audio/float32`, `video/smpte291`).
- **Egress** — MXL flows → ST 2110-20 / -30 / -40 multicast.

It is controlled exclusively through **AMWA NMOS IS-04 / IS-05** (including **BCP-007-03** for the MXL side) using **Sony nmos-cpp**, and configured through an **admin web UI** whose state lives in a **mounted configuration file**.

The data path uses **Intel Media Transport Library (MTL)** on **DPDK** with Intel **E810** NICs to achieve ST 2110-21 compliant pacing (narrow / narrow-linear) and hardware PTP timestamping. **GStreamer is explicitly not used.**

On its host the gateway is an ordinary MXL media function. Making its MXL flows available on other hosts (and flows of other hosts available to it) is the job of **mxl-fabrics-agent**, a separate per-host container that replicates flows with the MXL 1.1 Fabrics API into *mirror domains* (§8.6). The gateway cooperates with the agent only through standard NMOS (IS-04/IS-05, BCP-007-03) and the MXL domain conventions of §8.5; it contains no Fabrics code.

### 1.2 Goals

1. Spec-correct MXL: v210 grains contain real v210 (never RFC 4175 wire data), audio is de-interleaved float32 in a continuous flow, ANC follows the MXL RFC 8331 grain layout.
2. ST 2110-21 narrow pacing on transmit; ST 2022-7 Class A redundancy (two ports on the same NIC) on receive and transmit.
3. Lip-sync preserving: MXL grain/sample indices derived from RTP origination timestamps on ingest; RTP timestamps derived from MXL indices on egress.
4. Pass the official AMWA NMOS Testing Tool suites IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 with no failures.
5. Interoperate with Sony nmos-cpp registries and controllers (the operator's existing NMOS controller).
6. One container = one NIC, bidirectional, with two NMOS Nodes: the MXL side and the ST 2110 side (§7.1).
7. Container and Kubernetes deployment are both first-class (documented and shipped).
8. Observability via Prometheus `/metrics` plus a generated Grafana dashboard.
9. Works with mxl-fabrics-agent without code coupling: ingest flows can be mirrored to other hosts by domain id and flow id, and egress MXL Receivers find flows replicated from other hosts in mirror domains (§8.5, §8.6).

### 1.3 Non-Goals (this version)

- ST 2110-22 (JPEG XS / compressed video), ST 2022-6, AES67-only profiles beyond what ST 2110-30 covers.
- MXL Fabrics / RDMA inside the gateway process. Host-to-host replication is delegated to mxl-fabrics-agent (§8.6). Build MXL with `-DMXL_ENABLE_FABRICS_OFI=OFF` (§2, §14.1).
- Format conversion (scaling, frame-rate conversion, audio resampling, channel shuffling beyond 1:1). Formats are fixed per essence in the configuration.
- Sample-rate conversion or drift correction between an unlocked source and the host clock (see §5.6).
- More than one NIC (one port pair) per container — see §4.4 and §21 "Later stage".
- IS-07, IS-08, IS-12 / MS-05. They MUST be disabled in nmos-cpp settings.
- Authentication on the web UI (protected operations network assumed; same stance as mxl-decklink).

---

## 2. Pinned Dependencies and References

All versions are pinned in exactly one place each (`docker/Dockerfile` build args, mirrored in `.github/workflows/ci.yaml` with a "keep in sync" comment — the mxl-decklink pattern).

| Dependency | Pin | Notes |
|---|---|---|
| Intel MTL | tag **`v26.09`** (`VERSION` = `26.09.0.REL`) | `github.com/OpenVisualCloud/Media-Transport-Library`, BSD-3-Clause |
| DPDK | **26.07** + MTL patches (`versions.env`: `DPDK_VER=26.07`, `DPDK_MTL_MINOR_VER=90`) | built via MTL `script/build_dpdk.sh`; never use distro DPDK |
| MXL SDK | tag **`v1.1.0`** | `github.com/dmf-mxl/mxl`, Apache-2.0, Fabrics OFF (`-DMXL_ENABLE_FABRICS_OFI=OFF`), built with vcpkg exactly as in mxl-decklink `docker/Dockerfile`. MUST be the same pin as mxl-decklink's `MXL_REF`; bump both together. All MXL users sharing a host's MXL root (gateway, mxl-decklink, mxl-fabrics-agent, which builds its own MXL with Fabrics ON) MUST use the same v1.1 line so the shared-memory flow format is compatible — check the agent's pin when bumping (R12) |
| Sony nmos-cpp | commit **`fe303849527394b03bdedc8f161f377fe458bb62`** | = `master` on 2026-10-01 and = mxl-decklink pin; contains `urn:x-nmos:transport:mxl`, `make_connection_mxl_sender/receiver`, MXL `auto` resolution |
| AMWA nmos-testing | commit `90018513758e6fbc096c26d7ef001f700b28b510` (master 2026-10-01) | contains `BCP0070301Test.py` / `BCP0070302Test.py` |
| Host `ice` kernel driver | 2.6.7 (MTL `versions.env`), with MTL `patches/ice_drv/` | only needed on the host if VFs are used (§4.2) |
| E810 DDP package | ≥ 1.3.35.0 (MTL `doc/e800_series_drivers.md`) | MUST be present for the DPDK ice PMD (§14.4) |
| Base image | `ubuntu:24.04` | MTL validates on Ubuntu 24.04 |

Normative references (pin the versions in `docs/references.md`):

- AMWA BCP-007-03 v1.0.0 (tag `v1.0.0`, commit `76763d56`) — `NMOS-With-MXL.md`, schemas `mxl_domain_definition.json`, `sender_transport_params_mxl.json`, `receiver_transport_params_mxl.json`, `constraints-schema-mxl.json`.
- AMWA IS-04 v1.3, IS-05 v1.1 + v1.2, BCP-002-01 (grouping), BCP-004-01 (receiver capabilities).
- SMPTE ST 2110-10/-20/-21/-30/-40, ST 2022-7, ST 2059-1/-2, RFC 4175, RFC 8331, RFC 7273.
- MXL docs (`docs/Architecture.md`, `docs/Timing.md`, `docs/Configuration.md`, `docs/Tools.md`) at `v1.1.0`.
- MTL docs at `v26.09`: `doc/design.md` (§5.4 PTP), `doc/external_frame.md`, `doc/run.md`, `doc/e800_series_drivers.md`, `doc/kernel_socket.md`, `docker/README.md`.
- mxl-fabrics-agent `SPECIFICATION.md` at commit `3b981c6` (Draft v0.1): §5 domain scanning and classification, §7 mirror domains and mirror flows, §9 timing, §10 port defaults, §11 requirements on media functions.

Reference implementations (read, do not copy blindly):

- MTL `ecosystem/MTL_with_MXL/poc/src/sender/mxl_bridge.c` — FlowWriter setup, grain slot management, `query_ext_frame` pattern. **Known defect: it DMAs RFC 4175 pgroup data (`ST20_FMT_YUV_422_10BIT`) straight into grains declared as `video/v210` without conversion. Do not replicate.**
- mxl-decklink `src/mxlbridge/` (`videowriter`, `audiowriter`, `ancwriter`, `videoreader`, `audioreader`, `flowsync`, `domain`, `domainscan`, `flowdef`), `src/util/` (`audioconv`, `anc`, `taiclock`, `uuid`, `v210`), `src/nmos/node.cpp` (incl. `onRuntimeFlows`: Flow/Sender/`mxl_flow_id` update when a new flow UUID is minted), `src/ops/` (`httpserver`, `metrics`, `health`, `webapi`).
- nmos-cpp `Development/nmos-cpp-node/node_implementation.cpp` — MXL sender/receiver construction (~l.990–1070) and MXL `auto` resolution (~l.2125).

---

## 3. System Architecture

### 3.1 Process and Container Model

- One process (`mxl-st2110-gateway`), one container, one MTL instance, one NIC, two NMOS Nodes (MXL node and ST 2110 node, §7.1).
- The process hosts: the MTL instance (DPDK EAL, lcores, built-in PTP), the MXL instance(s) (one `mxlInstance` per configured domain), two nmos-cpp node servers, and the HTTP routes for `/admin`, `/api`, `/metrics`, `/livez`, `/readyz`, `/statusz`.
- The gateway routes share the MXL node's TCP port (`node.http_port` = `NMOS_PORT`, default `8080`) unless `node.web_port` (`WEB_PORT`) gives them their own; the ST 2110 node listens on `node.st2110.http_port` (default `NMOS_PORT + 1`), see §10.

### 3.2 Repository Layout (target)

```
mxl-st2110-gateway/
├── AGENTS.md  SPECIFICATION.md  IMPLEMENTATION_PLAN.md  README.md  LICENSE  CHANGELOG.md
├── CMakeLists.txt  cmake/
├── src/
│   ├── main.cpp
│   ├── config/        schema.{hpp,cpp} store.{hpp,cpp} config.{hpp,cpp} formats.{hpp,cpp}
│   ├── mtl/           instance.* (EAL/ports/PTP) video_rx.* video_tx.* audio_rx.* audio_tx.* anc_rx.* anc_tx.* sdp_map.*
│   ├── mxlbridge/     domain.* domainscan.* flowdef.* videowriter.* videoreader.* audiowriter.* audioreader.* ancwriter.* ancreader.* flowsync.*
│   ├── timing/        ptp.* rtpclock.* (RTP⇄TAI⇄MXL index, pure functions)
│   ├── codec/         audioconv.* (L16/L24 ⇄ float32) anc8331.* (st40 meta ⇄ RFC 8331)
│   ├── group/         group.* ingest_essence.* egress_essence.* group_manager.*
│   ├── nmos/          node.* resources.* activation.* caps.* ids.*
│   ├── ops/           httpserver.* webapi.* metrics.* health.* logging.*
│   └── util/
├── web/               Vue 3 + Vite, single-file build embedded in the binary (mxl-decklink pattern)
├── patches/mtl/       0001-ptp-status-api.patch 0002-ptp-domain-filter.patch 0003-ptp-dual-port-bmca.patch (§5.5)
├── tools/             mxl-pattern-writer/ mxl-verify/ gen_config_docs.py (§17.3, §18)
├── schema/            gateway-config.schema.json (JSON Schema draft 2020-12)
├── config/examples/   gateway.example.json  gateway.minimal.json  gateway.fabrics-host-a.json  gateway.fabrics-host-b.json
├── docker/            Dockerfile entrypoint.sh docker-compose.yaml docker-compose.fabrics.yaml
├── deploy/k8s/        namespace.yaml sriov-dp-configmap.yaml deployment.yaml service.yaml configmap.yaml kustomization.yaml README.md fabrics/
├── monitoring/        grafana/mxl-st2110-gateway.json prometheus/scrape-example.yaml tools/gen_dashboard.py
├── tests/unit/        doctest
├── tests/integration/ smoke.sh loopback.sh late-flow.sh nmos-testing.sh
└── .github/workflows/ ci.yaml container.yaml release.yaml
```

Language: **C++20**, CMake + Ninja, `-Wall -Wextra -Werror` in CI, unit tests with **doctest** (as mxl-decklink). MTL and DPDK are C libraries; wrap their handles in RAII types.

### 3.3 Core Concepts

- **Group** — the user-facing unit (e.g. "CAM 1"). A group has a **direction** (`ingest` = 2110→MXL, or `egress` = MXL→2110), a target MXL domain, a redundancy flag and **N video + M audio + K ANC essences**. A group maps to one BCP-002-01 group hint.
- **Essence** — one stream of one type within a group. Each essence owns exactly one MTL session and one MXL writer or reader.
- **Ingest essence** = IS-04 *Receiver* (`rtp.mcast`) + IS-04 *Source/Flow/Sender* (`mxl`) + MTL RX session + MXL FlowWriter.
- **Egress essence** = IS-04 *Receiver* (`mxl`) + IS-04 *Source/Flow/Sender* (`rtp.mcast`) + MXL FlowReader + MTL TX session.
- **Configured domain** — an MXL domain listed in `mxl.domains[]` (§8.2). The only kind of domain the gateway writes to; groups reference it by name.
- **Discovered domain** — any MXL domain found by scanning `mxl.scan_path` (§8.5). Readable by egress MXL Receivers, never written. A **mirror domain** is a discovered domain created by mxl-fabrics-agent that carries the identity of a domain on another host (§8.6).

### 3.4 Data Flow — Ingest (2110 → MXL)

```
E810 port P (+R) ──DPDK PMD──► MTL st20p/st30p/st40p RX (2022-7 merge, PTP-stamped)
     ──► frame callback / get_frame (lcore or app thread)
     ──► timing: RTP ts → TAI ns → MXL index        (§5.4)
     ──► codec: v210 conversion by MTL into the grain (video, ext_frame)
               L16/L24 BE interleaved → float32 per channel (audio)
               st40 meta+UDW → RFC 8331 grain body (ANC)
     ──► mxlFlowWriterOpenGrain / CommitGrain  (video, ANC)
         mxlFlowWriterOpenSamples / CommitSamples (audio)
```

### 3.5 Data Flow — Egress (MXL → 2110)

```
IS-05 mxl_domain_id ──► domain resolution over configured + discovered domains (§8.5)
     ──► mxlFlowReader (per essence, retried while the flow is missing  (§5.8)),
         all readers of a group in one mxlFlowSynchronizationGroup
     ──► wait for index i (group-aligned, behind head by the read offset) (§5.7)
     ──► codec: v210 grain used as st20p ext_frame (MTL converts to RFC 4175)
               float32 per channel → L16/L24 BE interleaved
               RFC 8331 grain body → st40 meta+UDW
     ──► MTL TX with USER_PACING at T_tx = TAI(i) + output_delay; RTP timestamp derived from T_tx (§5.4)
     ──► ST 2110-21 narrow pacing on port P (+R duplicate)
```

### 3.6 Threading Model

- MTL owns its lcores (polling, pacing, PTP). Lcore list is configurable (`nic.lcores`) and MUST be disjoint from application threads.
- Ingest: per essence, one application worker thread (blocking `*_get_frame` with timeout). Egress: per **group**, one worker thread that owns the group's `mxlFlowSynchronizationGroup` (MXL sync groups reorder their reader list on every wait and are not thread-safe — `lib/internal/src/FlowSynchronizationGroup.cpp`), reads all essences and hands frames to their MTL TX sessions. Callbacks executed on MTL lcores (`notify_frame_available`, `query_ext_frame` etc.) MUST NOT block, allocate, log synchronously or take contended locks — they only signal the worker. `mxlFlowWriterOpenGrain` is lock- and allocation-free and MAY be called from `query_ext_frame` (§6.1).
- One control thread processes NMOS activations and admin changes through a single serialized queue (mxl-decklink `enqueue(readActivation(...))` pattern). Pipeline (re)configuration never runs on an HTTP handler thread.
- Domain discovery (§8.5) runs on the control thread or a housekeeping thread, never on media worker threads or MTL lcores. Workers receive resolved domain paths; reader (re)creation and sync-group changes run on the thread that owns the egress group's `mxlFlowSynchronizationGroup` (§5.7, §5.8).
- Real-time threads MAY use `SCHED_FIFO` when `CAP_SYS_NICE` is available; failure to raise priority is a warning, not an error.

### 3.7 Latency Budget (targets, 1080p50)

| Stage | Target |
|---|---|
| Ingest video, last packet → grain committed | ≤ 1 frame + 2 ms (full-frame conversion) |
| Ingest audio, packet → samples committed | ≤ 1 audio block (default 1 ms) + 1 ms |
| Egress, grain origin time → first packet on wire | `output_delay` (default 2 grains, minimum 1 grain + largest read offset + 2 ms, configurable per group) |

Sub-frame (slice) latency is a later-stage feature (§21).

---

## 4. NIC and Network Model

### 4.1 Ownership

DPDK takes the media ports away from the kernel (`vfio-pci`). The ports therefore have **no kernel IP configuration**; source IP, netmask, gateway and (optionally) MAC override MUST be configured in the gateway (§9) and are handed to MTL (`mtl_init_params.sip_addr`, `netmask`, `gateway`, `port[]`). NMOS control traffic (IS-04/IS-05, admin UI, metrics) uses a **separate kernel-owned management interface**.

### 4.2 Port Binding Modes

| Mode | Binding | PTP | Notes |
|---|---|---|---|
| **PF (default, recommended)** | whole E810 port function bound to `vfio-pci` | MTL built-in PTP with HW timestamps, ≈ 30 ns (MTL `doc/design.md` §5.4.1) | DPDK ice PMD drives rate limiting / pacing |
| VF (supported, degraded) | SR-IOV VF bound to `vfio-pci`, PF stays on kernel `ice` | **no HW timesync on VF** — TSC-based, ≈ 1 µs (MTL doc §5.4.1) | host `ice` driver MUST carry MTL `patches/ice_drv` rate-limit patches |

The mode is detected from the PCI device (PF vs VF) at startup, logged, exposed in `/api/status` and as metric label `bind_mode`.

TX pacing (`nic.tx_pacing`, `mtl_init_params.pacing`): `auto` (default) lets MTL try the NIC rate limiter and fall back to TSC pacing; `rl` requires the rate limiter; `tsc` uses TSC pacing and never touches the rate limiter. On an E810 PF the rate limiter test restarts the port. When that restart fails (ice `Failed to add lan txq`, seen on E810-XXV with NVM 5.01 and 8 TX queues), MTL falls back to TSC pacing, but in one of two runs on those hosts `mtl_init` still failed; `tsc` skips the rate limiter test.

### 4.3 Port Pair and ST 2022-7

- The network is modelled as a **list of port pairs** (`nic.port_pairs[]`). Each pair has a `primary` port and an optional `redundant` port, each with its own PCI address and IP configuration.
- **This version limits the list to exactly one pair** (validation error otherwise). The data model is a list from day one so that a later version can lift the limit without a schema break (§21).
- ST 2022-7 is realised with **both ports of the same E810** (2×25G or 2×100G cards). Both legs share one PHC — no inter-leg clock offset. The NIC itself is not a redundant element; this protects against network failures, not NIC failure. This MUST be stated in the README.
- When a redundant port exists, PTP runs on **both** ports. A BMCA across the Announce messages received on both ports selects the port (and parent) that steers the PHC; the other port listens passively and takes over when the selected parent fails (patch 0003, §5.5). Pulling one leg therefore loses neither media nor PTP.
- Redundancy is enabled **per group** (`groups[].redundancy`), never per essence. A redundant group requires a `redundant` port in the pair; validation error otherwise.
- MTL maps: `MTL_SESSION_PORT_P` = primary, `MTL_SESSION_PORT_R` = redundant (`num_port = 2` in session ops).

### 4.4 Cards with More Ports (e.g. E810 4×25G)

The gateway binds **only** the PCI functions listed in `nic.port_pairs[0]`. Other functions of the same card stay with the kernel and remain usable for management or a host PTP client (§5.2 `external`). No error, no warning beyond an info log listing ignored sibling functions of the same card.

### 4.5 NMOS Interfaces

nmos-cpp normally derives the Node's `interfaces` from kernel network interfaces, which do not include DPDK ports. The gateway MUST build the IS-04 Node `interfaces` array itself:

- one entry per bound port, `name` = configured port name (e.g. `media-p`, `media-r`), `port_id` = port MAC, `chassis_id` = MAC of the card's first function (or `null` if unknown). MTL v26.09 has no public MAC or link API, so the gateway queries DPDK directly in the same process: `rte_eth_dev_get_port_by_name(<pci>)`, `rte_eth_macaddr_get`, `rte_eth_link_get_nowait` (read-only; MTL owns the port). Kernel backend: MAC and link from the kernel interface;
- the management interface is listed as well (from the kernel) so `href`/`api.endpoints` stay correct;
- every `rtp.mcast` Sender/Receiver has `interface_bindings` = `[primary]` or `[primary, redundant]`;
- every `mxl` Sender/Receiver has `interface_bindings = []` (BCP-007-03).

VERIFIED (nmos-cpp `fe30384` `node_resource.cpp`): `nmos::make_node(id, clocks, interfaces, settings)` stores the caller's `interfaces` JSON as given, so the gateway passes its own array; `make_sender`/`make_receiver` do not check `interface_bindings` against it, but SDP generation (`make_ts_refclk`) does, so the names MUST match.

---

## 5. Timing and PTP

This is the most critical part of the design. Implement it as pure, unit-tested functions in `src/timing/` before any pipeline code.

### 5.1 Clocks Involved

| Clock | Used by | Source |
|---|---|---|
| MTL PTP time (TAI ns) | MTL RX timestamps, TX pacing, RTP derivation | E810 PHC disciplined by MTL built-in PTP (or user callback) |
| Host `CLOCK_TAI` | **MXL**: `mxlGetTime()`, `mxlGetCurrentIndex()`, reader head expectations (MXL `lib/src/time.cpp` uses `Clock::TAI`) | kernel; `CLOCK_TAI = CLOCK_REALTIME + kernel TAI offset` |

MXL readers on the same host (other containers) compute "now" from **their** `CLOCK_TAI`. If the gateway writes grains at indices derived from MTL PTP time while the host `CLOCK_TAI` disagrees, other media functions see grains as too early/too late. The two clocks MUST agree to well within one grain.

The same holds across hosts. mxl-fabrics-agent replicates grains and samples 1:1 at the same indices (mxl-fabrics-agent §9), so **every host that writes, replicates or reads a flow MUST be TAI-disciplined** to the same time scale — PTP (`ptp4l` + `phc2sys`) or chrony with a PHC/PTP reference — **with the correct kernel TAI offset** (37 s). A host whose `CLOCK_TAI` is off reads the wrong grains, or none. This applies to hosts that run only an MXL Receiver of the gateway as well.

### 5.2 PTP Modes (`ptp.mode`)

| Mode | MTL flags | Host clock handling | Use when |
|---|---|---|---|
| `builtin` (default) | `MTL_FLAG_PTP_ENABLE` (+ `MTL_FLAG_PTP_PI` on PF) | **untouched**. The host MUST discipline `CLOCK_TAI` to the same grandmaster by other means (e.g. `ptp4l`+`phc2sys` on a kernel-owned port, chrony with a PHC refclock) **with a correct kernel TAI offset** (37 s) | normal hosts, shared hosts, Kubernetes |
| `builtin_phc2sys` | `MTL_FLAG_PTP_ENABLE` + `MTL_FLAG_PHC2SYS_ENABLE` (needs `CAP_SYS_TIME`) | MTL steers **`CLOCK_REALTIME`** to the PHC **without subtracting the UTC offset** (MTL `lib/src/mt_ptp.c` `phc2sys_adjust` / `ptp_adj_system_clock_time` use `CLOCK_REALTIME`). Consequence: `CLOCK_REALTIME` shows TAI (wall clock 37 s ahead of UTC). Therefore the kernel TAI offset MUST be 0 so that `CLOCK_TAI == PTP`. The gateway MUST check `adjtimex()` `tai` at startup and refuse to start in this mode if it is non-zero. NTP/chrony on the host MUST be disabled (MTL `doc/run.md` §8.12). | dedicated single-purpose appliances only; document the side effects prominently |
| `external` | no `MTL_FLAG_PTP_ENABLE`; `mtl_init_params.ptp_get_time_fn` returns `clock_gettime(CLOCK_TAI)` | host is the single time authority | VF deployments, hosts already PTP-locked via kernel; always used by the test-only kernel backend (§17.2) |

Never subtract or add the UTC offset anywhere in the media path: RTP, MTL and MXL all operate on TAI since the ST 2059-1 epoch. The UTC offset is only displayed.

### 5.3 Clock Supervision

- Every second, sample `d = mtl_ptp_read_time(mt) − clock_gettime(CLOCK_TAI)` (take the min-delay sample of several reads, like MTL `phc2sys_adjust`).
- Expose `mxl_st2110_gateway_clock_mtl_minus_host_tai_ns` (gauge) and its 60 s min/max.
- Thresholds (configurable): `ptp.warn_offset_ns` default 10 000 (10 µs), `ptp.max_offset_ns` default 1 000 000 (1 ms). Above max ⇒ `/readyz` reports not ready (reason `clock_mismatch`) and the UI shows a red banner. Media keeps flowing (no automatic stop).
- PTP lock state, offset, path delay and grandmaster data come from the patched MTL API (§5.5), per port, plus the port currently selected by the BMCA. Not locked ⇒ not ready (reason `ptp_unlocked`) unless `ptp.require_lock=false`.

### 5.4 Index Mapping (pure functions, `src/timing/rtpclock.*`)

Definitions: `T` = TAI ns since ST 2059-1 epoch. Media clocks per ST 2110-10: video and ANC 90 kHz, audio = sample rate (48 kHz). The RTP timestamp is the media-clock count since the epoch modulo 2³².

- `unwrapRtp(rtp32, clockHz, refTaiNs) → T`: compute `refTicks = refTaiNs·clockHz/1e9` (128-bit intermediate), choose the 64-bit tick value ≡ `rtp32 (mod 2³²)` closest to `refTicks`, convert back to ns. `refTaiNs` = MTL `receive_timestamp` of the frame (RX) — this is always within a few ms of origination.
- Video/ANC ingest: `index = mxlTimestampToIndex(&grainRate, T)` using MXL's own helper so rounding matches readers. For interlaced flows MXL internally doubles the declared `grain_rate` to a **field rate** (`FlowParser.cpp` ~l.303–305) and each grain holds one field (`height/2` lines); use the field rate for index math. Each MTL field (`second_field` flag) becomes one grain.
- Audio ingest: sample index `s = unwrapRtp(rtp32, 48000, ref)` directly in samples (VERIFIED: MXL `docs/Timing.md` "Continuous flows typically pass the sample rate (`grainRate`)"; the sync group computes `timestampToIndex(sampleRate, T)`). MXL's `OpenSamples(index, count)` addresses the `count` samples **ending** at `index`, so a block starting at `s` is opened with `index = s + n`.
- Egress (owner decision Q1, "transmit time" model): `T = mxlIndexToTimestamp(&rate, index)`; transmit time `T_tx = T + output_delay_ns`; `rtp32 = (T_tx·clockHz/1e9) mod 2³²` for every essence of the group. This matches ST 2110 practice (the RTP timestamp reflects the sender's output timing) and is what MTL can do uniformly: `st30p` TX has no user-timestamp flag and derives its RTP timestamp from the pacing time (MTL `st_tx_audio_session.c` `tx_audio_session_sync_pacing`); `st20p`/`st40p` use `USER_PACING` + `USER_TIMESTAMP` with the same TAI value. Lip-sync is kept because all essences of a group use the same `output_delay`. An "origin time" model (RTP = `T`) is possible later with `rtp_timestamp_delta_us = −output_delay` on `st20p`/`st30p` and an MTL change for `st40p`.

Mandatory unit tests: 25/1, 50/1, 30000/1001, 60000/1001, interlaced 25/1, 48 kHz; values just before/after a 2³² RTP wrap; reference ahead/behind by ±0.5 wrap; round-trip `index → rtp → index` is identity for 10⁶ consecutive indices from a 2026 epoch value; egress `T_tx` and RTP for a non-zero `output_delay`.

### 5.5 MTL Patches: PTP Status, Domain Filter, Dual-Port BMCA (`patches/mtl/`)

The public MTL API exposes only `mtl_ptp_read_time[_raw]` and the `ptp_sync_notify` callback (`master_utc_offset`, `delta`). Required status lives in the internal `struct mt_ptp_impl` (`lib/src/mt_main.h`): `locked`, `master_initialized`, `master_port_id`, `master_utc_offset`, `t1_domain_number`, `stat_delta_*`, `stat_path_delay_*`, `stat_*_err`, `stat_sync_cnt`. VERIFIED (MTL `v26.09`): there is no real lock in `mt_ptp_impl` (`ptp_timesync_lock` is a stub); Announce is parsed **only once** (`ptp_parse_announce`, guarded by `if (!ptp->master_initialized)`); Sync/Follow_Up/Delay_Resp from any other `source_port_identity` are dropped forever (`mt_ptp_parse`); `grandmaster_identity` is never stored; the domain number is only recorded, never filtered; PTP runs on `MTL_PORT_P` and on other ports only with RX timestamp offload (`mt_ptp_init`), and `mtl_ptp_read_time` always uses `MTL_PORT_P`.

Three small patches, each applying cleanly to `v26.09` on top of the previous one (CI fails otherwise), prepared as upstream PRs (`docs/upstream/mtl-ptp-status.md`):

1. **`0001-ptp-status-api.patch`** — add `struct mtl_ptp_status` and `int mtl_ptp_get_status(mtl_handle, enum mtl_port, struct mtl_ptp_status*)` to `include/mtl_api.h`, returning: `active`, `locked`, `master_initialized`, `selected` (this port steers the PHC), parent port identity (clock id + port number), **grandmaster identity**, `grandmaster_priority1/2`, `grandmaster_clock_quality` (class, accuracy, variance), `steps_removed`, `time_source`, `domain_number`, `utc_offset`, last/min/max/avg delta, last/min/max/avg path delay, sync count, error counters, `gm_change_count`, PHC2SYS locked flag. Parse **every** Announce: update the GM fields each time, count and log GM changes. Copy out under a per-instance seqlock written only by the PTP tasklet.
2. **`0002-ptp-domain-filter.patch`** — `mtl_init_params.ptp_domain` (`int16_t`, `-1` = accept any, the MTL default): PTP messages whose `domain_number` differs are ignored. The gateway passes `ptp.domain`.
3. **`0003-ptp-dual-port-bmca.patch`** — `MTL_FLAG_PTP_DUAL_PORT`: run a PTP instance on `MTL_PORT_P` and `MTL_PORT_R` (both share the E810 PHC). Each instance keeps the best Announce it received in the domain (IEEE 1588 data-set comparison: priority1, clockClass, clockAccuracy, offsetScaledLogVariance, priority2, grandmasterIdentity, stepsRemoved, sender port identity). A BMCA across both ports selects one instance; only the selected instance steers the PHC and answers `mtl_ptp_read_time`, the other one only listens. An instance whose parent sends no Announce for 3 announce intervals drops it and re-selects (also across ports), so a parent change — GM failover behind a transparent clock or a failed leg — re-locks instead of free-running. Selection changes are counted and logged.

The pure parts (data-set comparison, timeout and selection logic) are implemented as static functions that the gateway mirrors in `src/timing/ptp.*` and unit-tests with the same vectors; the patch itself is verified by an apply check in CI, a stub test of the API and on hardware (`docs/acceptance.md`).

### 5.6 Unlocked Sources

The gateway does not resample or drop/repeat to compensate drift. If a source is not locked to the same grandmaster, ingest indices drift relative to host time. Detect it: per ingest essence expose `mxl_st2110_gateway_ingest_origin_age_ns` = `now_tai − T(origin)` (gauge, last frame). If outside `[−history/2, +history/2]` the essence state becomes `degraded` with reason `source_clock_drift`; writes continue at the RTP-derived index. VERIFIED (MXL `v1.1.0` `PosixDiscreteFlowWriter.cpp`): `mxlFlowWriterOpenGrain` returns `MXL_ERR_INVALID_ARG` for any index ≤ the last committed index (there is no time-window check), and a forward jump invalidates the skipped grains. The continuous writer rejects overlapping sample ranges the same way. Rejected frames are counted (`mxl_st2110_gateway_mxl_write_errors_total`); if a source's timestamps jump backwards and stay behind (≥ `history` of consecutive rejections), the gateway releases and re-creates the FlowWriter (same flow id) to resynchronise, logs `mxl_writer_resync` and counts it.

### 5.7 Egress Synchronisation

- All MXL readers of one egress group are added to one `mxlFlowSynchronizationGroup` (`mxlFlowSynchronizationGroupAddReader`), owned by the group's worker thread (§3.6). For grain period *i* the worker waits with `mxlFlowSynchronizationGroupWaitForDataAt(group, T(i), timeout)`, then reads grain *i* of every video/ANC essence and the audio blocks of that period (bounded waits up to their deadlines). VERIFIED (MXL `v1.1.0` `FlowSynchronizationGroup.cpp`): for each reader the wait computes `expectedIndex = timestampToIndex(rate, T)` and blocks until the writer head reaches it — a complete grain for discrete flows, the sample at `T` for continuous flows; it returns `MXL_ERR_OUT_OF_RANGE_TOO_EARLY` when the timeout expires.
- Video/ANC are read per grain. Audio is read in blocks of `audio.block_us` (a multiple of the packet time) aligned to packet-time epochs, independent of the video cadence (at 29.97/59.94 a frame is not an integer number of samples).
- Transmit uses `USER_PACING` with the TAI transmit time `T_tx = T(i) + output_delay` on all three pipelines (`ST20P_TX_FLAG_USER_PACING`, `ST30P_TX_FLAG_USER_PACING`, `ST40P_TX_FLAG_USER_PACING`) and `ST20P_TX_FLAG_USER_TIMESTAMP` / `ST40P_TX_FLAG_USER_TIMESTAMP` with the same TAI value; `st30p` derives its RTP timestamp from the pacing time (§5.4). Use `*_DROP_WHEN_LATE` and count late frames.
- `output_delay` (per group, `output_delay_ns`, default **two grains** of the group's video rate — 40 ms at 50p — or 2 × `block_us` for audio-only groups, raised to one grain + the largest read offset + 2 ms when a read offset needs it; an explicit value must be at least that minimum and, in a group with video, at most the largest read offset + 5 grains) defines the constant MXL→wire latency. All essences of a group use the same delay ⇒ lip-sync preserved. Exact frame-epoch alignment of the output is not required (owner decision Q2); MTL's default user pacing aligns to the virtual receiver schedule anyway.
- **Read offset.** Every MXL Receiver (egress essence) has a read offset (`read_offset_grains` or `read_offset_ns`, §9.5; if unset, `mxl.default_read_offset_*`, which defaults to `0`, suitable for local flows) by which the gateway reads behind the writer. The worker does not start waiting for grain *i* (or the audio block starting at sample *s*) before the end of that grain period plus the read offset. Flows replicated from another host arrive in their mirror domain with replication lag (mxl-fabrics-agent §11 item 5), so their receivers SHOULD use a read offset ≥ the observed lag (`mxl_st2110_gateway_mxl_read_lag_grains`, §12.1, or the agent's `mxl_fabrics_agent_replication_lag_grains`). One sync group waits for one timestamp, so the group's worker uses the largest read offset of the group's essences. `output_delay_ns` MUST be at least one grain plus that largest read offset (validation, §9.5), so the transmit deadline `T(i)+output_delay` stays reachable. In a group with video it MUST be at most that read offset plus 5 grains: a grain waits from its read (`T(i)` + one grain + read offset) until it has been sent (`T(i)` + output delay + one grain), and an ST 2110-20 sender holds at most 8 frames (MTL `ST20_FB_MAX_COUNT`; 5 grains + one in transmission + two spare). The video sender's frame queue is the output delay in grains + 3, capped at 8. The read offset changes only the read schedule, never RTP timestamps or lip-sync.
- **No data by the deadline.** If a grain or audio block is not available when its transmit deadline would be missed, or the grain carries `MXL_GRAIN_FLAG_INVALID`, the essence counts a read timeout (`mxl_st2110_gateway_mxl_read_timeouts_total`) or invalid grain (`mxl_st2110_gateway_mxl_grains_invalid_total`) and sends replacement data so the ST 2110 stream stays continuous (owner decision Q3). Per group `missing_data`: `"black"` (default) — black video (Y=64, Cb=Cr=512), silent audio, ANC grain with `ANC_Count = 0`; `"repeat"` — the last good video grain, silent audio, empty ANC. Data that arrives after its deadline is skipped and counted as a late read (`mxl_st2110_gateway_mxl_late_reads_total`). This is the `no_signal` behaviour of §5.8, not an error. The same replacement data is sent whenever the essence's ST 2110 Sender is active but there is nothing to read (MXL Receiver in `waiting_for_flow` or not enabled).

### 5.8 MXL Reader Bring-up and Resilience (egress)

Applies to every MXL Receiver whose IS-05 `/active` has `master_enable=true`. A missing or silent flow is a normal operating state (for example a source not started yet, a writer restart, or replication by mxl-fabrics-agent that has not started); it never needs operator or controller action.

- **Flow or domain not present.** If `mxlCreateFlowReader` returns `MXL_ERR_FLOW_NOT_FOUND`, or the resolved domain disappeared (for example a mirror domain removed by mxl-fabrics-agent), the essence enters `waiting_for_flow`. It retries with exponential backoff: the first retry after 500 ms, doubling up to 5 s (see changelog C4), with ±10 % jitter. Every attempt re-runs domain resolution (§8.5); a negative result is never cached. Retrying continues for as long as `master_enable` is `true` — a missing flow or domain never fails the receiver permanently. `master_enable=false` stops the retries. The IS-05 activation itself succeeds immediately (§6.4).
- **Flow present but silent.** A flow that exists but delivers no new grains or samples — writer stopped, source lost, or a mirror flow whose replication has not started yet (mxl-fabrics-agent §7.1) — puts the essence in state `no_signal`. `no_signal` is not `error`: it does not affect `/readyz`, the output follows the "no data by the deadline" rule of §5.7, reads continue at the normal cadence, and the essence returns to `running` with the first grain that arrives in time, without reconnecting.
- **Flow re-created.** On `MXL_ERR_FLOW_INVALID` (the writer re-created the flow), the reader is released and re-created immediately with the same flow id, the flow definition is re-checked (§6.4) and reading resumes.
- **Flow definition mismatch.** Whenever a flow appears or is re-created, its `flow_def.json` is checked against the essence format (§6.4). A mismatch puts the essence in `error` with reason `format_mismatch`, but the retry loop continues, so a later matching flow is picked up automatically.
- **Sender not created.** If the ST 2110 sender of an enabled RTP Sender cannot be created (`egress_sender_failed` in the log), the essence is `error` with reason `egress_sender_failed` whatever the MXL side reads; creation is retried after 1 s, doubling up to 30 s, and with every new activation. A disable clears it.
- The other essences of a group keep running while one essence waits. Readers are added to and removed from the group's sync group on the thread that owns it (§3.6).
- Counters and gauges: `mxl_st2110_gateway_mxl_flow_not_found_total`, `mxl_st2110_gateway_mxl_read_timeouts_total`, `mxl_st2110_gateway_mxl_late_reads_total`, `mxl_st2110_gateway_mxl_grains_invalid_total`, `mxl_st2110_gateway_mxl_read_lag_grains`, `mxl_st2110_gateway_mxl_reader_info` (§12.1).

---

## 6. Essences

### 6.1 Video (ST 2110-20 ⇄ `video/v210`)

Supported formats (v1): YCbCr 4:2:2 10-bit (`sampling=YCbCr-4:2:2`, `depth=10`), progressive and interlaced, rates 23.98/24/25/29.97/30/50/59.94/60 (interlaced only 25/1 and 30000/1001 per MXL), sizes **1920×1080 and 3840×2160** (primary use case 1080p50). 1280×720 is not supported in v1 (owner decision Q4): MTL's RFC 4175 ⇄ v210 converters require a multiple of 3 pixel groups per line (`st20_rfc4175_422be10_to_v210_scalar` returns `-EINVAL` otherwise), while MXL pads v210 lines to 128 bytes (3456 B for 1280 px). For 1920 and 3840 px the tight v210 line length equals MXL's stride `((width+47)/48)*128`. Colorimetry BT709 / BT2020 and TCS SDR/PQ/HLG are carried as metadata (SDP + flow) without processing. `video/v210a` is out of scope.

**Ingest:** `st20p_rx` with `transport_fmt = ST20_FMT_YUV_422_10BIT`, `output_fmt = ST_FRAME_FMT_V210`, `ST20P_RX_FLAG_EXT_FRAME` + `ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME`, external frame = the MXL grain buffer opened with `mxlFlowWriterOpenGrain` (CPU SIMD conversion writes directly into shared memory — one pass, no extra copy). Line stride MUST equal MXL v210 stride `((width+47)/48)*128`. VERIFIED (MTL `v26.09` `st20_pipeline_rx.c`): in conversion mode `query_ext_frame` is called on the MTL lcore when a frame is complete, with the RTP timestamp and receive timestamp in the frame meta, and the ext frame becomes the **conversion destination**; the conversion itself runs in `st20p_rx_get_frame` on the worker thread. The callback therefore computes the grain index (§5.4) and opens the grain (lock- and allocation-free in MXL); because the conversion is done by the CPU, the grain buffers need not be DMA-mapped. Never write unconverted RFC 4175 data into a v210 grain.

Commit the grain with `validSlices == totalSlices` (VERIFIED: MXL `v1.1.0` `mxlGrainInfo` has `validSlices`/`totalSlices`, not `committedSize`; MXL's own `docs/Architecture.md` is outdated here). For frames with `status != complete` write the frame anyway, set `MXL_GRAIN_FLAG_INVALID` (`lib/include/mxl/flow.h`) and count `incomplete_frames`.

**Egress:** `st20p_tx` with `input_fmt = ST_FRAME_FMT_V210`, `transport_fmt = ST20_FMT_YUV_422_10BIT`, `ST20P_TX_FLAG_EXT_FRAME` (+ `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` if needed) pointing at the read-only mmapped grain; MTL converts v210 → RFC 4175 into its own hugepage buffer. Replacement frames (§5.7) come from a pre-filled black frame or a copy of the last good grain. Pacing `ST21_PACING_NARROW` by default (`LINEAR`/`WIDE` configurable). Packing GPM/BPM configurable (default BPM).

### 6.2 Audio (ST 2110-30 ⇄ `audio/float32`)

Supported: L24 and L16, 48 kHz, 1–64 channels (validated against ST 2110-30 conformance levels A/B/C for the chosen packet time), packet time 1 ms or 125 µs.

MXL audio is a **continuous** ring buffer with **one de-interleaved channel buffer per channel** (`docs/Architecture.md`, `mxlWrappedMultiBufferSlice` with `stride` between channels, wrap-around as two fragments).

**Ingest:** `st30p_rx` with `framebuff_size` = `audio.block_us` worth of samples (default 1000 µs = 48 samples, MUST be an integer multiple of the packet size, `st30_get_packet_size`). Per frame: unwrap RTP → sample index *s*; `mxlFlowWriterOpenSamples(writer, s + n, n, &slices)` (MXL addresses the `n` samples **ending** at the given index); convert big-endian interleaved L24/L16 to float32 (`x / 2^(bits−1)`, no clamping) into each channel's fragment(s); `mxlFlowWriterCommitSamples`. Respect `mxlFlowWriterGetMaxWriteLengthSamples` (half the buffer).

**Egress:** read the `n` samples starting at *s* (`mxlFlowReaderGetSamples(reader, s + n, n, …)`), convert float32 → L24/L16 big-endian interleaved **with clamping to [−1.0, +1.0) and round-to-nearest** (MXL leaves clamping to consumers), hand to `st30p_tx` with user pacing (§5.7).

The converter (`src/codec/audioconv.*`) MUST be SIMD-friendly scalar code with unit tests for full scale, −full scale, zero, over-range clamping, both bit depths, 1/2/8/16/64 channels and fragment wrap.

### 6.3 Ancillary Data (ST 2110-40 ⇄ `video/smpte291`)

MXL stores per grain the RFC 8331 payload **starting at the Length field** (the first 14 bytes — RTP header and extended sequence number — are not stored) in a fixed **4096-byte** grain (`docs/Architecture.md` "Ancillary Data", `docs/FabricsBandwidth.md`, `lib/include/mxl/dataformat.h`).

**Ingest:** `st40p_rx` delivers `st40_frame_info` (array of `st40_meta` with C/line/offset/stream/DID/SDID/UDW size + UDW buffer). `src/codec/anc8331.*` serialises that into the RFC 8331 structure (Length, ANC_Count, F, reserved, then per packet C, Line_Number, Horizontal_Offset, S, StreamNum, DID, SDID, Data_Count, UDW (10-bit, with parity bits), Checksum_Word, word_align). Grain index from the RTP timestamp (one grain per video frame; for interlaced formats one grain per **field**, and the ANC flow's `grain_rate` is the field rate, e.g. 50/1 for 1080i50 — owner decision Q10, because MXL does not double the rate of data flows). Frames with no ANC still produce a grain with `ANC_Count = 0` so readers keep cadence. A data grain has 4096 one-byte slices; it is always committed with `validSlices == totalSlices` (the Length field carries the payload size), because a partial commit would keep the grain open.

**Egress:** parse the grain back into `st40_meta`/UDW and send via `st40p_tx`. Payloads larger than one RTP packet are split by MTL.

Accepted v1 limits of MTL's ANC pipeline (owner decision Q10): at most 20 ANC packets per frame/field (`ST40_MAX_META`; extra packets are dropped and counted), and 8-bit user data words (MTL strips/regenerates bits 8–9, i.e. parity).

Unit tests: golden vectors for SMPTE 12M timecode (DID 0x60/SDID 0x60), CEA-708 (0x61/0x01), AFD (0x41/0x05), empty frame, parity and checksum generation, round-trip identity, rejection of truncated/oversized grains. Cross-check with MXL `mxl-data-probe` in integration tests.

### 6.4 Format Fixation and Validation

Formats are part of the essence configuration and are therefore static NMOS Flow attributes.

- Ingest: an IS-05 activation whose SDP does not match the configured essence format (resolution, rate, interlace, sampling/depth, channels, sample rate, bit depth, ptime) MUST be rejected at staging with HTTP 400 and a descriptive error. Receiver caps (BCP-004-01) advertise exactly the configured format. VERIFIED (nmos-cpp `fe30384` `connection_api.cpp`): nmos-cpp turns every transport-file error — including its own caps check — into HTTP 500 (AMWA IS-05 issue #40). The gateway therefore parses the SDP in its own `parse_transport_file` callback without the caps check (a malformed SDP still yields 500) and checks the format in the `validate_staged` callback, throwing a `web::json::json_exception`, which nmos-cpp maps to 400 (decision Q14).
- Egress: the MXL Receiver caps advertise the configured format; staging a `mxl_flow_id` whose `flow_def.json` exists in the resolved domain (§8.5) and does not match is rejected with 400 when `master_enable` is true. A disable is always accepted, whatever the staged flow (or, on an RTP Receiver, the staged SDP); otherwise a route that no longer fits could not be switched off. A not-yet-existing flow is accepted; the essence waits (state `waiting_for_flow`) and attaches when the flow appears (retry schedule of §5.8: 500 ms doubling to 5 s). Because a flow can appear or be re-created after staging (for example a mirror flow created by mxl-fabrics-agent), the format check is repeated every time the flow appears or is re-created; a mismatch then becomes essence state `error` / `format_mismatch` (§5.8) instead of an HTTP error.

---

## 7. NMOS Model and Behaviour

### 7.1 nmos-cpp Integration

- Use `nmos::experimental::make_node_server(node_model, implementation, log_model, gate)` as in mxl-decklink `src/nmos/node.cpp`, then mount the gateway routers on the **same** listener: `server.api_routers[{ {}, http_port }].mount(U("/admin"), …)`, likewise `/api`, `/metrics`, `/livez`, `/readyz`, `/statusz` (`nmos::server::api_routers` is a public `std::map<host_port, api_router>`, `nmos/server.h`). VERIFIED (nmos-cpp `fe30384`): `make_node_server` already created the listeners and `support_api` appended a catch-all `.*` handler that answers 404 (`api_utils.cpp` `add_api_finally_handler`); routers share their routes through a `shared_ptr`. The gateway therefore removes that handler with `api_router::pop_back()`, mounts its routes and re-adds it with `nmos::add_api_finally_handler`. Add an integration test proving all of them answer on one port. In setup mode (§9.1) a bare `nmos::server` serves only the gateway routes on the same port.
- Settings: `http_port` = the node's port; disable IS-07 (`events_port`, `events_ws_port`), IS-08 (`channelmapping_port`), IS-12/MS-05 (`configuration_port`, `control_protocol_ws_port`), IS-13 annotation (`annotation_port`) and the nmos-cpp Settings and Logging APIs (`settings_port`, `logging_port`, which nmos-cpp otherwise puts on `http_port`) by setting their ports to `-1`. VERIFIED: the routers are still created, but `nmos::server::open_listeners` (`server.cpp`) never opens a listener with a negative port, and `make_device` omits controls for negative ports. The gateway opens no host ports besides the two node ports and, if set, `node.web_port` (port table §15.4). A port that cannot be bound exits 75 (§14.4).
- **Two nodes (platform guideline, 1.3).** The process runs two nmos-cpp node servers with their own models: the **MXL node** (`http_port` = `node.http_port`) holds the MXL Senders and Receivers and registers with `node.registry` (the platform's MXL registry); the **ST 2110 node** (`http_port` = `node.st2110.http_port`, default `node.http_port + 1`; disabled with `node.st2110.enabled = false`) holds the RTP Senders and Receivers and registers with `node.st2110.registry` — none by default, so ST 2110 resources never reach the MXL registry. Both share the IS-05 callbacks, the activation path and the media pipeline. Label of the ST 2110 node: `node.st2110.label`, default `node.label + " ST 2110"`.
- `seed_id` = node UUID (§7.3); `label`/`description` from config; node and device `tags` = `node.tags`.
- **IP literals only (G5).** `host_address` = the resolved host address (`node.host_address` = `NMOS_HOST_ADDRESS`; deprecated `node.public_address`; default the deprecated `node.management_addresses[0]`, else the IPv4 of the default-route interface, else the first non-loopback, non-link-local IPv4); `host_addresses` = that address (plus the deprecated management addresses on the MXL node); `href_mode = 2` (addresses), so hrefs, `api.endpoints` and IS-05 hrefs never carry the host name. `node.st2110.host_address` overrides it for the ST 2110 node. The address must be an announceable IPv4 literal (no host name, `0.0.0.0`, `127/8`, link-local, multicast) — exit 78 otherwise. The SDP origin is the media port IP.
- **Registry.** `node.registry.dns_sd` (default false; null = true only for the deprecated `mode: "dns-sd"`): with DNS-SD the node browses for a registry and advertises itself over mDNS; without it nmos-cpp runs with `pri` = `highest_pri` = `lowest_pri` = `no_priority` — no browsing, no advertisement, no Avahi or D-Bus (VERIFIED in `src/nmos/node.cpp`). A static registry is `registry_address` = `node.registry.address`, `registration_port` = `node.registry.port` (default 3210), `registry_version` "v1.3" (VERIFIED setting names, `settings.h`); it is also the fallback when DNS-SD finds nothing. `query_address`/`query_port` (defaults: registry address, port + 1) are reported in `/api/nmos`; the gateway does not query.
- Reverse proxy / port mapping (owner decision C1): when `node.public_port` is set, the MXL node's hrefs advertise that port (all `*_port` settings = `public_port`) while the listener binds `http_port` (nmos-cpp `proxy_map` `[{client_port: public_port, server_port: http_port}]`).
- Source files that include nmos-cpp headers are compiled as C++17 (websocketpp does not compile as C++20), as in mxl-decklink; the rest of the gateway is C++20.
- IS-04 v1.3; IS-05 v1.1 and v1.2 (MXL resources only under v1.2 — nmos-cpp `connection_api.cpp` l.91).
- TLS (BCP-003-01) optional: `node.tls.enabled` → `server_secure=true`, certificate/key paths from mounted secrets. When enabled it applies to the whole port (admin UI and `/metrics` included).

### 7.2 Resources per Group

Device: one Device per Node (`label` = node label, so the MXL node's device label starts with `node.label`), `type urn:x-nmos:device:generic`, `tags` = `node.tags`. The Senders/Receivers of a node belong to its device.

Which node holds what: the **MXL node** has the Source + MXL Flow + MXL Sender of every ingest essence and the MXL Receiver of every egress essence; the **ST 2110 node** has the RTP Receiver of every ingest essence and the Source + ST 2110 Flow + RTP Sender of every egress essence.

Ingest group, per essence *e*:

| Resource | Key attributes |
|---|---|
| Receiver (2110, ST 2110 node) | `transport urn:x-nmos:transport:rtp.mcast`, `format` video/audio/data, `caps.media_types` (`video/raw`, `audio/L24` or `audio/L16`, `video/smpte291`), `caps.constraint_sets` = exact configured format (BCP-004-01), `interface_bindings` per §4.5 |
| Source + Flow (MXL, MXL node) | Flow = the MXL flow descriptor (`format`, `media_type` `video/v210` / `audio/float32` / `video/smpte291`, geometry/rate/channels); **the IS-04 Flow `id` IS the MXL flow id** and the JSON written to `flow_def.json` is the IS-04 Flow body (BCP-007-03 permits but does not require this; the gateway requires it so that controllers and mxl-fabrics-agent, which mirrors flows by id, can match IS-04 Flows and MXL flows on every host) |
| Sender (MXL, MXL node) | `transport urn:x-nmos:transport:mxl`, `interface_bindings: []`, `manifest_href: null`, `flow_id` = Flow id |

Egress group, per essence *e*:

| Resource | Key attributes |
|---|---|
| Receiver (MXL, MXL node) | `transport urn:x-nmos:transport:mxl`, `interface_bindings: []`, `format`, `caps.media_types` (`video/v210` / `audio/float32` / `video/smpte291`), `caps.constraint_sets` = configured format |
| Source + Flow (2110, ST 2110 node) | `video/raw` / `audio/L24`/`L16` / `video/smpte291` with configured attributes |
| Sender (2110, ST 2110 node) | `transport rtp.mcast`, `manifest_href` → `/transportfile`, SDP generated by nmos-cpp `nmos::make_sdp_parameters` + `make_session_description` (with `a=group:DUP` and two media sections when redundant, `ts-refclk:ptp=IEEE1588-2008:<gm>:<domain>`, `mediaclk:direct=0`) |

Tags on every Source/Flow/Sender/Receiver: `urn:x-nmos:tag:grouphint/v1.0` = `["<group label>:<Role> <n>"]` (BCP-002-01; roles `Video`, `Audio`, `Data`; numbering per type starting at 1), e.g. `CAM 1:Video 1`, `CAM 1:Audio 3`. Because an essence's Receiver and Sender sit on different nodes, the roles are unique per node (IS-04-01).

### 7.3 Stable Identifiers

IDs MUST survive restarts and container recreation.

- Node seed: `node.id` (UUID) in the config. Generated once on first start if absent and written back to the file. ST 2110 node id = UUIDv5(MXL node id, `"st2110-node"`).
- **`node.seed` (`NMOS_SEED`, platform guideline G3).** Seed namespace = UUIDv5(URL namespace, `"urn:x-mxl-st2110-gateway:seed:" + seed`). With a seed: MXL node id = UUIDv5(seed namespace, `"node"`), ST 2110 node id = UUIDv5(seed namespace, `"st2110-node"`) (`node.id` is ignored with a warning), every essence derivation below uses UUIDv5(seed namespace, essence `uid`) as its namespace instead of the `uid`, and a configured domain without an `id` gets UUIDv5(seed namespace, `"mxl-domain:" + name`). Seed-derived ids are never written back.
- Every group has an immutable `uid` (UUID) generated at creation and persisted. Renaming a group keeps its IDs.
- Every essence has an immutable `uid` as well (so reordering/removing essences does not shift IDs).
- Resource IDs = UUIDv5(namespace = essence `uid`, name = `"sender"|"receiver"|"source"`). Flow id = UUIDv5(namespace = essence `uid`, name = `"flow:" + canonical format`), where the canonical format is a fixed-order string of the format fields that end up in the flow (e.g. `video/v210;1920x1080;50/1;progressive;BT709;SDR`, `audio/float32;48000;8`, `video/smpte291;50/1`), so a format change mints a new flow UUID (owner decisions C5, Q8) while rename and restarts keep it. Device id = UUIDv5(node id, `"device"`).
- MXL domain ids (`domain_def.json` `id`) of configured domains are persisted in the config file and re-used after every restart and every tmpfs wipe (§8.3), because other hosts mirror a domain by its id (`mirror-<domain-id>`, §8.6) and controllers stage it as `mxl_domain_id`.
- **New flow UUID.** Editing an essence's format mints a new Flow id (derivation above). Whenever a new flow UUID is minted for an ingest essence, the gateway MUST, in one NMOS model update and before the new FlowWriter commits its first grain: insert the new IS-04 Flow (same Source); remove the old Flow; set the MXL Sender's `flow_id`; set `mxl_flow_id` in the Sender's IS-05 `/active` and `/staged` and in its `mxl_flow_id` constraint to the new id; bump all affected versions; and release the old FlowWriter. This is the same sequence as mxl-decklink (`onRuntimeFlows` in `src/nmos/node.cpp`). Receivers still pointing at the old flow id are not reconnected by the gateway; that is the controller's job (mxl-fabrics-agent reports them as `stale_reference`).
- Unit test: same config ⇒ same IDs; rename ⇒ same IDs; delete+recreate essence ⇒ new IDs; format change ⇒ new Flow id only; new flow UUID ⇒ Flow, Sender `flow_id` and `/active` `mxl_flow_id` updated together.

### 7.4 IS-05 Behaviour — MXL Side (BCP-007-03 v1.0.0)

Use nmos-cpp `make_connection_mxl_sender(id, domain_id, flow_id)` / `make_connection_mxl_receiver(id, domain_id)` and its `resolve_auto` machinery.

- One transport-parameter set (no `_R` leg) in `/staged`, `/active`, `/constraints`.
- MXL Sender (ingest): constraints `mxl_domain_id.enum = [group domain id]`, `mxl_flow_id.enum = [flow id]`. `auto` resolves to those. `null` accepted. `/transportfile` returns 404.
- MXL Receiver (egress): `mxl_domain_id` accepts any UUID, `null` or `auto`; its constraint is unconstrained (`{}`), so a domain that does not exist yet can be staged. Accessible domains are the configured domains plus the domains discovered under `mxl.scan_path` (§8.5), including mirror domains created by mxl-fabrics-agent; domain identity is the `id` in `domain_def.json`, never the directory name. `auto` resolves to the group's domain, or — if the staged `mxl_flow_id` is found in a different accessible domain — to that domain (BCP-007-03 "Automatic resolution"). Before resolving `auto` the gateway re-scans; negative results are never cached. `mxl_flow_id` accepts UUID or `null`, MUST NOT accept `auto`. A staging/activation request with a transport file is rejected; omitted or `{data:null,type:null}` is accepted.
- **Unknown domain (owner decision C3).** A `mxl_domain_id` that is not accessible after a fresh re-scan is accepted; the gateway logs `mxl_domain_unknown` (warning, rate-limited: first occurrence, then at most every 30 s per receiver) and the essence waits in `waiting_for_flow` (reason `domain_not_found`) with the retry loop of §5.8 until the domain appears — this is what mxl-fabrics-agent `MIRROR_MODE=on-demand` needs. This deliberately deviates from BCP-007-03 ("MUST reject … an MXL Domain the Node is not capable of accessing"): with the agent the node can access remote domains through mirrors (`docs/decisions.md`). Immediate activation whose `auto` cannot be resolved ⇒ HTTP 500 (BCP-007-03). A domain that disappears after activation is handled the same way.
- `master_enable=true` starts the MXL write (sender) / read (receiver); `false` stops it. Stopping a writer releases the FlowWriter (`mxlReleaseFlowWriter`), so the flow becomes inactive for readers. MXL Senders only ever write into their group's configured domain (constraint `mxl_domain_id.enum = [group domain id]`), never into a discovered or mirror domain (§8.3, §8.5).

### 7.5 IS-05 Behaviour — ST 2110 Side

- Receiver (ingest): accepts an SDP transport file **or** transport params (`multicast_ip`, `source_ip` (SSM), `destination_port`, `interface_ip` = `auto`, `rtp_enabled`). SDP parsed with nmos-cpp `sdp_utils` (`get_session_description_sdp_parameters`, `get_video_raw_parameters`, `get_audio_L_parameters`, `get_video_smpte291_parameters`); a pure mapper `src/mtl/sdp_map.*` converts `sdp_parameters` + transport params into MTL ops (fps enum, fmt, ptime, channels, payload type, IPs, ports). A one-leg SDP on a redundant group runs only the primary leg (`rtp_enabled=false` on leg 2).
- Sender (egress): `destination_ip`, `destination_port`, `source_ip` (`auto` → port IP), `rtp_enabled` per leg; defaults from config.
- **Re-activation without rebuild:** if only addresses/ports/`rtp_enabled` change, use `st20p_rx_update_source` / `st30p_rx_update_source` / `st40p_rx_update_source` and `*_tx_update_destination` (all exist in v26.09). Format changes are impossible by design (§6.4). Session teardown/creation only on `master_enable` transitions.
- Activation latency target: immediate activation returns within 200 ms (pipeline change is async but `/active` reflects the new state when the response is sent, as nmos-cpp requires).

### 7.6 Connection Persistence

`node.resume_connections` (default `true`): the last `/active` endpoint of every Sender/Receiver is persisted to `state/connections.json` (same mounted directory as the config, written atomically). On restart the gateway re-stages and re-activates them so the facility recovers without controller action. The state file is not part of config import/export.

**Group enable and first start (owner decision Q7).**

- `groups[].enabled = false`: the group's NMOS resources are not registered and no MTL/MXL objects exist. Toggling it is a group edit (§9.3).
- `enabled = true` and a saved `/active` exists (`resume_connections=true`): the saved state is restored, including `sender_id` / `receiver_id`. A saved Receiver activation that the current staging checks reject (§6.4, for example a route made before the format check) comes up staged disabled: log `restored_activation_disabled` with the reason, essence state `idle` with reason `restore_rejected: …`.
- `enabled = true` and no saved state (first start, new group, or `resume_connections=false`): the gateway stages and immediately activates its defaults — ST 2110 Receivers/Senders with `defaults.legs` and `master_enable=true`, MXL Senders with the group domain and flow id and `master_enable=true`, MXL Receivers with `mxl_domain_id = auto`, `mxl_flow_id = null` and `master_enable=false` (an MXL Receiver needs a flow id from a controller). A gateway therefore runs stand-alone without a controller. Essences without `defaults.legs` stay inactive.

### 7.7 Conformance Targets

AMWA nmos-testing (pinned commit) suites run in CI against a live gateway instance (MTL kernel-socket backend, §17.3): **IS-04-01, IS-05-01, IS-05-02** against both nodes and **BCP-007-03-01** against the MXL node — zero failures, warnings documented in `docs/conformance.md`. BCP-007-03-01 tests 01–18 (`nmostesting/suites/BCP0070301Test.py`) are the checklist for §7.4; test 15 is manual by design. IS-04-01 runs against nmos-testing's own mock registry, discovered by the gateway via **multicast** DNS-SD (`DNS_SD_MODE = 'multicast'`); its unicast-only tests report `DISABLED`, which counts as no failure (owner decision Q13). No separate registry container is used. The unknown-domain behaviour of §7.4 deviates from BCP-007-03 but is not exercised by BCP-007-03-01 (VERIFIED: tests 01–18 at `9001851`).

---

## 8. MXL Domains

### 8.1 Facts (MXL v1.1.0 + BCP-007-03 v1.0.0)

- A domain is a directory on **tmpfs** (or ramfs). Flows live in `${domain}/${flowId}.mxl-flow/` (`data`, `flow_def.json`, `access`, `grains/`, `channels` for audio).
- `mxlCreateInstance(domain, options)` only opens an **existing** directory.
- Optional MXL `options.json` in the domain root: `{"urn:x-mxl:option:history_duration/v1.0": <ns>}` (default 200 ms).
- BCP-007-03 requires `domain_def.json` in every domain root: `{id (UUID), label, description, tags}` (schema `mxl_domain_definition.json`). The **id is the `mxl_domain_id`** used in IS-05. The schema allows additional properties; readers MUST ignore unknown fields (for example the `x-mxl-fabrics-agent` marker below).
- Domains are mapped into containers by volume mounts; the path inside the container may differ from the host path. Identity comes only from `domain_def.json`, never from the directory name.
- All MXL domains of a host normally live as sibling directories in one host tmpfs, the **MXL root** (mxl-fabrics-agent `MXL_ROOT`). mxl-fabrics-agent creates **mirror domains** there as siblings of the local domains: `<MXL root>/mirror-<source-domain-id>/`. Their `domain_def.json` carries the **source** domain's `id` plus a marker object `"x-mxl-fabrics-agent": {"mirror": true, "source_host_id": "…", "owner_host_id": "…"}`; their `options.json` is copied from the source domain; their flows have the source's flow ids and `flow_def.json` verbatim and are written by the agent at the same grain/sample indices (mxl-fabrics-agent §7.2, §9).
- Reader status codes used in §5.8 (`lib/include/mxl/mxl.h`): `MXL_ERR_FLOW_NOT_FOUND` (flow absent), `MXL_ERR_OUT_OF_RANGE_TOO_EARLY` (not written yet; also returned when a blocking read times out), `MXL_ERR_OUT_OF_RANGE_TOO_LATE` (older than the history), `MXL_ERR_FLOW_INVALID` (flow re-created by its writer).

### 8.2 Configuration

`mxl.domains[]` — one or more entries `{name, path, id?, label?, description?, history_duration_ns?, gc_on_start?}`. Each group references a domain by `name`. Paths MUST be absolute and unique. A configured domain path MUST NOT be a mirror domain (basename starting with `mirror-`); configured domains SHOULD lie directly below `mxl.scan_path` (required for replication by mxl-fabrics-agent, §8.6). The first configured domain is the **output domain** of the platform variables `MXL_OUTPUT_DOMAIN_DIR` (path), `MXL_OUTPUT_DOMAIN_ID` (id) and `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS`; when the file has no domain they create it with the name `main`.

`mxl.cleanup_on_exit` (`MXL_CLEANUP_ON_EXIT`, default `false`) — see §8.4.

`mxl.scan_path` — absolute path of the MXL root inside the container (default `/Volumes/mxl`, the sibling projects' convention; `null` disables discovery; environment `MXL_DOMAIN_SCAN_PATH` or `MXLGW_MXL_SCAN_PATH`, §9.1). Egress MXL Receivers resolve `mxl_domain_id` over the configured domains plus all domains discovered under this path (§8.5). Each container maps the host MXL root to its own path (owner decision C7): the paths seen by the gateway, mxl-decklink and mxl-fabrics-agent may differ; domains are matched only by their `domain_def.json` id.

### 8.3 Startup Bootstrap (per domain, before NMOS starts)

1. **Mount check.** Determine the mount containing `path` (or its nearest existing ancestor). It MUST be tmpfs/ramfs (`statfs` `f_type`, same test as `mxlIsTmpFs`) **and** MUST NOT be the container's root/overlay filesystem. If the check fails ⇒ log `mxl_domain_not_tmpfs` with the path and the detected filesystem type, and **exit with code 78 (EX_CONFIG)**. No fallback, no "warn and continue" (stricter than mxl-decklink, by requirement).
2. **Mirror check.** If the basename of `path` starts with `mirror-`, or an existing `domain_def.json` contains an `x-mxl-fabrics-agent` object ⇒ log `mxl_domain_is_mirror` and exit 78. The gateway never writes into a mirror domain: neither files nor flows.
3. **Directory.** Create `path` (and parents within the tmpfs mount) with mode `0775` if missing.
4. **`domain_def.json`.**
   - Exists and valid ⇒ **adopt** its `id`; never rewrite it. Unknown fields are ignored. The adopted id is written back to the config file (owner decision C6), so the same id is used when the file must be re-created after a tmpfs wipe. If the config (or the environment, or the seed) specified a different `id` ⇒ **error** `domain_id_mismatch` (old and new id, platform guideline G2): the file is kept and wins, the gateway uses its id and continues. An id from the environment (§9.1) or derived from `node.seed` (§7.3) is never written back.
   - Exists and invalid ⇒ exit 78 (do not overwrite someone else's file).
   - Missing ⇒ write it atomically (temp file + `rename`) with `id` = config `id`, else the seed-derived id (§7.3), else a new UUIDv4, `label`/`description` from config (default: domain `name`), `tags: {}`; write a generated UUIDv4 back to the config file. The id is therefore stable across restarts and host reboots — remote hosts mirror the domain by this id (§7.3, §8.6).
5. **`options.json`.** Written only if missing **and** `history_duration_ns` is configured. Never overwritten. If present with a different value ⇒ warning, the file wins.
6. `mxlCreateInstance`, then garbage collection (owner decision Q9). VERIFIED (MXL `v1.1.0` `Instance.cpp` `garbageCollect`): `mxlGarbageCollectFlows` is domain-wide and deletes **every** flow whose data file is not write-locked, including stopped flows of other media functions. Default: the gateway only removes its **own** stale flows — the flow ids of its configured ingest essences — using the same test (exclusive non-blocking `flock` on the flow's `data` file succeeds ⇒ no writer ⇒ remove the flow directory). Domain-wide `mxlGarbageCollectFlows` runs only if the domain has `gc_on_start: true` (default `false`). Garbage collection never runs on discovered or mirror domains (§8.5).
7. Expose per domain in `/api/status`: path, id, label, tmpfs ok, flow count, free/used bytes of the mount.

Domain deletion is not offered (as in mxl-decklink).

### 8.4 Flow Lifecycle

- Ingest writer flows are created on MXL Sender activation (`master_enable=true`) with the essence's flow descriptor and released on deactivation or shutdown.
- If `mxlCreateFlowWriter` reports `created=false` (the flow still exists, for example after a crash or while other processes hold it), MXL opens it without comparing definitions (`lib/include/mxl/flow.h`). The gateway compares the existing definition (`mxlGetFlowDef`) with the essence's descriptor; a mismatch is an error (`flow_def_mismatch`, essence state `error`) — an existing flow is never re-used silently with a different format. This matters for mirrored flows, because mxl-fabrics-agent copies `flow_def.json` verbatim (§8.6).
- On SIGTERM: stop MTL sessions, release all FlowWriters/Readers, deregister (§14.4), destroy MXL instances, then MTL (`mtl_uninit`), within `node.shutdown_timeout_s` (default 10 s). With `mxl.cleanup_on_exit` the gateway then removes the directories of its configured domains — only when `domain_def.json` still carries the domain's id, no other process holds a writer lock on a flow, the directory contains no nested domain and it is not the MXL root (`mxl_domain_removed_on_exit`, else `mxl_domain_cleanup_skipped` with the reason). Restarts requested through the API never remove domains.

### 8.5 Domain Discovery (MXL Receivers)

- **Scan set.** `mxl.scan_path` itself and each of its direct subdirectories (depth 1, the mxl-fabrics-agent §5.1 convention) that contains a `domain_def.json`, plus every configured domain (always accessible, also outside the scan path).
- **Filesystem.** A candidate is used only if it passes the tmpfs/ramfs test of §8.3 step 1. A failing candidate is skipped with warning `mxl_domain_skipped` (no exit: discovered domains belong to other functions). The scan path itself may be on the container root filesystem, for example when only individual domains are mounted.
- **Identity.** `domain_def.json` MUST be a JSON object with a UUID `id`; `label`, `description` and `tags` are optional when reading (BCP-007-03 requires them, but several MXL writers leave them out; absent they are `""` and `{}`, present they need the schema's type); unknown fields are ignored; an invalid file means the directory is skipped with a warning. The `id` is the domain's identity. The directory name is never used as identity — it is only used to recognise `mirror-*` for the write protection of §8.3.
- **Classification.** `configured` (in `mxl.domains[]`); `mirror` (`x-mxl-fabrics-agent` marker present, or basename `mirror-*`); `discovered` (all others, for example domains of other media functions on the host).
- **Duplicate ids.** If several directories carry the same id, a configured domain wins; otherwise the id is classified `conflict`, excluded from resolution, and reported (status, log `mxl_domain_conflict`, metric `mxl_st2110_gateway_mxl_discovered_domains{kind="conflict"}`); receivers staged with it wait as for an unknown domain (§7.4).
- **When.** At startup, every 2 s while running, and inline before resolving `auto`, before validating a staged `mxl_domain_id` (§7.4) and on every reader retry (§5.8). A scan reads only the scan path directory and the `domain_def.json` files, so it is cheap enough to run inline. Negative results are never cached: a failed lookup always triggers a fresh scan, and its "not found" outcome is not remembered.
- **Access.** For each discovered domain used by at least one reader, the gateway opens one `mxlInstance` and destroys it when no reader uses the domain any more. It never creates directories, `domain_def.json`, `options.json` or flows in discovered domains, never garbage-collects them and never changes their permissions. Readers on mirror domains are plain MXL readers; no Fabrics API is involved.
- **Reporting.** `/api/domains` and the MXL tab list every accessible domain with kind, path, id, label and, for mirror domains, `source_host_id` from the marker (§11). Changes are logged (`mxl_domain_discovered`, `mxl_domain_removed`).

### 8.6 Host-to-Host Replication with mxl-fabrics-agent

mxl-fabrics-agent runs as one container per host. It reads IS-04/IS-05 state from the registry and replicates the flows that enabled MXL Receivers on its host need from the host that holds the origin flow (mxl-fabrics-agent §1, §6, §8). The gateway never calls the agent and needs no configuration for it.

- **Gateway as MXL Sender (ingest, host A).** Flows are written into a configured domain with a stable domain id (§8.3) and stable flow ids (§7.3). For the agent on host A to export the domain, it MUST be a direct subdirectory of the host's MXL root (the agent discovers only direct subdirectories, mxl-fabrics-agent §5.1) — hence the recommendation in §8.2. The agent on host B mirrors the flows into `mirror-<domain-id>` with the same flow ids and indices.
- **Gateway as MXL Receiver (egress, host B).** A controller stages the remote Sender's `mxl_domain_id` and `mxl_flow_id`. The receiver finds the mirror domain by id (§8.5), waits while the mirror flow is missing or still silent (§5.8), and reads behind head by its read offset (§5.7). Both agent modes work: `MIRROR_MODE=eager` (default) creates mirror domains before activation; with `MIRROR_MODE=on-demand` the domain appears after activation and the receiver waits for it (§7.4).

Requirements on media functions from mxl-fabrics-agent §11, and where this specification meets them:

| mxl-fabrics-agent §11 requirement | Gateway section |
|---|---|
| 1. Receivers resolve `mxl_domain_id` by scanning the MXL root; mirror domains are siblings of local domains | §7.4, §8.2, §8.5 |
| 2. Retry with backoff instead of failing when a flow is not (yet) present | §5.8 |
| 3. Tolerate a flow that exists but has no new grains | §5.7, §5.8 |
| 4. No `MXL_ENABLE_FABRICS_OFI` | §1.3, §2, §14.1 |
| 5. Read with a small latency offset on destination hosts | §5.7, §9.5 |

Further rules for this scenario: no writes into mirror domains (§8.3), new flow UUIDs propagated to NMOS (§7.3), all hosts TAI-disciplined (§5.1), non-colliding ports (§15.4), multi-host demo (§15.1, §15.2).

---

## 9. Configuration File

### 9.1 Location, Format, Ownership

- Path: `/config/gateway.json` (override with env `MXLGW_CONFIG`). The `/config` directory is a mounted volume; `state/` lives below it.
- Format: JSON, validated against `schema/gateway-config.schema.json` (shipped in the image at `/usr/share/mxl-st2110-gateway/` and served at `/api/schema`).
- **Precedence (owner decision C2): environment > config file > built-in default**, per setting, as in mxl-decklink. Every scalar setting of `node`, `nic` (the single port pair), `ptp` and `mxl` can be set by an environment variable; groups and essences live in the file only (they are managed by the UI and identified by `uid`). Where the MXL PoC platform defines a standard name, that name is canonical; every other setting uses `MXLGW_` + the upper-snake JSON path. Older names stay valid as **aliases**. If two variables of one setting are set to different values, the start fails with exit 78 naming both; unknown variables are ignored. The complete list with defaults is the README "Settings" table (generated from the schema, `docs/configuration.md`).

  | Environment variable (aliases) | Setting |
  |---|---|
  | `NMOS_SEED` (`MXLGW_NODE_SEED`), `MXLGW_NODE_ID`, `NMOS_LABEL` (`MXLGW_NODE_LABEL`), `MXLGW_NODE_DESCRIPTION`, `NMOS_TAGS` (JSON object, `MXLGW_NODE_TAGS`) | `node.seed`, `.id`, `.label`, `.description`, `.tags` |
  | `NMOS_PORT` (`MXLGW_NODE_HTTP_PORT`, `MXLGW_HTTP_PORT`), `WEB_PORT` (`MXLGW_NODE_WEB_PORT`), `NMOS_HOST_ADDRESS` (`MXLGW_NODE_HOST_ADDRESS`, `MXLGW_NODE_PUBLIC_ADDRESS`), `MXLGW_NODE_PUBLIC_PORT`, `MXLGW_NODE_MANAGEMENT_ADDRESSES` (comma list), `MXLGW_NODE_RESUME_CONNECTIONS`, `MXLGW_NODE_LOG_LEVEL` (`MXLGW_LOG_LEVEL`), `SHUTDOWN_TIMEOUT_S` (`MXLGW_NODE_SHUTDOWN_TIMEOUT_S`) | `node.http_port`, `.web_port`, `.host_address`, `.public_port`, `.management_addresses`, `.resume_connections`, `.log_level`, `.shutdown_timeout_s` |
  | `NMOS_DNS_SD` (`MXLGW_NODE_REGISTRY_DNS_SD`), `NMOS_REGISTRY_ADDRESS` (`MXLGW_NODE_REGISTRY_ADDRESS`), `NMOS_REGISTRY_PORT` (`MXLGW_NODE_REGISTRY_PORT`), `NMOS_QUERY_ADDRESS`, `NMOS_QUERY_PORT`, `MXLGW_NODE_REGISTRY_MODE` (deprecated) | `node.registry.*` |
  | `MXLGW_NODE_ST2110_ENABLED`, `_LABEL`, `_HTTP_PORT`, `_HOST_ADDRESS`, `_REGISTRY_DNS_SD`, `_REGISTRY_ADDRESS`, `_REGISTRY_PORT` | `node.st2110.*` |
  | `MXLGW_NODE_TLS_ENABLED`, `_CERTIFICATE`, `_PRIVATE_KEY` | `node.tls.*` |
  | `MXLGW_NIC_BACKEND`, `_LCORES`, `_LCORE_COUNT`, `_TX_PACING`, `_APP_CPUS`, `_HUGEPAGE_SOCKET` | `nic.*` |
  | `MXLGW_NIC_PRIMARY_NAME`, `_PCI`, `_IFNAME`, `_IP`, `_NETMASK`, `_GATEWAY`; same with `MXLGW_NIC_REDUNDANT_` | `nic.port_pairs[0].primary` / `.redundant` |
  | `MXLGW_PTP_MODE`, `_DOMAIN`, `_REQUIRE_LOCK`, `_WARN_OFFSET_NS`, `_MAX_OFFSET_NS` | `ptp.*` |
  | `MXL_DOMAIN_SCAN_PATH` (alias `MXLGW_MXL_SCAN_PATH`) | `mxl.scan_path` |
  | `MXLGW_MXL_DEFAULT_READ_OFFSET_GRAINS` (alias `MXL_READ_OFFSET_GRAINS`), `MXLGW_MXL_DEFAULT_READ_OFFSET_NS` (alias `MXL_READ_OFFSET_MS`, in ms) | `mxl.default_read_offset_*` — default for MXL Receivers without their own read offset (§5.7) |
  | `MXL_CLEANUP_ON_EXIT` (`MXLGW_MXL_CLEANUP_ON_EXIT`) | `mxl.cleanup_on_exit` |
  | `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`, `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS` | path, id, `history_duration_ns` of the first configured domain (created as `main` if the file has none, §8.2) |
  | `MXLGW_MXL_DOMAIN_<NAME>_PATH`, `_ID`, `_LABEL`, `_DESCRIPTION`, `_HISTORY_DURATION_NS`, `_GC_ON_START` (`<NAME>` = upper-snake domain `name`) | fields of a domain defined in the file |
  | `MXLGW_CONFIG`, `MXLGW_LOG_FORMAT` | bootstrap only (config path, log format); the config file's directory holds all state the gateway writes |

  Values are parsed by type (integers, `true`/`false`, comma lists, JSON objects); an invalid value is a configuration error (exit 78) naming the variable. Kubernetes PCI injection (`"pci": "env:PCIDEVICE_…"`, §15.2) keeps working inside the file. Environment-set keys are shown read-only in the UI with their variable name ("set via environment variable"), cannot be changed through `/api` (per-field error), and are never written into the file — including values the gateway would otherwise write back (`node.id`, domain ids).
- If the file does not exist at startup, the gateway writes `config/examples/gateway.minimal.json` semantics (no groups, NIC unconfigured) and starts in **setup mode**: NMOS and MTL are not started, only the admin UI, `/livez` (ok) and `/readyz` (not ready, reason `unconfigured`).

### 9.2 Writers and Restart Semantics

- **Admin UI writes**: validate → write atomically (temp file in the same directory + `fsync` + `rename`) → keep `gateway.json.bak` (previous version) → apply.
- **Hand edits** are allowed. They take effect only after a **full service restart** (container restart or `POST /api/restart`, which exits with code 0 for the orchestrator/`restart: unless-stopped` to restart). The gateway watches the file's mtime; if it changes on disk while running, the UI shows a persistent banner "configuration changed on disk — restart required" and UI saves are blocked until the operator chooses *reload from disk (restart)* or *overwrite with UI state*.
- Invalid file at startup ⇒ exit 78 with every validation error printed (JSON pointer + message). Validation always runs on the **effective** configuration (file merged with environment and defaults).

### 9.3 Apply Semantics from the UI

| Change | Effect |
|---|---|
| Add a group | applied live: resources created, registered, MTL/MXL objects created on activation |
| Edit / remove a group (incl. its essences) | only that group is torn down and rebuilt; its NMOS resources are re-registered (versions bump, IDs unchanged except removed essences) |
| Edit an essence's network defaults only | applied live via update_source/destination if the essence is active |
| Edit an egress essence's read offset only (`read_offset_grains` / `read_offset_ns`) | applied live by the group's worker from the next grain (subject to the `output_delay_ns` rule of §9.5) |
| `nic.*`, `ptp.*`, `mxl.domains`, `mxl.scan_path`, `mxl.default_read_offset_*`, `node.*` | persisted, flagged `restart_required` (UI banner, `/api/status`) |
| Any key set by an environment variable | not editable (read-only in the UI, rejected by `/api`) |

### 9.4 Import / Export

- `GET /api/v1/config/export` (also `/api/config/export`) → the current file byte-for-byte (`Content-Disposition: attachment; filename=gateway-<node-label>-<date>.json`). The file holds no secrets (TLS uses file paths), so nothing is omitted; environment-set values are not part of it.
- `POST /api/v1/config/import` (also `/api/config/import`) → body validated against the schema and semantic rules; on success written as in §9.2 and the response states `restart_required: true`. Import never applies live; with `?restart=true` the gateway restarts gracefully right after writing (202, exit 0, the orchestrator restarts it), which restores the configuration in one call. Option `keep_ids` (default `true`): keeps `node.id` and all `uid`s from the file; `false` regenerates them (for cloning a gateway onto another host — the UI explains the consequence).

### 9.5 Schema (normative shape)

```jsonc
{
  "schema_version": 1,
  "node": {
    "id": "c0f1…",                      // generated if absent; ignored with "seed"
    "seed": null,                       // NMOS_SEED: every NMOS id and default domain id derive from it (§7.3)
    "label": "GW-STUDIO1-A",
    "description": "ST 2110 <-> MXL gateway",
    "tags": { "urn:x-nmos:tag:location/v1.0": ["Studio 1"] },   // on both nodes and devices
    "http_port": 8080,                  // MXL node; co-location: env override, port mapping or reverse proxy (§15.4)
    "web_port": null,                   // optional own port for /admin, /api, /metrics, health; null = http_port
    "host_address": "10.10.0.21",       // IPv4 literal announced in hrefs; null = default-route interface's address
    "public_port": null,                // optional: port advertised in NMOS hrefs (reverse proxy / port mapping)
    "registry": { "dns_sd": false, "address": "10.10.0.5", "port": 3210 },  // MXL registry; query_address/query_port optional
    "st2110": {                         // ST 2110 node (§7.1)
      "enabled": true, "label": null, "http_port": null,                    // null = label + " ST 2110", http_port + 1
      "host_address": null, "registry": { "dns_sd": false, "address": null }   // no registry by default
    },
    "tls": { "enabled": false, "certificate": "/certs/tls.crt", "private_key": "/certs/tls.key" },
    "resume_connections": true,
    "log_level": "info",
    "shutdown_timeout_s": 10            // SIGTERM: graceful shutdown bound (§14.4)
  },
  "nic": {
    "backend": "dpdk",                  // dpdk | kernel (test-only, ports use "ifname" instead of "pci", §17.2)
    "lcores": "4-9",                    // MTL lcores, disjoint from app threads; null = from the CPU affinity (dpdk)
    "lcore_count": 4,                   // lcores taken from the affinity when "lcores" is null
    "tx_pacing": "auto",                // MTL TX pacing: auto | rl | tsc (§4.2)
    "app_cpus": "10-15",                // optional affinity for worker threads; null = the rest of the affinity (dpdk)
    "hugepage_socket": "auto",
    "port_pairs": [                     // exactly one in v1
      {
        "name": "media",
        "primary":   { "name": "media-p", "pci": "0000:31:00.0", "ip": "10.1.1.21", "netmask": "255.255.255.0", "gateway": "10.1.1.1" },
        "redundant": { "name": "media-r", "pci": "0000:31:00.1", "ip": "10.2.1.21", "netmask": "255.255.255.0", "gateway": "10.2.1.1" }
      }
    ]
  },
  "ptp": {
    "mode": "builtin",                  // builtin | builtin_phc2sys | external
    "domain": 127,                      // filtered by MTL patch 0002; both ports run PTP with BMCA (patch 0003)
    "require_lock": true,
    "warn_offset_ns": 10000,
    "max_offset_ns": 1000000
  },
  "mxl": {
    "scan_path": "/Volumes/mxl",        // MXL root; egress receivers also resolve discovered/mirror domains here (§8.5); null = off
    "default_read_offset_grains": 0,    // or "default_read_offset_ns"; default for receivers without their own (§5.7)
    "domains": [                        // configured domains: the only ones the gateway writes to; never mirror-*
      { "name": "main", "path": "/Volumes/mxl/main", "id": null, "label": "Studio 1",
        "history_duration_ns": 200000000, "gc_on_start": false }
    ],
    "cleanup_on_exit": false            // SIGTERM: remove the configured domains (§8.4)
  },
  "groups": [
    {
      "uid": "8f7e…", "label": "CAM 1", "direction": "ingest", "domain": "main",
      "redundancy": true, "enabled": true,
      "video": [ { "uid": "…", "label": "CAM 1 V", "width": 1920, "height": 1080, "rate": "50/1",
                   "interlace": "progressive", "colorimetry": "BT709", "tcs": "SDR",
                   "payload_type": 96, "packing": "BPM",
                   "defaults": { "legs": [ { "multicast": "239.1.1.1", "source": "10.1.1.50", "port": 20000 },
                                           { "multicast": "239.2.1.1", "source": "10.2.1.50", "port": 20000 } ] } } ],
      "audio": [ { "uid": "…", "label": "CAM 1 A1-8", "channels": 8, "bit_depth": 24, "sample_rate": 48000,
                   "ptime_us": 1000, "block_us": 1000, "payload_type": 97, "defaults": { "legs": [ … ] } } ],
      "anc":   [ { "uid": "…", "label": "CAM 1 ANC", "payload_type": 100, "defaults": { "legs": [ … ] } } ]
    },
    {
      "uid": "…", "label": "PGM OUT", "direction": "egress", "domain": "main", "redundancy": true, "enabled": true,
      "output_delay_ns": null,                           // null = two grains, at least one grain + largest read offset + 2 ms (§5.7); with video at most largest read offset + 5 grains
      "missing_data": "black",                           // black | repeat (§5.7)
      "video": [ { …format…, "pacing": "narrow",
                   "read_offset_grains": null,          // or "read_offset_ns"; per MXL Receiver; null = mxl.default_read_offset_* (§5.7)
                   "defaults": { "legs": [ { "multicast": "239.10.0.1", "port": 20000 }, { … } ] } } ],
      "audio": [ … ], "anc": [ … ]                       // audio/anc egress essences accept the same read offset keys
    }
  ]
}
```

Semantic validation (beyond JSON Schema) in `src/config/config.cpp`, each rule unit-tested: unique labels/uids/ports names; one port pair; redundant port required when any group has `redundancy`; PCI address format and existence (at runtime, not in import); IPs in the configured subnet; `block_us` multiple of packet time; video size 1920×1080 or 3840×2160 (§6.1); interlace only with 1080 lines and 25/1 or 30000/1001; channel count vs ptime limits; multicast addresses in 224.0.0.0/4; no two egress legs with identical destination; `output_delay_ns` (when set) ≥ one grain + the largest effective read offset of the group's essences + 2 ms (§5.7); in a group with video the effective output delay ≤ that read offset + 5 grains (§5.7); `read_offset_grains` / `read_offset_ns` (and the `mxl.default_read_offset_*` pair) are mutually exclusive, ≥ 0 and only allowed on egress essences; domain references exist; configured domain paths are not mirror domains (basename `mirror-*`); `mxl.scan_path` is absolute or `null`; `nic.backend = kernel` ports have `ifname`, `dpdk` ports have `pci`; `public_port` 1–65535 when set; lcores parse and are disjoint from `app_cpus`; `host_address`, `st2110.host_address`, `public_address` and `management_addresses` are announceable IPv4 literals; `host_address` and the deprecated `public_address` agree when both are set; the ST 2110 node port differs from `http_port` and `web_port` and is ≤ 65535; registry `mode: "static"` needs an `address` and contradicts `dns_sd: true`; registry port + 1 ≤ 65535 unless `query_port` is set.

---

## 10. HTTP Surface

Ports: the MXL node's `node.http_port` (`NMOS_PORT`, default 8080) carries its NMOS APIs and the gateway routes below; `node.web_port` (`WEB_PORT`, optional) moves the gateway routes to their own listener; the ST 2110 node's `node.st2110.http_port` (default `NMOS_PORT + 1`) carries only its NMOS APIs.

| Path | Owner | Purpose |
|---|---|---|
| `/x-nmos/node/…`, `/x-nmos/connection/…` | nmos-cpp | IS-04 Node API, IS-05 Connection API of each node, on its own port (`/` stays nmos-cpp's base listing — do not override) |
| `/admin/` | gateway | Admin web UI (single embedded HTML file) |
| `/api/…` | gateway | JSON REST API used by the UI (§11.3) |
| `/metrics` | gateway | Prometheus text exposition format 0.0.4 |
| `/livez` | gateway | 200 when the process and HTTP server are alive |
| `/readyz` | gateway | 200 only when: config valid, MTL up, PTP locked (unless `require_lock=false`), clock offset ≤ `max_offset_ns`, all configured domains ok, and each NMOS node with a configured registry (DNS-SD or address) registered (reasons `nmos_not_registered`, `st2110_nmos_not_registered`); `shutting_down` during the shutdown; else 503 with JSON reasons. Discovered/mirror domains and essences in `waiting_for_flow` or `no_signal` (§5.8) do not affect readiness |
| `/statusz` | gateway | human-readable plain-text status (mxl-decklink parity) |

No authentication (§1.3). All mutating `/api` routes require `Content-Type: application/json` and reject cross-origin requests (check `Origin` against `Host`).

The two node ports (and `node.web_port` if set) are the only TCP ports the gateway opens on the host. nmos-cpp's IS-07 Events WebSocket, the Settings and Logging APIs and the other optional APIs are disabled (§7.1), so that the gateway can share host networking with mxl-decklink and mxl-fabrics-agent (port table §15.4).

---

## 11. Admin Web UI

### 11.1 Technology

Vue 3 + Vite, built to a single HTML file embedded into the binary at build time (mxl-decklink `web/` pattern). No external assets, works offline. Live data via polling `/api/status` (1 s). Light/dark theme following the browser.

### 11.2 Tabs

- **Dashboard** — node label/id, version (gateway, MTL, DPDK, MXL, nmos-cpp), readiness with reasons, PTP lock + GM identity, MTL−host-TAI offset, NIC link state, per-group tiles with per-essence state (including `waiting_for_flow` and `no_signal`, shown as neutral/amber, not red), bitrate, frame/packet counters, 2022-7 leg health (green/amber/red from `pkts_recv[P/R]` vs `pkts_total`); egress tiles show the resolved MXL domain per essence and mark mirror domains.
- **Groups** — list of groups; *Create group* dialog: label, direction (ingest/egress), MXL domain, redundancy, and **counts per type** (Video ×n, Audio ×m, ANC ×k as steppers). Creating generates the essences with defaults (format from a node-wide default profile, e.g. 1080p50 / 8 ch L24 1 ms); each essence is then editable (format, payload type, default legs, labels). Editing/removing a group shows which NMOS resources will be re-registered or removed and whether active connections will be interrupted. Duplicate group (new uids).
- **NMOS** — node/device ids, registry discovered/used, registration state and last heartbeat, table of all Senders/Receivers (id, label, transport, group hint, master_enable, active transport params, active SDP for 2110 senders), links to the raw `/x-nmos` resources.
- **Network** — port pair configuration form (PCI, IP, netmask, gateway, port names), detected card model, bind mode (PF/VF), MAC, link speed/state, DDP package version, per-port RX/TX counters and errors. Changes are `restart_required`.
- **PTP** — mode, domain, lock state, grandmaster identity, GM priority/clock class/accuracy, steps removed, parent port identity, UTC offset, last/min/max offset and path delay (sparklines, last 5 min held in memory), GM change counter, PHC2SYS state, MTL−host-TAI offset with thresholds.
- **MXL** — configured domains (path, id from `domain_def.json`, label, tmpfs check, usage), discovered and mirror domains under `mxl.scan_path` (kind, path, id, label, `source_host_id` of mirrors, conflicts), flow browser per domain (id, label, media type, active, head index, last write) — mxl-decklink §7.6 behaviour, read-only. Per egress MXL Receiver: resolved domain (id, path, kind), state, flow-not-found retries, read lag and read offset.
- **Configuration** — export/download, import/upload (with validation report before writing), raw JSON view (read-only), "changed on disk" diff and resolution, restart button, preflight report (§14.3).

All forms are validated client-side for UX and server-side authoritatively (same rule set as §9.5); errors are shown per field (JSON pointer mapping). Every setting shows its provenance (default / file / environment); environment-set settings are read-only with the variable name shown (§9.1).

### 11.3 REST API

Every endpoint also answers under `/api/v1/…` (`/api/v1/status`, `/api/v1/config/export`, …), the versioned name of the v1 contract.

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status` | GET | everything the dashboard needs (one call) |
| `/api/config` | GET | full config (file content) + effective config + per-key provenance (`default` / `file` / `env:<VAR>`) + `ETag` |
| `/api/config` | PUT | full replace, requires `If-Match` (412 on conflict); returns `restart_required` and per-group apply results |
| `/api/groups` | POST | create group (body: label, direction, domain, redundancy, counts or full essences) |
| `/api/groups/{uid}` | PUT / DELETE | edit / delete group (live apply, §9.3) |
| `/api/config/export` | GET | download file |
| `/api/config/import` | POST | upload file (`?keep_ids=true`), never live; `?restart=true` restarts gracefully after writing |
| `/api/schema` | GET | JSON Schema |
| `/api/nic` `/api/ptp` `/api/domains` `/api/flows?domain=` `/api/nmos` | GET | tab data. `/api/domains` lists configured, discovered and mirror domains with `kind`; `/api/flows?domain=` takes a domain id (`domain_def.json`), not a path; `/api/nmos` has the MXL node's fields at the top level, `nodes` (both nodes: id, label, href, registry, registration) and the Senders/Receivers of both nodes with `node` |
| `/api/preflight` | GET | preflight results |
| `/api/restart` | POST | graceful exit for supervisor restart |

---

## 12. Metrics and Grafana

### 12.1 Prometheus Metrics (prefix `mxl_st2110_gateway_`)

Implement a minimal registry like mxl-decklink `src/ops/metrics.*` (no external Prometheus library required). Counters end in `_total`. Common essence labels: `group`, `essence`, `uid`, `type` (`video|audio|anc`), `direction` (`ingest|egress`).

| Metric | Type | Labels |
|---|---|---|
| `mxl_st2110_gateway_build_info` | gauge=1 | `version, mtl, dpdk, mxl, nmos_cpp` |
| `mxl_st2110_gateway_ready` | gauge | — |
| `mxl_st2110_gateway_restart_required` | gauge | — |
| `mxl_st2110_gateway_ptp_locked` | gauge | `port` (`p\|r`; both ports run PTP when redundant, §4.3) |
| `mxl_st2110_gateway_ptp_selected` | gauge | `port` (1 for the port whose PTP instance steers the PHC, chosen by the dual-port BMCA, §5.5) |
| `mxl_st2110_gateway_ptp_selection_changes_total` | counter | — |
| `mxl_st2110_gateway_ptp_info` | gauge=1 | `port, gm_identity, parent_port_identity, domain, bind_mode` |
| `mxl_st2110_gateway_ptp_offset_ns` / `_path_delay_ns` | gauge | `port` (last value; min/max over 60 s as `stat="min|max"`) |
| `mxl_st2110_gateway_ptp_utc_offset_seconds` | gauge | `port` |
| `mxl_st2110_gateway_ptp_gm_changes_total`, `mxl_st2110_gateway_ptp_sync_total`, `mxl_st2110_gateway_ptp_errors_total` | counter | `port` (+`kind` for errors) |
| `mxl_st2110_gateway_clock_mtl_minus_host_tai_ns` | gauge | — |
| `mxl_st2110_gateway_nic_link_up`, `mxl_st2110_gateway_nic_link_speed_mbps` | gauge | `port` (DPDK `rte_eth_link_get_nowait`, kernel backend: sysfs) |
| `mxl_st2110_gateway_nic_info` | gauge=1 | `port, mac, pci, driver, ddp_package` (DDP version parsed from the ice PMD's "Active package is" log line) |
| `mxl_st2110_gateway_nic_rx_packets_total`, `_tx_packets_total`, `_rx_bytes_total`, `_tx_bytes_total`, `_rx_errors_total`, `_rx_missed_total` | counter | `port` (VERIFIED: MTL `mtl_get_port_stats` → `struct mtl_port_status` `rx_packets`, `tx_packets`, `rx_bytes`, `tx_bytes`, `rx_err_packets`, `rx_hw_dropped_packets`) |
| `mxl_st2110_gateway_essence_state` | gauge (1 for current) | essence labels + `state` (`idle\|waiting_for_flow\|no_signal\|running\|degraded\|error`) |
| `mxl_st2110_gateway_rx_frames_total` | counter | essence + `result` (`complete|incomplete|dropped`) |
| `mxl_st2110_gateway_rx_leg_packets_total` | counter | essence + `leg` (`p|r`) |
| `mxl_st2110_gateway_rx_packets_total` | counter | essence (after 2022-7 merge) |
| `mxl_st2110_gateway_rx_leg_seq_lost_total` | counter | essence + `leg` |
| `mxl_st2110_gateway_ingest_origin_age_ns` | gauge | essence |
| `mxl_st2110_gateway_mxl_grains_written_total`, `mxl_st2110_gateway_mxl_samples_written_total`, `mxl_st2110_gateway_mxl_write_errors_total` | counter | essence |
| `mxl_st2110_gateway_mxl_grains_read_total`, `mxl_st2110_gateway_mxl_read_timeouts_total` | counter | essence (`read_timeouts` = grains/blocks with no data by their deadline, §5.7) |
| `mxl_st2110_gateway_mxl_late_reads_total` | counter | essence (data that became available only after its deadline and was skipped) |
| `mxl_st2110_gateway_mxl_grains_invalid_total` | counter | essence (grains read with `MXL_GRAIN_FLAG_INVALID`, i.e. the writer marked them as having no valid data) |
| `mxl_st2110_gateway_mxl_flow_not_found_total` | counter | essence (reader attempts that found no flow or no domain, §5.8) |
| `mxl_st2110_gateway_mxl_read_lag_grains` | gauge | essence (writer head index − read index; for audio converted to grains of the group's video rate, or to audio blocks in audio-only groups) |
| `mxl_st2110_gateway_mxl_reader_info` | gauge=1 | essence + `domain_id, domain_path, domain_kind` (`configured\|discovered\|mirror`), `flow_id` — resolved domain of every enabled MXL Receiver; the series is replaced when the resolution changes |
| `mxl_st2110_gateway_mxl_discovered_domains` | gauge | `kind` (`discovered\|mirror\|conflict`) (§8.5) |
| `mxl_st2110_gateway_tx_frames_total`, `mxl_st2110_gateway_tx_late_frames_total` | counter | essence |
| `mxl_st2110_gateway_egress_lead_ns` | gauge | essence (time from grain available to TX deadline; negative = late) |
| `mxl_st2110_gateway_nmos_registered` | gauge | `node` (`mxl`, `st2110`; one series per running node) |
| `mxl_st2110_gateway_nmos_activations_total` | counter | `kind` (`sender|receiver`), `transport`, `result` |
| `mxl_st2110_gateway_mxl_domain_bytes` | gauge | `domain, kind` (`used|free`) |
| `mxl_st2110_gateway_mxl_domain_flows` | gauge | `domain` |

Metric names and labels are a public interface: document them in `docs/metrics.md` and keep them stable across minor versions.

### 12.2 Grafana Dashboard

- Generated by `monitoring/tools/gen_dashboard.py` (deterministic output, no network access) into `monitoring/grafana/mxl-st2110-gateway.json`; CI fails if the committed file differs from a fresh generation.
- Grafana ≥ 11 JSON model; template variables: `datasource` (Prometheus), `instance`, `group`, `essence`.
- Rows: *Overview* (ready, restart required, PTP locked, GM identity as table, clock offset), *PTP* (offset, path delay, GM changes, sync rate, lock and BMCA selection per port), *NIC* (link, throughput, errors/missed per port), *Ingest* (frames by result, leg packet loss P vs R, origin age), *Egress* (late frames, lead time, read timeouts; per MXL Receiver: read lag in grains, flow-not-found retry rate, no-data / invalid / late reads, essence state incl. `waiting_for_flow` and `no_signal`, and a table of resolved domains from `mxl_st2110_gateway_mxl_reader_info` with mirror domains highlighted), *NMOS* (registered, activations), *MXL domains* (usage, flow counts, discovered/mirror/conflict domain counts).
- Also ship `monitoring/prometheus/scrape-example.yaml` and a Kubernetes `ServiceMonitor` example (`deploy/k8s/servicemonitor.yaml`, optional).

---

## 13. Logging

Structured JSON lines to stdout (one object per line: `ts`, `level`, `event`, fields), human-readable text with `MXLGW_LOG_FORMAT=text`. Stable `event` identifiers (e.g. `mxl_domain_not_tmpfs`, `ptp_gm_changed`, `nmos_activation`, `essence_state`; for §8.5/§5.8: `mxl_domain_is_mirror`, `mxl_domain_discovered`, `mxl_domain_removed`, `mxl_domain_skipped`, `mxl_domain_conflict`, `mxl_flow_not_found` (rate-limited: first occurrence, then at most every 30 s per essence), `flow_def_mismatch`). MTL and DPDK log output is redirected into the same stream with `component=mtl` (VERIFIED API: `mtl_set_log_printer` — process-global — `mtl_set_log_level`, `mtl_openlog_stream` for DPDK; the printer only enqueues into a lock-free ring drained by the logging thread, because MTL logs from lcores). Last 500 lines kept in memory for the UI.

---

## 14. Container

### 14.1 Dockerfile (multi-stage, `docker/Dockerfile`)

1. `webui` — `node:22-bookworm`, `npm ci && npm run build`.
2. `deps` — `ubuntu:24.04`: toolchain, vcpkg; DPDK 26.07 with the MTL patches, using the same steps as MTL `script/build_dpdk.sh` but with `-Dplatform=generic` (the script builds for `-march=native` of the build host, which would make the image depend on the CI runner's CPU; MTL itself uses runtime SIMD dispatch); MTL v26.09 + `patches/mtl/*.patch` (`git apply --check` first); MXL v1.1.0 (vcpkg, `-DMXL_ENABLE_FABRICS_OFI=OFF`, `-DBUILD_TOOLS=ON` for `mxl-info` / `mxl-data-probe`; same `MXL_REF` as mxl-decklink; no libfabric or rdma-core in the image — the gateway does not use the Fabrics API even when mxl-fabrics-agent replicates its flows); nmos-cpp at the pinned commit, built once as C++17 and installed as a CMake package. This stage changes only when pins or `patches/mtl` change → maximal cache hits.
3. `build` — compile the gateway, run unit tests (`ctest --output-on-failure`); failing tests fail the image build.
4. `runtime` — `ubuntu:24.04` with runtime libraries only, the binary, MTL/DPDK/MXL shared libs, `mxl-info`, `mxl-data-probe`, the JSON schema, example configs, and the **E810 DDP package** (from the pinned `ice` driver tarball, `versions.env` `ICE_VER`/`ICE_DMID`) installed where the DPDK ice PMD looks for it (`/lib/firmware/updates/intel/ice/ddp/ice.pkg` and `/lib/firmware/intel/ice/ddp/ice.pkg`; VERIFIED in DPDK 26.07 `drivers/net/intel/ice/ice_ethdev.h` / `ice_load_pkg`: devarg `ddp_pkg_file` first, then a custom path from the host's `/sys/module/firmware_class/parameters/path`, then `ice-<DSN>.pkg` and `ice.pkg` in `updates/` and the default directory; without a package the PMD refuses to start unless `safe-mode-support=1`). The PMD runs in user space inside the container, so the package must be in the **container** filesystem; a host `/lib/firmware` mount MAY override it.

Build args (one place, mirrored in CI): `MTL_REF`, `DPDK_VER`, `MXL_REF`, `MXL_REVISION` (the commit of `MXL_REF`, checked against the clone), `NMOS_CPP_REF`, `ICE_VER`, `ICE_DMID`; per build `MXLGW_VERSION` and `VCS_REF`. Labels: `org.opencontainers.image.title`, `.description`, `.source` (repository URL), `.revision` (`VCS_REF`), `.version`, `.licenses` (`MIT`), `io.dmf.mxl.revision` (= `MXL_REVISION`) and the pins as `io.github.mxlgw.*`. The runtime image runs as uid/gid 1000 (`USER 1000:1000`, `/config` owned by it). Image is `linux/amd64` only.

### 14.2 Runtime Requirements

| Requirement | Why |
|---|---|
| `/dev/vfio` device (`/dev/vfio/vfio` + group nodes) | DPDK access to the PFs/VFs |
| hugepages mounted at `/dev/hugepages` (hugetlbfs) | DPDK memory |
| `CAP_IPC_LOCK` | lock/pin DMA memory (also lifts `RLIMIT_MEMLOCK` for vfio pinning) |
| `CAP_SYS_NICE` | NUMA policy, RT priorities |
| `CAP_SYS_TIME` | **only** for `ptp.mode = builtin_phc2sys` |
| unlimited `memlock` ulimit | belt and braces for Docker |
| `/config` volume (read-write) | config + `state/` |
| host MXL root (tmpfs, e.g. `/Volumes/mxl`) mounted read-write at `mxl.scan_path` (default `/Volumes/mxl`), containing the configured domains | §8; mounting the whole root lets egress receivers discover sibling and mirror domains (§8.5). Mounting only individual domains remains possible, but then nothing is discovered |
| management network | NMOS (both nodes) + UI; host network for the DPDK NIC; DNS-SD only with `node.registry.dns_sd` |
| — (not required) | no RDMA devices, libfabric or Fabrics capabilities: replication is done by mxl-fabrics-agent in its own container (§8.6) |

**No `privileged: true`.** The image runs as uid/gid 1000 (platform guideline G11). With the dpdk backend the container runs as root with gid 1000 (`user: "0:1000"`, Kubernetes `runAsUser: 0`, `runAsGroup: 1000`) because DPDK opens the root-owned VFIO group device nodes; a non-root DPDK mode is documented in the README (VFIO device ownership, writable hugetlbfs, unlimited memlock; untested on hardware). The process sets `umask 002`, and the files it creates in domains use mode `0664`/`0775`, so other media functions of the same group can read and write them.

### 14.3 Preflight (`mxl-st2110-gateway --preflight`, also run at startup and served at `/api/preflight`)

Checks with actionable messages (each with a README anchor): hugepages mounted and free pages sufficient for the configured sessions (estimate per session); `/dev/vfio/vfio` present; each configured PCI address exists in `/sys/bus/pci/devices`, is bound to `vfio-pci`, has an IOMMU group whose node exists in `/dev/vfio`; device is an E810/E830 (vendor 0x8086, warn otherwise); capabilities present (`CAP_IPC_LOCK`, `CAP_SYS_NICE`, `CAP_SYS_TIME` if needed); MXL domains tmpfs and not mirror domains (§8.3); `mxl.scan_path` exists and is readable, with a listing of discovered/mirror domains and conflicts (informational, §8.5); kernel TAI offset (`adjtimex`) consistent with the PTP mode — a zero offset outside `builtin_phc2sys` is a warning that names the multi-host requirement of §5.1; the node ports and `node.web_port` free (warning: the listener then exits 75) and `node.http_port` not one of the sibling defaults of §15.4 (warning); lcores exist and are not shared with `app_cpus`. Startup aborts with exit code 78 on any hard failure; soft failures are warnings in the UI.

### 14.4 Signals and Exit Codes

Exit codes: `0` = normal / restart requested (`/api/restart`, import with `restart=true`); `1` = runtime failure; `75` (`EX_TEMPFAIL`) = a listening port cannot be bound (the node ports, `node.web_port`; checked after opening because nmos-cpp swallows listener errors); `78` (`EX_CONFIG`) = configuration or environment error (do not restart-loop silently: log the reason every time); `143` / `130` = terminated by SIGTERM / SIGINT after the graceful shutdown.

SIGTERM/SIGINT → graceful shutdown bounded by `node.shutdown_timeout_s` (`SHUTDOWN_TIMEOUT_S`, default 10 s; a watchdog exits after it): `/readyz` reports `shutting_down`; the control thread stops; every group is removed (MTL sessions stopped, FlowWriters/Readers released, §8.4); every NMOS resource of both nodes is erased, children first and the node last, so nmos-cpp's registration thread sends the DELETEs (waiting at most min(3 s, timeout/2) for the node's DELETE; log `nmos_deregistered`); the listeners close; MXL instances and MTL are released; with `mxl.cleanup_on_exit` the configured domains are removed (§8.4); exit 143 (130 for SIGINT). Nothing else is waited for (no child processes).

---

## 15. Deployment

### 15.1 Docker Compose (`docker/docker-compose.yaml`, also in README)

```yaml
services:
  mxl-st2110-gateway:
    image: ghcr.io/<owner>/mxl-st2110-gateway:1.0.0  # or :nightly-dev
    container_name: mxl-st2110-gateway
    restart: unless-stopped
    init: true
    network_mode: host                # media ports are DPDK-owned; NMOS announces the host's address
    stop_grace_period: 15s            # > SHUTDOWN_TIMEOUT_S
    user: "0:1000"                    # root for VFIO, group 1000 shared with the other media functions
    ulimits:
      memlock: { soft: -1, hard: -1 }
    cap_add: [IPC_LOCK, SYS_NICE]     # + SYS_TIME only for ptp.mode=builtin_phc2sys
    devices:
      - /dev/vfio:/dev/vfio
    volumes:
      - ./config:/config                       # gateway.json + state/
      - /dev/hugepages:/dev/hugepages
      # Host MXL root, shared with other media functions and mxl-fabrics-agent on this host:
      # /Volumes/mxl is a host tmpfs (see README "Host preparation"). The gateway writes only to its
      # configured domains (e.g. /Volumes/mxl/main) and discovers sibling and mirror-<id> domains (§8.5).
      # The container path may differ per container; identity comes from domain_def.json.
      - type: bind
        source: /Volumes/mxl
        target: /Volumes/mxl
    environment:
      NMOS_SEED: gw-studio1           # every NMOS id and the output domain id (§7.3)
      NMOS_LABEL: GW-STUDIO1
      NMOS_REGISTRY_ADDRESS: "10.10.0.5"   # static MXL registry; no DNS-SD
      NMOS_REGISTRY_PORT: "3210"
      NMOS_PORT: "8080"               # e.g. 8090 when mxl-decklink already uses 8080 on this host (§15.4)
      MXL_DOMAIN_SCAN_PATH: /Volumes/mxl
      MXL_OUTPUT_DOMAIN_DIR: /Volumes/mxl/gw-studio1
      SHUTDOWN_TIMEOUT_S: "10"
    healthcheck:
      test: ["CMD-SHELL", "curl -fsS http://127.0.0.1:$${WEB_PORT:-$${NMOS_PORT:-8080}}/livez"]
      interval: 10s
      timeout: 3s
      start_period: 60s
```

README explains: why host networking, how to create the host tmpfs (`/etc/fstab`: `tmpfs /Volumes/mxl tmpfs size=8g,mode=1777 0 0`, the CBC `mxl-hands-on` convention also used by mxl-decklink and mxl-fabrics-agent), an alternative compose `tmpfs:` volume when the domain is only shared inside one compose project (other services mount the same named tmpfs volume), how other MXL media functions mount the same domain (read-only for readers is allowed by MXL), that all containers of a host (gateway, mxl-decklink, mxl-fabrics-agent) MUST mount the **same** host MXL root — each at its own container path (owner decision C7) — and the port table of §15.4.

**Optional multi-host scenario with mxl-fabrics-agent** (`docker/docker-compose.fabrics.yaml`, one file per real host, README walkthrough):

- *Host A:* the gateway with an **ingest** group (ST 2110 → MXL Sender, config `config/examples/gateway.fabrics-host-a.json`, configured domain `/Volumes/mxl/main` with a fixed `id`), plus mxl-fabrics-agent with `MXL_ROOT` = the same host root.
- *Host B:* mxl-fabrics-agent plus an MXL Receiver — either the gateway with an **egress** group (`gateway.fabrics-host-b.json`, MXL → ST 2110) or mxl-decklink with an output channel.
- Shared: one NMOS registry (both hosts register; the agents discover each other via their NMOS Nodes or a static `PEERS` map), TAI-disciplined clocks on both hosts (§5.1), non-colliding ports (§15.4).
- Demo steps: activate the host-A gateway's ingest receivers and MXL Senders; then, with `curl` or a controller, PATCH the host-B MXL Receiver with host A's `mxl_domain_id` and `mxl_flow_id`. The host-B agent creates (eager mode: has already created) `mirror-<domain-id>` and replicates; the host-B receiver resolves the mirror domain by id (§8.5), waits in `waiting_for_flow` / `no_signal` until grains arrive (§5.8) and starts without further action. Show `mxl_st2110_gateway_mxl_reader_info{domain_kind="mirror"}` and `mxl_st2110_gateway_mxl_read_lag_grains`, and recommend a read offset of a few grains on host B (§5.7).

### 15.2 Kubernetes (`deploy/k8s/`, first-class, also in README)

- **Device allocation:** SR-IOV Network Device Plugin (supports PFs, `drivers: ["vfio-pci"]`, `pciAddresses` selector). **One resource per port** so primary/redundant stay deterministic, e.g. `intel.com/e810_media_p` and `intel.com/e810_media_r`. The plugin injects `PCIDEVICE_INTEL_COM_E810_MEDIA_P=0000:31:00.0` (+ `_INFO` with the vfio mounts). The config accepts `"pci": "env:PCIDEVICE_INTEL_COM_E810_MEDIA_P"` for this. Ship `sriov-dp-configmap.yaml` as an example. PFs must be bound to `vfio-pci` on the node beforehand (README: `driverctl set-override`).
- **Pod:** `Deployment`, `replicas: 1`, `strategy: Recreate`, `nodeSelector`/affinity to the node with the card, `hostNetwork: true` (default; alternative: pod network + `Service` for `NMOS_PORT` and `NMOS_PORT + 1` + `NMOS_HOST_ADDRESS`/`public_port`), `dnsPolicy: ClusterFirstWithHostNet`, `terminationGracePeriodSeconds: 15` (> `SHUTDOWN_TIMEOUT_S`), no `hostIPC`. The example sets the platform's standard variables (`NMOS_SEED`, `NMOS_LABEL`, `NMOS_TAGS`, `NMOS_HOST_ADDRESS` from `status.hostIP`, `NMOS_REGISTRY_ADDRESS`/`_PORT`, `NMOS_DNS_SD=false`, `NMOS_PORT`, `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_CLEANUP_ON_EXIT`, `SHUTDOWN_TIMEOUT_S`); any other setting MAY come from the pod spec too (§9.1), e.g. `MXLGW_NIC_PRIMARY_PCI`.
- **Resources:** Guaranteed QoS — requests = limits, integer CPUs (static CPU manager policy recommended for lcore pinning; with `nic.lcores` unset the MTL lcores and worker CPUs come from the pod's cpuset), `hugepages-1Gi` (e.g. `4Gi`), `memory`, the two device resources.
- **Security:** `runAsUser: 0`, `runAsGroup: 1000`, `supplementalGroups: [1000]`, `fsGroup: 1000` (§14.2), `capabilities.add: [IPC_LOCK, SYS_NICE]` (+ `SYS_TIME` only for `builtin_phc2sys`), `privileged: false`, `allowPrivilegeEscalation: false`. `IPC_LOCK` covers memlock (no runtime ulimit change needed — **VERIFY** on the target containerd). No D-Bus/Avahi mounts and no AppArmor override (only needed with DNS-SD).
- **Volumes:** hugepages `emptyDir: {medium: HugePages-1Gi}` at `/dev/hugepages`; config on a **PersistentVolumeClaim** at `/config` (the gateway writes its config — a ConfigMap is read-only, so a ConfigMap MAY only seed the file via an `initContainer` that copies it if absent); MXL domain:
  - **default:** `hostPath` to the node's MXL root tmpfs (`/Volumes/mxl` mounted at `/Volumes/mxl`, `type: Directory`; the configured domain `/Volumes/mxl/main` is created by the bootstrap of §8.3) so media functions and the mxl-fabrics-agent DaemonSet in **other pods on the node** share the domains and egress receivers discover mirror domains (§8.5);
  - **alternative:** `emptyDir: {medium: Memory, sizeLimit: …}` when all MXL consumers are containers of the **same pod** (emptyDir is not shared across pods, so this alternative cannot be combined with mxl-fabrics-agent).
- **Probes:** `startupProbe` `/livez` (failureThreshold covering ≥ 120 s DPDK init), `livenessProbe` `/livez`, `readinessProbe` `/readyz`.
- **PTP:** the media port runs MTL built-in PTP inside the pod; the node's `CLOCK_TAI` must be disciplined by the cluster (e.g. linuxptp DaemonSet / PTP operator) for `ptp.mode=builtin`. With mxl-fabrics-agent, every node that writes, replicates or reads flows MUST be TAI-disciplined (§5.1).
- **Optional multi-host scenario with mxl-fabrics-agent** (`deploy/k8s/fabrics/`, kustomize overlay + README): the agent runs as a DaemonSet from its own repository's manifests (hostNetwork, hostPath of the same MXL root); the gateway with an ingest group is pinned to node A; an MXL Receiver — the gateway with an egress group (its own Deployment, pinned to node B, with its own NIC resources) or mxl-decklink — runs on node B. Same demo steps and checks as §15.1. The overlay MUST pass `kubeconform`.
- Files: `namespace.yaml`, `sriov-dp-configmap.yaml`, `pvc.yaml`, `configmap-seed.yaml`, `deployment.yaml`, `service.yaml`, `servicemonitor.yaml` (optional), `kustomization.yaml`, `README.md`, `fabrics/` (optional overlay). All MUST pass `kubeconform` in CI.

### 15.3 Host Preparation (README section, both deployment styles)

BIOS: VT-d on, SR-IOV on (if VFs), C-states limited; kernel cmdline `intel_iommu=on iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=<n>` (or 2M pages); bind media PFs to `vfio-pci` persistently; E810 NVM/firmware as recommended by MTL; for VF mode the patched host `ice` driver (MTL `doc/e800_series_drivers.md`); recommended `isolcpus`/`nohz_full`/`rcu_nocbs` for MTL lcores and irqbalance exclusion; host time sync for `CLOCK_TAI` (§5.2) with correct TAI offset (`ptp4l -f … ` + `phc2sys -a -r` or chrony with `leapsectz right/UTC`) — on **every** host that writes, replicates or reads MXL flows, including hosts that only run receivers, because grain indices are TAI-based and replicated 1:1 by mxl-fabrics-agent (§5.1); one host tmpfs as MXL root for all MXL domains of the host, mounted by every MXL container of that host at its configured root path (gateway: `mxl.scan_path`, mxl-fabrics-agent: `MXL_ROOT`). Works on Ubuntu 24.04 and Debian 13 hosts (container userland is independent of the host distro; host kernel needs VFIO).

### 15.4 Port Usage and Co-location (README port table)

The README MUST contain this table. Under host networking all containers of a host share one port space.

| Container | Default port(s) | Protocol | Purpose |
|---|---|---|---|
| mxl-st2110-gateway | `8080` (`NMOS_PORT` = `node.http_port`) | TCP (HTTP/HTTPS) | MXL node: IS-04 Node API, IS-05 Connection API; admin UI, `/api`, `/metrics`, health unless `WEB_PORT` is set (§10) |
| mxl-st2110-gateway | `8081` (`node.st2110.http_port`, default `NMOS_PORT + 1`) | TCP (HTTP/HTTPS) | ST 2110 node: IS-04 Node API, IS-05 Connection API |
| mxl-st2110-gateway | none (`WEB_PORT` = `node.web_port`, optional) | TCP | admin UI, `/api`, `/metrics`, health on their own listener |
| mxl-st2110-gateway | none | — | no WebSocket: IS-07 Events, nmos-cpp Settings/Logging APIs and other optional APIs are disabled (§7.1) |
| mxl-st2110-gateway | none on the host stack | — | ST 2110 media and PTP run on DPDK-owned ports; only the test-only kernel backend (§17.2) uses UDP ports of the configured legs on its test interfaces |
| mxl-st2110-gateway | `5353/udp` via the host's Avahi, only with `node.registry.dns_sd` | mDNS | DNS-SD (registry discovery), shared host daemon |
| mxl-decklink | `8080` | TCP | web UI, REST, health, metrics |
| mxl-decklink | `3212`, `3213` | TCP | NMOS Node/Connection API, IS-07 WebSocket |
| mxl-fabrics-agent | `8095` | TCP | UI, REST/control API, health, metrics |
| mxl-fabrics-agent | `3232`, `3233` | TCP | NMOS Node API, WebSocket |
| mxl-fabrics-agent | `23500`–`23599` | TCP / RDMA CM | fabric data ports (target pool) |

**Collision with mxl-decklink (`8080`) — resolved at deployment level (owner decision C1):** the gateway default stays `8080`. When both run on one host, either (a) set `NMOS_PORT` (e.g. `8090`; `8090` and `8091` are free in this table) under host networking, (b) run the gateway in a bridge network with a Compose port mapping and set `node.public_port` / `NMOS_HOST_ADDRESS` so NMOS hrefs advertise the mapped port, or (c) put a reverse proxy in front and set `NMOS_HOST_ADDRESS` / `node.public_port` to the proxy (§7.1). Two gateways on one host use `NMOS_PORT`s at least two apart. DNS-SD registry discovery (optional) needs host networking. The preflight warns about a port that is in use or equals a sibling default (§14.3). Prometheus scrape examples and health checks use the configured port.

---

## 16. CI/CD (GitHub Actions, GHCR)

### 16.1 `ci.yaml` (pull requests and pushes)

- Build the `build` stage (compiles + unit tests) with GHA cache.
- Lint: compiler warnings as errors; `clang-format --dry-run --Werror`.
- Check `patches/mtl/*.patch` applies to `MTL_REF`.
- Validate `config/examples/*.json` (including the `fabrics-host-*` examples) against the schema; regenerate the Grafana dashboard and diff; `kubeconform` on `deploy/k8s` (including the `fabrics/` overlay); `docker compose config` on both compose files.
- Integration job (§17.3): container smoke test, platform lifecycle test, kernel-socket loopback media test, late-flow receiver test and AMWA nmos-testing suites.

### 16.2 `container.yaml` (build and push to `ghcr.io/${{ github.repository }}`)

Same structure as mxl-decklink `.github/workflows/container.yaml`:

- Triggers: push to `main`, tags `v*.*.*`, `workflow_dispatch`, and a nightly `schedule` (rebuild of `main`, picks up base-image security updates).
- `docker/metadata-action` tags:
  - release tag `v1.2.3` → `1.2.3`, `1.2`, `1` **and** `latest`;
  - push to `main` / nightly / manual → **`nightly-dev`** (the "dev latest" tag, always the newest successful `main` build);
  - every push to `main` → `git-<sha7>`, written once: release, nightly and manual rebuilds of the same commit do not write it again.
  - `1.2.3` and `git-<sha7>` are never moved: a release build fails if its `1.2.3` tag already exists; examples reference released tags only.
  - `flavor: latest=${{ startsWith(github.ref, 'refs/tags/') }}` so `latest` is never a dev build.
- `docker/build-push-action` with `cache-from/to: type=gha,mode=max`, `provenance: true`, `sbom: true`, build args `MXLGW_VERSION`, `VCS_REF`. The package `ghcr.io/leeo86/mxl-st2110-gateway` is public.
- Permissions: `contents: read`, `packages: write`.

### 16.3 Releases

- On a `v*.*.*` tag, after the image is pushed, a `release` job (`needs: build-and-push`, `contents: write`) creates the **GitHub Release** for that tag with notes from the matching `CHANGELOG.md` section, and attaches: `gateway-config.schema.json`, the Grafana dashboard JSON, `docker-compose.yaml`, a `k8s-manifests-<version>.tar.gz`, and the image digest. The image reference with digest is printed in the release notes.
- Version source of truth: the git tag; the binary embeds it (`--version`, `mxl_st2110_gateway_build_info`, `/api/status`); untagged builds report `0.0.0-dev+<sha>`.

---

## 17. Testing Strategy

### 17.1 Unit Tests (doctest, no hardware, run in the image build)

Config schema + semantic rules; ID derivation (§7.3); `rtpclock` (§5.4); `audioconv` (§6.2); `anc8331` (§6.3); `sdp_map` (SDP fixtures for 1080p50, 1080i50, 2160p50, 8 ch L24 1 ms, 16 ch L24 125 µs, ANC, with and without DUP groups, malformed SDPs); configuration precedence (environment > file > default, standard names and aliases, alias conflicts, type errors, env-set keys never written back); seed-derived ids, host-address validation and detection, registry/DNS-SD mapping, port rules, CPU placement from the affinity, shutdown/cleanup (`app-tests`); BCP-007-03 constraint/auto resolution helpers (including resolution into discovered and mirror domains, re-scan before resolving, unknown domain accepted and waited for); PTP data-set comparison and dual-port selection (§5.5); egress replacement frames (black, repeat); domain bootstrap against a temporary tmpfs (skip with message if not mountable) and a non-tmpfs dir (must fail), a `mirror-*` path and a domain with the `x-mxl-fabrics-agent` marker (must fail), id write-back; domain discovery (§8.5): identity from `domain_def.json` and never from the directory name, unknown fields ignored, mirror classification, duplicate-id conflict, no negative caching (a domain created after a failed lookup is found by the next lookup); reader retry schedule (§5.8: backoff 500 ms → 5 s, continues while `master_enable`, stops on disable); read-offset validation against `output_delay_ns` (§9.5); new-flow-UUID NMOS update sequence (§7.3); config store atomic write/backup/ETag.

### 17.2 Hardware Abstraction

All MTL calls go through thin interfaces (`src/mtl/*.hpp`) so that unit tests and the mock can run without DPDK. A `nic.backend` setting selects `dpdk` (default, production) or `kernel` (MTL kernel-socket backend `MTL_PMD_KERNEL_SOCKET`, marked experimental by MTL; ports configured with `ifname`, passed as `kernel:<ifname>` — MTL `doc/kernel_socket.md`; still needs hugepages; the interface's kernel IP is used; `net.core.rmem_max ≥ 4194304` recommended). The kernel backend is **test-only**: no pacing guarantees, no HW PTP; the UI and `/readyz` show a permanent "test backend" warning, `ptp.mode` is forced to `external` and `ptp.require_lock` to false.

### 17.3 Integration Tests (CI, GitHub-hosted Ubuntu runner)

- Runner prep: `sudo sysctl vm.nr_hugepages=1024`, create a veth pair with multicast routing, tmpfs for the domain.
- `tools/mxl-pattern-writer` (part of this repo): writes a v210 colour-bar pattern with a frame counter, a 1 kHz tone per channel, and SMPTE 12M timecode ANC into an MXL domain.
- `tests/integration/loopback.sh`: gateway instance with an **egress** group (pattern flows → 2110 on veth A) and an **ingest** group (2110 on veth B → new MXL flows); verify with `tools/mxl-verify` (frame counter continuity, tone frequency/level per channel, timecode continuity, audio/video alignment within ±1 audio block) and `mxl-info` / `mxl-data-probe`.
- `tests/integration/late-flow.sh` (receiver activated before its flow exists): start the gateway with an egress group and no pattern writer running; create a simulated mirror domain `mirror-<id>` (with `domain_def.json` incl. the `x-mxl-fabrics-agent` marker) under the scan path; IS-05 PATCH the MXL Receivers with that `mxl_domain_id`, a not-yet-existing `mxl_flow_id` and `master_enable=true` — the activation succeeds, the essences report `waiting_for_flow` and `mxl_st2110_gateway_mxl_flow_not_found_total` increases; after ≥ 10 s start `tools/mxl-pattern-writer` for that flow id in the mirror domain — the receivers start without any further request (state `running`, `mxl_st2110_gateway_mxl_reader_info{domain_kind="mirror"}`, ST 2110 output verified by the ingest side); stop the writer → `no_signal`, not `error`, `/readyz` unaffected; restart the writer (flow re-created) → reading resumes automatically; finally verify that no file was written into the mirror domain by the gateway.
- `tests/integration/nmos-testing.sh`: run the AMWA nmos-testing tool (pinned commit, Python venv or container, host networking, `DNS_SD_MODE = 'multicast'`, Avahi on the runner) and the gateway with `node.registry.dns_sd` and `node.st2110.registry.dns_sd` = true; run IS-04-01, IS-05-01 (v1.1 on the ST 2110 node, v1.2 on both), IS-05-02 against both nodes and BCP-007-03-01 against the MXL node non-interactively (`nmos-test.py suite <S> --host … --port … --version …`), fail on any `Fail`, publish the JSON results as an artifact. No registry container (owner decision Q13).
- `tests/integration/lifecycle.sh` (platform guideline G14): a gateway on the mock backend configured only by the platform's standard variables, as uid 1000 without D-Bus, against a mock registry (`tests/integration/mock_registry.py`, Registration + Query API): start → registered → `/readyz` 200; seed-derived ids, IP-literal hrefs/endpoints and `NMOS_TAGS` in the registry, only MXL resources registered, the ST 2110 node on `NMOS_PORT + 1`; a second instance on other ports; a taken port exits 75; an active MXL Receiver survives a restart; SIGTERM → exit 143 within `SHUTDOWN_TIMEOUT_S`, every resource DELETEd (node last), the own output domain removed with `MXL_CLEANUP_ON_EXIT=true` and a sibling domain untouched; image user 1000 and OCI labels.

### 17.4 Hardware Acceptance (manual, documented in `docs/acceptance.md`)

On an E810 2×25G and a 2×100G host with a real PTP grandmaster and the operator's NMOS controller: §19 list, plus an ST 2110 analyser check of the egress (narrow pacing compliance, RTP offset, 2022-7 skew).

---

## 18. Non-Functional Requirements

- **Capacity targets** (measure and document in `docs/performance.md`; per direction, concurrently, zero packet loss and zero late frames over 24 h): E810 2×25G — 8× 1080p50 in + 8× 1080p50 out, each with 16 ch audio and ANC; E810 2×100G — 4× 2160p50 in + 4× 2160p50 out with audio/ANC. Report CPU cores used for conversion.
- **Robustness:** loss of one 2022-7 leg causes no frame loss; loss of PTP is reported within 2 s; a crashed reader in another container never affects the gateway; malformed RTP/SDP never crashes the process (fuzz `sdp_map` and `anc8331` parsers with libFuzzer in CI for 60 s); flows that appear late, go silent, disappear or are re-created — including mirror domains and mirror flows managed by mxl-fabrics-agent — never require operator or controller action and never fail a receiver permanently (§5.8); a malformed or foreign `domain_def.json` in a discovered domain never stops the gateway (§8.5).
- **Startup:** ready ≤ 60 s after container start on a prepared host (excluding PTP lock time).
- **Security:** no secrets in config; TLS optional; container runs without `privileged`; dependencies pinned; SBOM published.
- **Documentation:** README sections for overview, quick start (Compose), Kubernetes, host preparation, configuration reference (generated from the schema by `tools/gen_config_docs.py` into `docs/configuration.md`), admin UI, metrics, PTP modes and their side effects, port table and co-location with mxl-decklink / mxl-fabrics-agent (§15.4), multi-host operation with mxl-fabrics-agent (§8.6, the mxl-fabrics-agent §11 checklist, demo of §15.1/§15.2), limitations (one NIC, NIC is not a redundant element, fixed formats), troubleshooting (every preflight message), conformance results, license.

---

## 19. Acceptance Criteria (v1.0)

1. Fresh host following the README → container ready; setup mode works without a config file.
2. UI: create an ingest group "CAM 1" with 1 video + 2 audio (8 ch each) + 1 ANC and an egress group "PGM" likewise, redundancy on; config file contains both; export → import on a second instance reproduces identical NMOS ids with `keep_ids=true`.
3. Operator's NMOS controller connects an external 2110 source to the ingest receivers; MXL flows appear in the domain, `mxl-info` shows them active with correct descriptors; a third-party MXL reader displays correct video (proves real v210), audio and ANC.
4. Controller connects ingest MXL senders to egress MXL receivers (same or second gateway); egress output is verified on an ST 2110 analyser: narrow pacing compliant, 2022-7 both legs, lip-sync error ≤ 1 audio sample block versus source.
5. Pulling one network leg: no visible/audible error; metrics show leg loss; PTP stays locked via the other port (`mxl_st2110_gateway_ptp_selected` moves if the pulled leg was selected).
6. GM failover (with and without a parent change): `mxl_st2110_gateway_ptp_gm_changes_total` increments, GM identity in UI changes, PTP re-locks, media continues.
7. Hand-editing the config + restart applies; UI save after external edit is blocked until resolved.
8. AMWA suites IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01: zero failures.
9. Restart with `resume_connections=true` restores all active connections without controller action.
10. Prometheus scrapes `/metrics`; the shipped Grafana dashboard renders all panels with data.
11. Tag `v1.0.0` produces images `1.0.0`, `1.0`, `1`, `latest` on GHCR and a GitHub Release with assets; a push to `main` updates `nightly-dev`.
12. Kubernetes manifests deploy successfully on a node with the SR-IOV device plugin; readiness turns green; another pod on the node reads the flows via the hostPath domain.
13. An egress MXL Receiver activated before its flow exists starts automatically when the flow appears, goes to `no_signal` (not `error`) when the writer stops and resumes after the writer is restarted (CI test `late-flow.sh`, §17.3).
14. *(Optional, does not gate v1.0.)* Multi-host demo with mxl-fabrics-agent (§15.1 or §15.2): an ingest flow of the gateway on host A is received by an MXL Receiver on host B (gateway egress or mxl-decklink) through a mirror domain, without manual steps beyond the IS-05 connection; `mxl_st2110_gateway_mxl_reader_info` shows `domain_kind="mirror"`.

---

## 20. Risks and Open Items

| # | Item | Mitigation |
|---|---|---|
| R1 | `st20p` RX external-frame support with v210 output (§6.1) | resolved: verified in MTL `v26.09` (`query_ext_frame` in conversion mode); 720p dropped because of the converter's width constraint |
| R2 | MXL behaviour when writing indices outside the ring window (§5.6) | resolved: writer rejects index ≤ last committed; writer re-creation after sustained rejections; integration test against MXL v1.1.0 |
| R3 | Sync-group semantics with continuous (audio) flows (§5.7) | resolved: wait = head ≥ `timestampToIndex(rate, T)` per reader; one worker per group |
| R4 | `builtin_phc2sys` turns the host wall clock into TAI | default mode `builtin`; preflight check; prominent docs |
| R5 | nmos-cpp interfaces/ports overrides (§4.5, §7.1) | resolved in nmos-cpp `fe30384` (own `interfaces`, negative ports, catch-all handler moved); integration test with nmos-testing |
| R6 | DDP package discovery inside the container (§14.1) | bundle + log loaded version; fail on PMD "safe mode" |
| R7 | MTL patch drift on MTL upgrades | minimal patch, CI apply-check, upstream PR |
| R8 | Kernel-socket backend multicast behaviour on GitHub runners | veth pair with explicit multicast routes; fall back to self-hosted runner if flaky |
| R9 | Single NIC is a single point of failure | documented limitation; later stage multi-NIC (§21) |
| R10 | License | MIT (LICENSE in the repository); dependency licenses (BSD-3 MTL/DPDK, Apache-2.0 MXL/nmos-cpp) listed in `THIRD_PARTY_NOTICES.md` |
| R11 | Accepting unknown `mxl_domain_id` deviates from BCP-007-03 (§7.4) | owner decision C3 for mxl-fabrics-agent on-demand mirroring; logged warning; not exercised by BCP-007-03-01; documented in `docs/conformance.md` |
| R12 | MXL version skew between gateway, mxl-decklink and mxl-fabrics-agent sharing one MXL root (the agent may pin a later v1.1 revision with Fabrics fixes) | same v1.1 line on a host (§2); check the agent's pin on every `MXL_REF` bump; integration test against a flow written by the agent's MXL build when available |
| R13 | Host-port collisions under host networking (default `8080` = mxl-decklink) | port table §15.4, preflight warning, deployment-level remapping / reverse proxy (owner decision C1) |
| R15 | Dual-port BMCA patch (0003) is a behaviour change inside MTL's PTP | pure selection logic unit-tested in the gateway with the same vectors; hardware acceptance with leg pull and GM failover; upstream PR |
| R14 | Cross-host TAI misalignment makes replicated indices land at the wrong time | TAI discipline required on all hosts (§5.1), preflight warning on a zero TAI offset, `mxl_st2110_gateway_mxl_read_lag_grains` and the agent's TAI-offset metric in Grafana |

---

## 21. Later Stage (explicitly out of v1, keep the design open for them)

Multiple port pairs / two NICs per container (lift the `port_pairs` limit, add `groups[].port_pair`); four-port cards as two independent 2022-7 pairs; ST 2110-22; sub-frame latency (MTL slice RX + MXL partial grain commits / `mxlFlowReaderGetGrainSlice`); MXL Fabrics inside the gateway process (host-to-host replication is already covered in v1 by mxl-fabrics-agent, §8.6); BCP-008-01/-02 status monitoring (fed by the 2022-7 leg counters); IS-08 audio channel mapping; IS-12 control; "follow SDP" dynamic formats; `video/v210a`; authenticated UI.

---

## 22. Glossary

**PF/VF** physical/virtual PCI function · **PMD** DPDK poll-mode driver · **lcore** DPDK logical core · **PHC** PTP hardware clock · **GM** grandmaster · **TAI** International Atomic Time (PTP timescale) · **grain** one discrete MXL ring-buffer entry (video frame/field, ANC frame) · **continuous flow** MXL sample-indexed ring buffer (audio) · **leg** one of the two 2022-7 paths · **MXL root** the host tmpfs that holds all MXL domains of a host (mounted at `mxl.scan_path`) · **configured / discovered / mirror domain** see §3.3 and §8.5 · **origin flow** a flow written by a media function in a local domain · **mirror flow** a flow in a mirror domain, written by mxl-fabrics-agent with the origin's flow id, definition and indices · **read offset** how far behind the writer an MXL Receiver reads (§5.7).
