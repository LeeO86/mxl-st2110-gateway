# mxl-st2110-gateway — Technical Specification

| | |
|---|---|
| Status | Draft 1.0 — implementation baseline for Claude Code |
| Date | 2026-10-01 |
| Repository | `mxl-st2110-gateway` (new, empty repository) |
| Sibling project | [`LeeO86/mxl-decklink`](https://github.com/LeeO86/mxl-decklink) — reuse its conventions (layout, CI, web UI stack, health/metrics, NMOS integration, `mxlbridge/` module shapes) wherever this document does not say otherwise |
| Companion documents | `IMPLEMENTATION_PLAN.md` (phases + acceptance criteria), `AGENTS.md` (agent instructions) |

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119. Anything marked **VERIFY** is a fact that was checked against the pinned sources on 2026-10-01 but that the implementer MUST re-confirm in code before relying on it (write a unit test or a source comment citing file and line).

---

## 1. Overview, Goals, Non-Goals

### 1.1 Purpose

`mxl-st2110-gateway` is a single container that bridges **SMPTE ST 2110** networks and a host-local **MXL** (Media eXchange Layer) domain, in **both directions**, for **video, audio and ancillary data**:

- **Ingest** — ST 2110-20 / -30 / -40 multicast → MXL flows (`video/v210`, `audio/float32`, `video/smpte291`).
- **Egress** — MXL flows → ST 2110-20 / -30 / -40 multicast.

It is controlled exclusively through **AMWA NMOS IS-04 / IS-05** (including **BCP-007-03** for the MXL side) using **Sony nmos-cpp**, and configured through an **admin web UI** whose state lives in a **mounted configuration file**.

The data path uses **Intel Media Transport Library (MTL)** on **DPDK** with Intel **E810** NICs to achieve ST 2110-21 compliant pacing (narrow / narrow-linear) and hardware PTP timestamping. **GStreamer is explicitly not used.**

### 1.2 Goals

1. Spec-correct MXL: v210 grains contain real v210 (never RFC 4175 wire data), audio is de-interleaved float32 in a continuous flow, ANC follows the MXL RFC 8331 grain layout.
2. ST 2110-21 narrow pacing on transmit; ST 2022-7 Class A redundancy (two ports on the same NIC) on receive and transmit.
3. Lip-sync preserving: MXL grain/sample indices derived from RTP origination timestamps on ingest; RTP timestamps derived from MXL indices on egress.
4. Pass the official AMWA NMOS Testing Tool suites IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 with no failures.
5. Interoperate with Sony nmos-cpp registries and controllers (the operator's existing NMOS controller).
6. One container = one NIC = one NMOS Node, bidirectional.
7. Container and Kubernetes deployment are both first-class (documented and shipped).
8. Observability via Prometheus `/metrics` plus a generated Grafana dashboard.

### 1.3 Non-Goals (this version)

- ST 2110-22 (JPEG XS / compressed video), ST 2022-6, AES67-only profiles beyond what ST 2110-30 covers.
- MXL Fabrics / RDMA (host-to-host MXL). Build MXL with `-DMXL_ENABLE_FABRICS_OFI=OFF`.
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
| MXL SDK | tag **`v1.1.0`** | `github.com/dmf-mxl/mxl`, Apache-2.0, Fabrics OFF, built with vcpkg exactly as in mxl-decklink `docker/Dockerfile` |
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

Reference implementations (read, do not copy blindly):

- MTL `ecosystem/MTL_with_MXL/poc/src/sender/mxl_bridge.c` — FlowWriter setup, grain slot management, `query_ext_frame` pattern. **Known defect: it DMAs RFC 4175 pgroup data (`ST20_FMT_YUV_422_10BIT`) straight into grains declared as `video/v210` without conversion. Do not replicate.**
- mxl-decklink `src/mxlbridge/` (`videowriter`, `audiowriter`, `ancwriter`, `videoreader`, `audioreader`, `flowsync`, `domain`, `flowdef`), `src/util/` (`audioconv`, `anc`, `taiclock`, `uuid`, `v210`), `src/nmos/node.cpp`, `src/ops/` (`httpserver`, `metrics`, `health`, `webapi`).
- nmos-cpp `Development/nmos-cpp-node/node_implementation.cpp` — MXL sender/receiver construction (~l.990–1070) and MXL `auto` resolution (~l.2125).

---

## 3. System Architecture

### 3.1 Process and Container Model

- One process (`mxl-st2110-gateway`), one container, one MTL instance, one NMOS Node, one NIC.
- The process hosts: the MTL instance (DPDK EAL, lcores, built-in PTP), the MXL instance(s) (one `mxlInstance` per configured domain), the nmos-cpp node server, and the HTTP routes for `/admin`, `/api`, `/metrics`, `/livez`, `/readyz`, `/statusz`.
- All HTTP endpoints share **one TCP port** (`node.http_port`, default `8080`), see §10.

### 3.2 Repository Layout (target)

```
mxl-st2110-gateway/
├── AGENTS.md  SPECIFICATION.md  IMPLEMENTATION_PLAN.md  README.md  LICENSE  CHANGELOG.md
├── CMakeLists.txt  cmake/
├── src/
│   ├── main.cpp
│   ├── config/        schema.{hpp,cpp} store.{hpp,cpp} config.{hpp,cpp} formats.{hpp,cpp}
│   ├── mtl/           instance.* (EAL/ports/PTP) video_rx.* video_tx.* audio_rx.* audio_tx.* anc_rx.* anc_tx.* sdp_map.*
│   ├── mxlbridge/     domain.* flowdef.* videowriter.* videoreader.* audiowriter.* audioreader.* ancwriter.* ancreader.* flowsync.*
│   ├── timing/        ptp.* rtpclock.* (RTP⇄TAI⇄MXL index, pure functions)
│   ├── codec/         audioconv.* (L16/L24 ⇄ float32) anc8331.* (st40 meta ⇄ RFC 8331)
│   ├── group/         group.* ingest_essence.* egress_essence.* group_manager.*
│   ├── nmos/          node.* resources.* activation.* caps.* ids.*
│   ├── ops/           httpserver.* webapi.* metrics.* health.* logging.*
│   └── util/
├── web/               Vue 3 + Vite, single-file build embedded in the binary (mxl-decklink pattern)
├── patches/mtl/       0001-ptp-status-api.patch (§5.5)
├── schema/            gateway-config.schema.json (JSON Schema draft 2020-12)
├── config/examples/   gateway.example.json  gateway.minimal.json
├── docker/            Dockerfile entrypoint.sh docker-compose.yaml
├── deploy/k8s/        namespace.yaml sriov-dp-configmap.yaml deployment.yaml service.yaml configmap.yaml kustomization.yaml README.md
├── monitoring/        grafana/mxl-st2110-gateway.json prometheus/scrape-example.yaml tools/gen_dashboard.py
├── tests/unit/        doctest
├── tests/integration/ smoke.sh loopback.sh nmos-testing.sh
└── .github/workflows/ ci.yaml container.yaml release.yaml
```

Language: **C++20**, CMake + Ninja, `-Wall -Wextra -Werror` in CI, unit tests with **doctest** (as mxl-decklink). MTL and DPDK are C libraries; wrap their handles in RAII types.

### 3.3 Core Concepts

- **Group** — the user-facing unit (e.g. "CAM 1"). A group has a **direction** (`ingest` = 2110→MXL, or `egress` = MXL→2110), a target MXL domain, a redundancy flag and **N video + M audio + K ANC essences**. A group maps to one BCP-002-01 group hint.
- **Essence** — one stream of one type within a group. Each essence owns exactly one MTL session and one MXL writer or reader.
- **Ingest essence** = IS-04 *Receiver* (`rtp.mcast`) + IS-04 *Source/Flow/Sender* (`mxl`) + MTL RX session + MXL FlowWriter.
- **Egress essence** = IS-04 *Receiver* (`mxl`) + IS-04 *Source/Flow/Sender* (`rtp.mcast`) + MXL FlowReader + MTL TX session.

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
mxlFlowReader (per essence), all readers of a group in one mxlFlowSynchronizationGroup
     ──► wait for index i (group-aligned)                               (§5.7)
     ──► codec: v210 grain used as st20p ext_frame (MTL converts to RFC 4175)
               float32 per channel → L16/L24 BE interleaved
               RFC 8331 grain body → st40 meta+UDW
     ──► MTL TX with USER_TIMESTAMP (RTP ts from index i) and USER_PACING (TAI(i) + output_delay)
     ──► ST 2110-21 narrow pacing on port P (+R duplicate)
```

### 3.6 Threading Model

- MTL owns its lcores (polling, pacing, PTP). Lcore list is configurable (`nic.lcores`) and MUST be disjoint from application threads.
- Per essence, one application worker thread (blocking `*_get_frame` with timeout for RX, sync-group wait for TX). Callbacks executed on MTL lcores (`notify_frame_available` etc.) MUST NOT block, allocate, log synchronously or take contended locks — they only signal the worker.
- One control thread processes NMOS activations and admin changes through a single serialized queue (mxl-decklink `enqueue(readActivation(...))` pattern). Pipeline (re)configuration never runs on an HTTP handler thread.
- Real-time threads MAY use `SCHED_FIFO` when `CAP_SYS_NICE` is available; failure to raise priority is a warning, not an error.

### 3.7 Latency Budget (targets, 1080p50)

| Stage | Target |
|---|---|
| Ingest video, last packet → grain committed | ≤ 1 frame + 2 ms (full-frame conversion) |
| Ingest audio, packet → samples committed | ≤ 1 audio block (default 1 ms) + 1 ms |
| Egress, grain available → first packet on wire | `output_delay` (default 1 frame, configurable per group) |

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

### 4.3 Port Pair and ST 2022-7

- The network is modelled as a **list of port pairs** (`nic.port_pairs[]`). Each pair has a `primary` port and an optional `redundant` port, each with its own PCI address and IP configuration.
- **This version limits the list to exactly one pair** (validation error otherwise). The data model is a list from day one so that a later version can lift the limit without a schema break (§21).
- ST 2022-7 is realised with **both ports of the same E810** (2×25G or 2×100G cards). Both legs share one PHC — no inter-leg clock offset. The NIC itself is not a redundant element; this protects against network failures, not NIC failure. This MUST be stated in the README.
- Redundancy is enabled **per group** (`groups[].redundancy`), never per essence. A redundant group requires a `redundant` port in the pair; validation error otherwise.
- MTL maps: `MTL_SESSION_PORT_P` = primary, `MTL_SESSION_PORT_R` = redundant (`num_port = 2` in session ops).

### 4.4 Cards with More Ports (e.g. E810 4×25G)

The gateway binds **only** the PCI functions listed in `nic.port_pairs[0]`. Other functions of the same card stay with the kernel and remain usable for management or a host PTP client (§5.2 `external`). No error, no warning beyond an info log listing ignored sibling functions of the same card.

### 4.5 NMOS Interfaces

nmos-cpp normally derives the Node's `interfaces` from kernel network interfaces, which do not include DPDK ports. The gateway MUST build the IS-04 Node `interfaces` array itself:

- one entry per bound port, `name` = configured port name (e.g. `media-p`, `media-r`), `port_id` = port MAC from MTL (`mtl_port_mac`), `chassis_id` = MAC of the card's first function (or `null` if unknown);
- the management interface is listed as well (from the kernel) so `href`/`api.endpoints` stay correct;
- every `rtp.mcast` Sender/Receiver has `interface_bindings` = `[primary]` or `[primary, redundant]`;
- every `mxl` Sender/Receiver has `interface_bindings = []` (BCP-007-03).

**VERIFY** how `nmos::make_node` receives interfaces in nmos-cpp `fe30384` (`node_resources.cpp`) and override accordingly.

---

## 5. Timing and PTP

This is the most critical part of the design. Implement it as pure, unit-tested functions in `src/timing/` before any pipeline code.

### 5.1 Clocks Involved

| Clock | Used by | Source |
|---|---|---|
| MTL PTP time (TAI ns) | MTL RX timestamps, TX pacing, RTP derivation | E810 PHC disciplined by MTL built-in PTP (or user callback) |
| Host `CLOCK_TAI` | **MXL**: `mxlGetTime()`, `mxlGetCurrentIndex()`, reader head expectations (MXL `lib/src/time.cpp` uses `Clock::TAI`) | kernel; `CLOCK_TAI = CLOCK_REALTIME + kernel TAI offset` |

MXL readers on the same host (other containers) compute "now" from **their** `CLOCK_TAI`. If the gateway writes grains at indices derived from MTL PTP time while the host `CLOCK_TAI` disagrees, other media functions see grains as too early/too late. The two clocks MUST agree to well within one grain.

### 5.2 PTP Modes (`ptp.mode`)

| Mode | MTL flags | Host clock handling | Use when |
|---|---|---|---|
| `builtin` (default) | `MTL_FLAG_PTP_ENABLE` (+ `MTL_FLAG_PTP_PI` on PF) | **untouched**. The host MUST discipline `CLOCK_TAI` to the same grandmaster by other means (e.g. `ptp4l`+`phc2sys` on a kernel-owned port, chrony with a PHC refclock) **with a correct kernel TAI offset** (37 s) | normal hosts, shared hosts, Kubernetes |
| `builtin_phc2sys` | `MTL_FLAG_PTP_ENABLE` + `MTL_FLAG_PHC2SYS_ENABLE` (needs `CAP_SYS_TIME`) | MTL steers **`CLOCK_REALTIME`** to the PHC **without subtracting the UTC offset** (MTL `lib/src/mt_ptp.c` `phc2sys_adjust` / `ptp_adj_system_clock_time` use `CLOCK_REALTIME`). Consequence: `CLOCK_REALTIME` shows TAI (wall clock 37 s ahead of UTC). Therefore the kernel TAI offset MUST be 0 so that `CLOCK_TAI == PTP`. The gateway MUST check `adjtimex()` `tai` at startup and refuse to start in this mode if it is non-zero. NTP/chrony on the host MUST be disabled (MTL `doc/run.md` §8.12). | dedicated single-purpose appliances only; document the side effects prominently |
| `external` | no `MTL_FLAG_PTP_ENABLE`; `mtl_init_params.ptp_get_time_fn` returns `clock_gettime(CLOCK_TAI)` | host is the single time authority | VF deployments, hosts already PTP-locked via kernel |

Never subtract or add the UTC offset anywhere in the media path: RTP, MTL and MXL all operate on TAI since the ST 2059-1 epoch. The UTC offset is only displayed.

### 5.3 Clock Supervision

- Every second, sample `d = mtl_ptp_read_time(mt) − clock_gettime(CLOCK_TAI)` (take the min-delay sample of several reads, like MTL `phc2sys_adjust`).
- Expose `mxlgw_clock_mtl_minus_host_tai_ns` (gauge) and its 60 s min/max.
- Thresholds (configurable): `ptp.warn_offset_ns` default 10 000 (10 µs), `ptp.max_offset_ns` default 1 000 000 (1 ms). Above max ⇒ `/readyz` reports not ready (reason `clock_mismatch`) and the UI shows a red banner. Media keeps flowing (no automatic stop).
- PTP lock state, offset, path delay and grandmaster data come from the patched MTL API (§5.5). Not locked ⇒ not ready (reason `ptp_unlocked`) unless `ptp.require_lock=false`.

### 5.4 Index Mapping (pure functions, `src/timing/rtpclock.*`)

Definitions: `T` = TAI ns since ST 2059-1 epoch. Media clocks per ST 2110-10: video and ANC 90 kHz, audio = sample rate (48 kHz). The RTP timestamp is the media-clock count since the epoch modulo 2³².

- `unwrapRtp(rtp32, clockHz, refTaiNs) → T`: compute `refTicks = refTaiNs·clockHz/1e9` (128-bit intermediate), choose the 64-bit tick value ≡ `rtp32 (mod 2³²)` closest to `refTicks`, convert back to ns. `refTaiNs` = MTL `receive_timestamp` of the frame (RX) — this is always within a few ms of origination.
- Video/ANC ingest: `index = mxlTimestampToIndex(&grainRate, T)` using MXL's own helper so rounding matches readers. For interlaced flows MXL internally doubles the declared `grain_rate` to a **field rate** (`FlowParser.cpp` ~l.303–305) and each grain holds one field (`height/2` lines); use the field rate for index math. Each MTL field (`second_field` flag) becomes one grain.
- Audio ingest: sample index `s = unwrapRtp(rtp32, 48000, ref)` directly in samples (MXL continuous flows are indexed in samples at the sample rate — **VERIFY** in `docs/Timing.md` "continuous flows typically pass the sample rate (`grainRate`)").
- Egress: `T = mxlIndexToTimestamp(&rate, index)`; `rtp32 = (T·clockHz/1e9) mod 2³²`; transmit time = `T + output_delay_ns`.

Mandatory unit tests: 25/1, 50/1, 30000/1001, 60000/1001, interlaced 25/1, 48 kHz; values just before/after a 2³² RTP wrap; reference ahead/behind by ±0.5 wrap; round-trip `index → rtp → index` is identity for 10⁶ consecutive indices from a 2026 epoch value.

### 5.5 MTL Patch: PTP Status API (`patches/mtl/0001-ptp-status-api.patch`)

The public MTL API exposes only `mtl_ptp_read_time[_raw]` and the `ptp_sync_notify` callback (`master_utc_offset`, `delta`). Required status lives in the internal `struct mt_ptp_impl` (`lib/src/mt_main.h`): `locked`, `master_initialized`, `master_port_id`, `master_utc_offset`, `t1_domain_number`, `stat_delta_*`, `stat_path_delay_*`, `stat_*_err`, `stat_sync_cnt`.

MTL parses Announce **only once** (`ptp_parse_announce`, guarded by `if (!ptp->master_initialized)`) and never stores `grandmaster_identity` although `struct mt_ptp_announce_msg` contains it. The patch MUST:

1. Add `struct mtl_ptp_status` and `int mtl_ptp_get_status(mtl_handle, enum mtl_port, struct mtl_ptp_status*)` to `include/mtl_api.h`, returning: `locked`, `master_initialized`, parent port identity (clock id + port number), **grandmaster identity**, `grandmaster_priority1/2`, `grandmaster_clock_quality` (class, accuracy, variance), `steps_removed`, `time_source`, `domain_number`, `utc_offset`, last/min/max/avg delta, last/min/max/avg path delay, sync count, error counters, PHC2SYS locked flag.
2. Parse **every** Announce: update GM fields each time; count GM changes (`gm_change_count`) and log a GM change.
3. Be minimal, thread-safe (copy under the existing spinlock or a seqlock), and apply cleanly to `v26.09`. CI MUST fail if the patch does not apply.
4. Be prepared as an upstream PR (`docs/upstream/mtl-ptp-status.md` with rationale).

### 5.6 Unlocked Sources

The gateway does not resample or drop/repeat to compensate drift. If a source is not locked to the same grandmaster, ingest indices drift relative to host time. Detect it: per ingest essence expose `mxlgw_ingest_origin_age_ns` = `now_tai − T(origin)` (gauge, last frame). If outside `[−history/2, +history/2]` the essence state becomes `degraded` with reason `source_clock_drift`; writes continue at the RTP-derived index (MXL rejects/overwrites per ring rules — **VERIFY** behaviour of `OpenGrain` for indices older than the ring tail and handle the status code without crashing).

### 5.7 Egress Synchronisation

- All MXL readers of one egress group are added to one `mxlFlowSynchronizationGroup` (`mxlFlowSynchronizationGroupAddReader`), and the worker waits with `mxlFlowSynchronizationGroupWaitForDataAt(group, T(i), timeout)` before reading index *i* of every essence. **VERIFY** semantics for mixed discrete/continuous flows in MXL `lib/tests/test_flow_sync_groups.cpp`.
- Video/ANC are read per grain; audio is read in blocks aligned to the video grain period (e.g. 960 samples at 50/1) or, for audio-only groups, to `audio.block_us`.
- Transmit uses MTL `ST20P_TX_FLAG_USER_TIMESTAMP` + `ST20P_TX_FLAG_USER_PACING` (and the st30p/st40p equivalents `ST30P_TX_FLAG_USER_PACING`, `ST40P_TX_FLAG_USER_TIMESTAMP`, `ST40P_TX_FLAG_USER_PACING`). Use `*_DROP_WHEN_LATE` and count late frames.
- `output_delay` (per group, default one video frame, minimum enforced so that `T(i)+delay` is in the future when the grain is complete) defines the constant MXL→wire latency. All essences of a group use the same delay ⇒ lip-sync preserved.

---

## 6. Essences

### 6.1 Video (ST 2110-20 ⇄ `video/v210`)

Supported formats (v1): YCbCr 4:2:2 10-bit (`sampling=YCbCr-4:2:2`, `depth=10`), progressive and interlaced, rates 23.98/24/25/29.97/30/50/59.94/60 (interlaced only 25/1 and 30000/1001 per MXL), sizes 1280×720, 1920×1080, 3840×2160. Colorimetry BT709 / BT2020 and TCS SDR/PQ/HLG are carried as metadata (SDP + flow) without processing. `video/v210a` is out of scope.

**Ingest:** `st20p_rx` with `transport_fmt = ST20_FMT_YUV_422_10BIT`, `output_fmt = ST_FRAME_FMT_V210`, `ST20P_RX_FLAG_EXT_FRAME`, external frame = the MXL grain buffer opened with `mxlFlowWriterOpenGrain` (CPU SIMD conversion writes directly into shared memory — one pass, no extra copy). Line stride MUST equal MXL v210 stride `((width+47)/48)*128`. Because the conversion is done by the CPU, the grain buffers need not be DMA-mapped. If MTL's pipeline cannot target a per-frame external buffer in RX (VERIFY `query_ext_frame` support in `st20p_rx_ops` at v26.09), fall back to MTL-owned frames + `st20_rfc4175_422be10_to_v210_simd` into the grain. Never write unconverted RFC 4175 data into a v210 grain.

Commit the grain with `committedSize == grainSize` only for complete frames; for frames with `status != complete` write the frame anyway but set the grain's invalid flag (**VERIFY** the flag name in MXL `mxlGrainInfo`) and count `incomplete_frames`.

**Egress:** `st20p_tx` with `input_fmt = ST_FRAME_FMT_V210`, `transport_fmt = ST20_FMT_YUV_422_10BIT`, `ST20P_TX_FLAG_EXT_FRAME` (+ `_MANUAL_RELEASE` if needed) pointing at the read-only mmapped grain; MTL converts v210 → RFC 4175 into its own hugepage buffer. Pacing `ST21_PACING_NARROW` by default (`LINEAR`/`WIDE` configurable). Packing GPM/BPM configurable (default BPM).

### 6.2 Audio (ST 2110-30 ⇄ `audio/float32`)

Supported: L24 and L16, 48 kHz, 1–64 channels (validated against ST 2110-30 conformance levels A/B/C for the chosen packet time), packet time 1 ms or 125 µs.

MXL audio is a **continuous** ring buffer with **one de-interleaved channel buffer per channel** (`docs/Architecture.md`, `mxlWrappedMultiBufferSlice` with `stride` between channels, wrap-around as two fragments).

**Ingest:** `st30p_rx` with `framebuff_size` = `audio.block_us` worth of samples (default 1000 µs = 48 samples, MUST be an integer multiple of the packet size, `st30_get_packet_size`). Per frame: unwrap RTP → sample index *s*; `mxlFlowWriterOpenSamples(writer, s, n, &slices)`; convert big-endian interleaved L24/L16 to float32 (`x / 2^(bits−1)`, no clamping) into each channel's fragment(s); `mxlFlowWriterCommitSamples`. Respect `mxlFlowWriterGetMaxWriteLengthSamples` (never write more than half the buffer).

**Egress:** read `n` samples at index *s* (`mxlFlowReaderGetSamples`), convert float32 → L24/L16 big-endian interleaved **with clamping to [−1.0, +1.0) and round-to-nearest** (MXL leaves clamping to consumers), hand to `st30p_tx` with user timestamp/pacing.

The converter (`src/codec/audioconv.*`) MUST be SIMD-friendly scalar code with unit tests for full scale, −full scale, zero, over-range clamping, both bit depths, 1/2/8/16/64 channels and fragment wrap.

### 6.3 Ancillary Data (ST 2110-40 ⇄ `video/smpte291`)

MXL stores per grain the RFC 8331 payload **starting at the Length field** (the first 14 bytes — RTP header and extended sequence number — are not stored) in a fixed **4096-byte** grain (`docs/Architecture.md` "Ancillary Data", `docs/FabricsBandwidth.md`, `lib/include/mxl/dataformat.h`).

**Ingest:** `st40p_rx` delivers `st40_frame_info` (array of `st40_meta` with C/line/offset/stream/DID/SDID/UDW size + UDW buffer). `src/codec/anc8331.*` serialises that into the RFC 8331 structure (Length, ANC_Count, F, reserved, then per packet C, Line_Number, Horizontal_Offset, S, StreamNum, DID, SDID, Data_Count, UDW (10-bit, with parity bits), Checksum_Word, word_align). Grain index from the RTP timestamp (one grain per video frame/field). Frames with no ANC still produce a grain with `ANC_Count = 0` so readers keep cadence.

**Egress:** parse the grain back into `st40_meta`/UDW and send via `st40p_tx`. Payloads larger than one RTP packet are split by MTL.

Unit tests: golden vectors for SMPTE 12M timecode (DID 0x60/SDID 0x60), CEA-708 (0x61/0x01), AFD (0x41/0x05), empty frame, parity and checksum generation, round-trip identity, rejection of truncated/oversized grains. Cross-check with MXL `mxl-data-probe` in integration tests.

### 6.4 Format Fixation and Validation

Formats are part of the essence configuration and are therefore static NMOS Flow attributes.

- Ingest: an IS-05 activation whose SDP does not match the configured essence format (resolution, rate, interlace, sampling/depth, channels, sample rate, bit depth, ptime) MUST be rejected at staging with HTTP 400 and a descriptive error. Receiver caps (BCP-004-01) advertise exactly the configured format.
- Egress: the MXL Receiver caps advertise the configured format; staging a `mxl_flow_id` whose `flow_def.json` exists and does not match is rejected with 400. A not-yet-existing flow is accepted; the essence waits (state `waiting_for_flow`) and attaches when the flow appears (poll `mxlIsFlowActive` / flow directory every 500 ms).

---

## 7. NMOS Model and Behaviour

### 7.1 nmos-cpp Integration

- Use `nmos::experimental::make_node_server(node_model, implementation, log_model, gate)` as in mxl-decklink `src/nmos/node.cpp`, then mount the gateway routers on the **same** listener: `server.api_routers[{ {}, http_port }].mount(U("/admin"), …)`, likewise `/api`, `/metrics`, `/livez`, `/readyz`, `/statusz` (`nmos::server::api_routers` is a public `std::map<host_port, api_router>`, `nmos/server.h`). Add an integration test proving all of them answer on one port.
- Settings: `http_port` = `node.http_port`; disable IS-07 (`events_port`, `events_ws_port`), IS-08 (`channelmapping_port`), IS-12/MS-05 (`configuration_port`, `control_protocol_ws_port`) — **VERIFY** in `node_server.cpp` that a negative port skips mounting (mxl-decklink uses `control_protocol_ws_port = -1`). Bind the nmos-cpp Settings and Logging APIs to `127.0.0.1` on loopback-only ports or disable them; they MUST NOT be reachable on the public port.
- `seed_id` = node UUID (§7.3); `label`/`description` from config; `host_address`/`host_addresses` = management IP(s); registry: DNS-SD (default) or static `registry_address`/`registration_port`; `registration_version` v1.3.
- IS-04 v1.3; IS-05 v1.1 and v1.2 (MXL resources only under v1.2 — nmos-cpp `connection_api.cpp` l.91).
- TLS (BCP-003-01) optional: `node.tls.enabled` → `server_secure=true`, certificate/key paths from mounted secrets. When enabled it applies to the whole port (admin UI and `/metrics` included).

### 7.2 Resources per Group

Device: one Device per Node (`label` = node label), `type urn:x-nmos:device:generic`. All Senders/Receivers belong to it.

Ingest group, per essence *e*:

| Resource | Key attributes |
|---|---|
| Receiver (2110) | `transport urn:x-nmos:transport:rtp.mcast`, `format` video/audio/data, `caps.media_types` (`video/raw`, `audio/L24` or `audio/L16`, `video/smpte291`), `caps.constraint_sets` = exact configured format (BCP-004-01), `interface_bindings` per §4.5 |
| Source + Flow (MXL) | Flow = the MXL flow descriptor (`format`, `media_type` `video/v210` / `audio/float32` / `video/smpte291`, geometry/rate/channels); **the IS-04 Flow `id` IS the MXL flow id** and the JSON written to `flow_def.json` is the IS-04 Flow body |
| Sender (MXL) | `transport urn:x-nmos:transport:mxl`, `interface_bindings: []`, `manifest_href: null`, `flow_id` = Flow id |

Egress group, per essence *e*:

| Resource | Key attributes |
|---|---|
| Receiver (MXL) | `transport urn:x-nmos:transport:mxl`, `interface_bindings: []`, `format`, `caps.media_types` (`video/v210` / `audio/float32` / `video/smpte291`), `caps.constraint_sets` = configured format |
| Source + Flow (2110) | `video/raw` / `audio/L24`/`L16` / `video/smpte291` with configured attributes |
| Sender (2110) | `transport rtp.mcast`, `manifest_href` → `/transportfile`, SDP generated by nmos-cpp `nmos::make_sdp_parameters` + `make_session_description` (with `a=group:DUP` and two media sections when redundant, `ts-refclk:ptp=IEEE1588-2008:<gm>:<domain>`, `mediaclk:direct=0`) |

Tags on every Source/Flow/Sender/Receiver: `urn:x-nmos:tag:grouphint/v1.0` = `["<group label>:<Role> <n>"]` (BCP-002-01; roles `Video`, `Audio`, `Data`; numbering per type starting at 1), e.g. `CAM 1:Video 1`, `CAM 1:Audio 3`.

### 7.3 Stable Identifiers

IDs MUST survive restarts and container recreation.

- Node seed: `node.id` (UUID) in the config. Generated once on first start if absent and written back to the file.
- Every group has an immutable `uid` (UUID) generated at creation and persisted. Renaming a group keeps its IDs.
- Every essence has an immutable `uid` as well (so reordering/removing essences does not shift IDs).
- Resource IDs = UUIDv5(namespace = essence `uid`, name = `"sender"|"receiver"|"source"|"flow"`). Device id = UUIDv5(node id, `"device"`).
- Unit test: same config ⇒ same IDs; rename ⇒ same IDs; delete+recreate essence ⇒ new IDs.

### 7.4 IS-05 Behaviour — MXL Side (BCP-007-03 v1.0.0)

Use nmos-cpp `make_connection_mxl_sender(id, domain_id, flow_id)` / `make_connection_mxl_receiver(id, domain_id)` and its `resolve_auto` machinery.

- One transport-parameter set (no `_R` leg) in `/staged`, `/active`, `/constraints`.
- MXL Sender (ingest): constraints `mxl_domain_id.enum = [group domain id]`, `mxl_flow_id.enum = [flow id]`. `auto` resolves to those. `null` accepted. `/transportfile` returns 404.
- MXL Receiver (egress): `mxl_domain_id` constraint = enum of all configured domain ids; `auto` resolves to the group's domain, or — if the staged `mxl_flow_id` is found in a different configured domain — to that domain (BCP-007-03 "Automatic resolution"). `mxl_flow_id` accepts UUID or `null`, MUST NOT accept `auto`. A staging/activation request with a transport file is rejected; omitted or `{data:null,type:null}` is accepted.
- Immediate activation whose `auto` cannot be resolved ⇒ HTTP 500 (BCP-007-03). A `mxl_domain_id` that is not one of the configured domains ⇒ 400 at staging.
- `master_enable=true` starts the MXL write (sender) / read (receiver); `false` stops it. Stopping a writer releases the FlowWriter (`mxlReleaseFlowWriter`), so the flow becomes inactive for readers.

### 7.5 IS-05 Behaviour — ST 2110 Side

- Receiver (ingest): accepts an SDP transport file **or** transport params (`multicast_ip`, `source_ip` (SSM), `destination_port`, `interface_ip` = `auto`, `rtp_enabled`). SDP parsed with nmos-cpp `sdp_utils` (`get_session_description_sdp_parameters`, `get_video_raw_parameters`, `get_audio_L_parameters`, `get_video_smpte291_parameters`); a pure mapper `src/mtl/sdp_map.*` converts `sdp_parameters` + transport params into MTL ops (fps enum, fmt, ptime, channels, payload type, IPs, ports). A one-leg SDP on a redundant group runs only the primary leg (`rtp_enabled=false` on leg 2).
- Sender (egress): `destination_ip`, `destination_port`, `source_ip` (`auto` → port IP), `rtp_enabled` per leg; defaults from config.
- **Re-activation without rebuild:** if only addresses/ports/`rtp_enabled` change, use `st20p_rx_update_source` / `st30p_rx_update_source` / `st40p_rx_update_source` and `*_tx_update_destination` (all exist in v26.09). Format changes are impossible by design (§6.4). Session teardown/creation only on `master_enable` transitions.
- Activation latency target: immediate activation returns within 200 ms (pipeline change is async but `/active` reflects the new state when the response is sent, as nmos-cpp requires).

### 7.6 Connection Persistence

`node.resume_connections` (default `true`): the last `/active` endpoint of every Sender/Receiver is persisted to `state/connections.json` (same mounted directory as the config, written atomically). On restart the gateway re-stages and re-activates them so the facility recovers without controller action. The state file is not part of config import/export.

### 7.7 Conformance Targets

AMWA nmos-testing (pinned commit) suites run in CI against a live gateway instance (MTL kernel-socket backend, §17.3): **IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01** — zero failures, warnings documented in `docs/conformance.md`. BCP-007-03-01 tests 01–18 (`nmostesting/suites/BCP0070301Test.py`) are the checklist for §7.4.

---

## 8. MXL Domains

### 8.1 Facts (MXL v1.1.0 + BCP-007-03 v1.0.0)

- A domain is a directory on **tmpfs** (or ramfs). Flows live in `${domain}/${flowId}.mxl-flow/` (`data`, `flow_def.json`, `access`, `grains/`, `channels` for audio).
- `mxlCreateInstance(domain, options)` only opens an **existing** directory.
- Optional MXL `options.json` in the domain root: `{"urn:x-mxl:option:history_duration/v1.0": <ns>}` (default 200 ms).
- BCP-007-03 requires `domain_def.json` in every domain root: `{id (UUID), label, description, tags}` (schema `mxl_domain_definition.json`). The **id is the `mxl_domain_id`** used in IS-05.
- Domains are mapped into containers by volume mounts; the path inside the container may differ from the host path. Identity comes only from `domain_def.json`.

### 8.2 Configuration

`mxl.domains[]` — one or more entries `{name, path, id?, label?, description?, history_duration_ns?}`. Each group references a domain by `name`. Paths MUST be absolute and unique.

### 8.3 Startup Bootstrap (per domain, before NMOS starts)

1. **Mount check.** Determine the mount containing `path` (or its nearest existing ancestor). It MUST be tmpfs/ramfs (`statfs` `f_type`, same test as `mxlIsTmpFs`) **and** MUST NOT be the container's root/overlay filesystem. If the check fails ⇒ log `mxl_domain_not_tmpfs` with the path and the detected filesystem type, and **exit with code 78 (EX_CONFIG)**. No fallback, no "warn and continue" (stricter than mxl-decklink, by requirement).
2. **Directory.** Create `path` (and parents within the tmpfs mount) with mode `0775` if missing.
3. **`domain_def.json`.**
   - Exists and valid ⇒ **adopt** its `id`; never rewrite it. If the config also specifies a different `id` ⇒ warning `domain_id_mismatch`, the file wins, the UI shows it.
   - Exists and invalid ⇒ exit 78 (do not overwrite someone else's file).
   - Missing ⇒ write it atomically (temp file + `rename`) with `id` = config `id` or a new UUIDv4, `label`/`description` from config (default: domain `name`), `tags: {}`; write the generated id back to the config file.
4. **`options.json`.** Written only if missing **and** `history_duration_ns` is configured. Never overwritten. If present with a different value ⇒ warning, the file wins.
5. `mxlCreateInstance`, then `mxlGarbageCollectFlows` (removes stale flows from crashed writers; MXL uses advisory locks for detection).
6. Expose per domain in `/api/status`: path, id, label, tmpfs ok, flow count, free/used bytes of the mount.

Domain deletion is not offered (as in mxl-decklink).

### 8.4 Flow Lifecycle

- Ingest writer flows are created on MXL Sender activation (`master_enable=true`) with the essence's flow descriptor and released on deactivation or shutdown.
- On SIGTERM: stop MTL sessions, release all FlowWriters/Readers, destroy MXL instances, then MTL (`mtl_uninit`), within 10 s.

---

## 9. Configuration File

### 9.1 Location, Format, Ownership

- Path: `/config/gateway.json` (override with env `MXLGW_CONFIG`). The `/config` directory is a mounted volume; `state/` lives below it.
- Format: JSON, validated against `schema/gateway-config.schema.json` (shipped in the image at `/usr/share/mxl-st2110-gateway/` and served at `/api/schema`).
- Environment variables are limited to bootstrap: `MXLGW_CONFIG`, `MXLGW_LOG_LEVEL`, `MXLGW_HTTP_PORT` (overrides `node.http_port`), and in Kubernetes the PCI override (§15.2). Everything else lives in the file.
- If the file does not exist at startup, the gateway writes `config/examples/gateway.minimal.json` semantics (no groups, NIC unconfigured) and starts in **setup mode**: NMOS and MTL are not started, only the admin UI, `/livez` (ok) and `/readyz` (not ready, reason `unconfigured`).

### 9.2 Writers and Restart Semantics

- **Admin UI writes**: validate → write atomically (temp file in the same directory + `fsync` + `rename`) → keep `gateway.json.bak` (previous version) → apply.
- **Hand edits** are allowed. They take effect only after a **full service restart** (container restart or `POST /api/restart`, which exits with code 0 for the orchestrator/`restart: unless-stopped` to restart). The gateway watches the file's mtime; if it changes on disk while running, the UI shows a persistent banner "configuration changed on disk — restart required" and UI saves are blocked until the operator chooses *reload from disk (restart)* or *overwrite with UI state*.
- Invalid file at startup ⇒ exit 78 with every validation error printed (JSON pointer + message).

### 9.3 Apply Semantics from the UI

| Change | Effect |
|---|---|
| Add a group | applied live: resources created, registered, MTL/MXL objects created on activation |
| Edit / remove a group (incl. its essences) | only that group is torn down and rebuilt; its NMOS resources are re-registered (versions bump, IDs unchanged except removed essences) |
| Edit an essence's network defaults only | applied live via update_source/destination if the essence is active |
| `nic.*`, `ptp.*`, `mxl.domains`, `node.*` | persisted, flagged `restart_required` (UI banner, `/api/status`) |

### 9.4 Import / Export

- `GET /api/config/export` → the current file byte-for-byte (`Content-Disposition: attachment; filename=gateway-<node-label>-<date>.json`).
- `POST /api/config/import` → body validated against the schema and semantic rules; on success written as in §9.2 and the response states `restart_required: true`. Import never applies live. Option `keep_ids` (default `true`): keeps `node.id` and all `uid`s from the file; `false` regenerates them (for cloning a gateway onto another host — the UI explains the consequence).

### 9.5 Schema (normative shape)

```jsonc
{
  "schema_version": 1,
  "node": {
    "id": "c0f1…",                      // generated if absent
    "label": "GW-STUDIO1-A",
    "description": "ST 2110 <-> MXL gateway",
    "http_port": 8080,
    "management_addresses": ["10.10.0.21"],   // empty = auto (all non-DPDK interfaces)
    "registry": { "mode": "dns-sd" },          // or { "mode": "static", "address": "10.10.0.5", "port": 8235 }
    "tls": { "enabled": false, "certificate": "/certs/tls.crt", "private_key": "/certs/tls.key" },
    "resume_connections": true,
    "log_level": "info"
  },
  "nic": {
    "lcores": "4-9",                    // MTL lcores, disjoint from app threads
    "app_cpus": "10-15",                // optional affinity for worker threads
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
    "domain": 127,
    "port": "primary",
    "require_lock": true,
    "warn_offset_ns": 10000,
    "max_offset_ns": 1000000
  },
  "mxl": {
    "domains": [
      { "name": "main", "path": "/mxl/main", "id": null, "label": "Studio 1", "history_duration_ns": 200000000 }
    ]
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
      "uid": "…", "label": "PGM OUT", "direction": "egress", "domain": "main", "redundancy": true,
      "output_delay_ns": 20000000,
      "video": [ { …format…, "pacing": "narrow", "defaults": { "legs": [ { "multicast": "239.10.0.1", "port": 20000 }, { … } ] } } ],
      "audio": [ … ], "anc": [ … ]
    }
  ]
}
```

Semantic validation (beyond JSON Schema) in `src/config/config.cpp`, each rule unit-tested: unique labels/uids/ports names; one port pair; redundant port required when any group has `redundancy`; PCI address format and existence (at runtime, not in import); IPs in the configured subnet; `block_us` multiple of packet time; interlace only with 25/1 or 30000/1001; channel count vs ptime limits; multicast addresses in 224.0.0.0/4; no two egress legs with identical destination; `output_delay_ns` ≥ one grain; domain references exist; lcores parse and are disjoint from `app_cpus`.

---

## 10. HTTP Surface (single port)

| Path | Owner | Purpose |
|---|---|---|
| `/x-nmos/node/…`, `/x-nmos/connection/…` | nmos-cpp | IS-04 Node API, IS-05 Connection API (`/` stays nmos-cpp's base listing — do not override) |
| `/admin/` | gateway | Admin web UI (single embedded HTML file) |
| `/api/…` | gateway | JSON REST API used by the UI (§11.3) |
| `/metrics` | gateway | Prometheus text exposition format 0.0.4 |
| `/livez` | gateway | 200 when the process and HTTP server are alive |
| `/readyz` | gateway | 200 only when: config valid, MTL up, PTP locked (unless `require_lock=false`), clock offset ≤ `max_offset_ns`, all domains ok, NMOS node registered (or registry intentionally absent); else 503 with JSON reasons |
| `/statusz` | gateway | human-readable plain-text status (mxl-decklink parity) |

No authentication (§1.3). All mutating `/api` routes require `Content-Type: application/json` and reject cross-origin requests (check `Origin` against `Host`).

---

## 11. Admin Web UI

### 11.1 Technology

Vue 3 + Vite, built to a single HTML file embedded into the binary at build time (mxl-decklink `web/` pattern). No external assets, works offline. Live data via polling `/api/status` (1 s). Light/dark theme following the browser.

### 11.2 Tabs

- **Dashboard** — node label/id, version (gateway, MTL, DPDK, MXL, nmos-cpp), readiness with reasons, PTP lock + GM identity, MTL−host-TAI offset, NIC link state, per-group tiles with per-essence state, bitrate, frame/packet counters, 2022-7 leg health (green/amber/red from `pkts_recv[P/R]` vs `pkts_total`).
- **Groups** — list of groups; *Create group* dialog: label, direction (ingest/egress), MXL domain, redundancy, and **counts per type** (Video ×n, Audio ×m, ANC ×k as steppers). Creating generates the essences with defaults (format from a node-wide default profile, e.g. 1080p50 / 8 ch L24 1 ms); each essence is then editable (format, payload type, default legs, labels). Editing/removing a group shows which NMOS resources will be re-registered or removed and whether active connections will be interrupted. Duplicate group (new uids).
- **NMOS** — node/device ids, registry discovered/used, registration state and last heartbeat, table of all Senders/Receivers (id, label, transport, group hint, master_enable, active transport params, active SDP for 2110 senders), links to the raw `/x-nmos` resources.
- **Network** — port pair configuration form (PCI, IP, netmask, gateway, port names), detected card model, bind mode (PF/VF), MAC, link speed/state, DDP package version, per-port RX/TX counters and errors. Changes are `restart_required`.
- **PTP** — mode, domain, lock state, grandmaster identity, GM priority/clock class/accuracy, steps removed, parent port identity, UTC offset, last/min/max offset and path delay (sparklines, last 5 min held in memory), GM change counter, PHC2SYS state, MTL−host-TAI offset with thresholds.
- **MXL** — configured domains (path, id from `domain_def.json`, label, tmpfs check, usage), flow browser per domain (id, label, media type, active, head index, last write) — mxl-decklink §7.6 behaviour, read-only.
- **Configuration** — export/download, import/upload (with validation report before writing), raw JSON view (read-only), "changed on disk" diff and resolution, restart button, preflight report (§14.3).

All forms are validated client-side for UX and server-side authoritatively (same rule set as §9.5); errors are shown per field (JSON pointer mapping).

### 11.3 REST API

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status` | GET | everything the dashboard needs (one call) |
| `/api/config` | GET | full config + `ETag` |
| `/api/config` | PUT | full replace, requires `If-Match` (412 on conflict); returns `restart_required` and per-group apply results |
| `/api/groups` | POST | create group (body: label, direction, domain, redundancy, counts or full essences) |
| `/api/groups/{uid}` | PUT / DELETE | edit / delete group (live apply, §9.3) |
| `/api/config/export` | GET | download file |
| `/api/config/import` | POST | upload file (`?keep_ids=true`), never live |
| `/api/schema` | GET | JSON Schema |
| `/api/nic` `/api/ptp` `/api/domains` `/api/flows?domain=` `/api/nmos` | GET | tab data |
| `/api/preflight` | GET | preflight results |
| `/api/restart` | POST | graceful exit for supervisor restart |

---

## 12. Metrics and Grafana

### 12.1 Prometheus Metrics (prefix `mxlgw_`)

Implement a minimal registry like mxl-decklink `src/ops/metrics.*` (no external Prometheus library required). Counters end in `_total`. Common essence labels: `group`, `essence`, `uid`, `type` (`video|audio|anc`), `direction` (`ingest|egress`).

| Metric | Type | Labels |
|---|---|---|
| `mxlgw_build_info` | gauge=1 | `version, mtl, dpdk, mxl, nmos_cpp` |
| `mxlgw_ready` | gauge | — |
| `mxlgw_restart_required` | gauge | — |
| `mxlgw_ptp_locked` | gauge | `port` |
| `mxlgw_ptp_info` | gauge=1 | `port, gm_identity, parent_port_identity, domain, bind_mode` |
| `mxlgw_ptp_offset_ns` / `_path_delay_ns` | gauge | `port` (last value; min/max over 60 s as `stat="min|max"`) |
| `mxlgw_ptp_utc_offset_seconds` | gauge | `port` |
| `mxlgw_ptp_gm_changes_total`, `mxlgw_ptp_sync_total`, `mxlgw_ptp_errors_total` | counter | `port` (+`kind` for errors) |
| `mxlgw_clock_mtl_minus_host_tai_ns` | gauge | — |
| `mxlgw_nic_link_up`, `mxlgw_nic_link_speed_mbps` | gauge | `port` |
| `mxlgw_nic_rx_packets_total`, `_tx_packets_total`, `_rx_bytes_total`, `_tx_bytes_total`, `_rx_errors_total`, `_rx_missed_total` | counter | `port` (from MTL port stats — **VERIFY** `mtl_get_port_stats`) |
| `mxlgw_essence_state` | gauge (1 for current) | essence labels + `state` (`idle|waiting_for_flow|running|degraded|error`) |
| `mxlgw_rx_frames_total` | counter | essence + `result` (`complete|incomplete|dropped`) |
| `mxlgw_rx_leg_packets_total` | counter | essence + `leg` (`p|r`) |
| `mxlgw_rx_packets_total` | counter | essence (after 2022-7 merge) |
| `mxlgw_rx_leg_seq_lost_total` | counter | essence + `leg` |
| `mxlgw_ingest_origin_age_ns` | gauge | essence |
| `mxlgw_mxl_grains_written_total`, `mxlgw_mxl_samples_written_total`, `mxlgw_mxl_write_errors_total` | counter | essence |
| `mxlgw_mxl_grains_read_total`, `mxlgw_mxl_read_timeouts_total` | counter | essence |
| `mxlgw_tx_frames_total`, `mxlgw_tx_late_frames_total` | counter | essence |
| `mxlgw_egress_lead_ns` | gauge | essence (time from grain available to TX deadline; negative = late) |
| `mxlgw_nmos_registered` | gauge | — |
| `mxlgw_nmos_activations_total` | counter | `kind` (`sender|receiver`), `transport`, `result` |
| `mxlgw_mxl_domain_bytes` | gauge | `domain, kind` (`used|free`) |
| `mxlgw_mxl_domain_flows` | gauge | `domain` |

Metric names and labels are a public interface: document them in `docs/metrics.md` and keep them stable across minor versions.

### 12.2 Grafana Dashboard

- Generated by `monitoring/tools/gen_dashboard.py` (deterministic output, no network access) into `monitoring/grafana/mxl-st2110-gateway.json`; CI fails if the committed file differs from a fresh generation.
- Grafana ≥ 11 JSON model; template variables: `datasource` (Prometheus), `instance`, `group`, `essence`.
- Rows: *Overview* (ready, restart required, PTP locked, GM identity as table, clock offset), *PTP* (offset, path delay, GM changes, sync rate), *NIC* (link, throughput, errors/missed per port), *Ingest* (frames by result, leg packet loss P vs R, origin age), *Egress* (late frames, lead time, read timeouts), *NMOS* (registered, activations), *MXL domains* (usage, flow counts).
- Also ship `monitoring/prometheus/scrape-example.yaml` and a Kubernetes `ServiceMonitor` example (`deploy/k8s/servicemonitor.yaml`, optional).

---

## 13. Logging

Structured JSON lines to stdout (one object per line: `ts`, `level`, `event`, fields), human-readable text with `MXLGW_LOG_FORMAT=text`. Stable `event` identifiers (e.g. `mxl_domain_not_tmpfs`, `ptp_gm_changed`, `nmos_activation`, `essence_state`). MTL and DPDK log output is redirected into the same stream with `component=mtl` (MTL log callback / `mtl_set_log_level`, **VERIFY** API). Last 500 lines kept in memory for the UI.

---

## 14. Container

### 14.1 Dockerfile (multi-stage, `docker/Dockerfile`)

1. `webui` — `node:22-bookworm`, `npm ci && npm run build`.
2. `deps` — `ubuntu:24.04`: toolchain, vcpkg; DPDK 26.07 via MTL `script/build_dpdk.sh` (MTL patches); MTL v26.09 + `patches/mtl/*.patch` (`git apply --check` first); MXL v1.1.0 (vcpkg, `-DMXL_ENABLE_FABRICS_OFI=OFF`, `-DBUILD_TOOLS=ON` for `mxl-info` / `mxl-data-probe`); nmos-cpp at the pinned commit. This stage changes only when pins change → maximal cache hits.
3. `build` — compile the gateway, run unit tests (`ctest --output-on-failure`); failing tests fail the image build.
4. `runtime` — `ubuntu:24.04` with runtime libraries only, the binary, MTL/DPDK/MXL shared libs, `mxl-info`, `mxl-data-probe`, the JSON schema, example configs, and the **E810 DDP package** (from the pinned `ice` driver tarball, `versions.env` `ICE_VER`/`ICE_DMID`) installed where the DPDK ice PMD looks for it (`/lib/firmware/updates/intel/ice/ddp/ice.pkg` and `/lib/firmware/intel/ice/ddp/ice.pkg` — **VERIFY** the search path list in DPDK 26.07 `drivers/net/intel/ice/ice_ethdev.c`). The PMD runs in user space inside the container, so the package must be in the **container** filesystem; a host `/lib/firmware` mount MAY override it.

Build args (one place, mirrored in CI): `MTL_REF`, `DPDK_VER`, `MXL_REF`, `NMOS_CPP_REF`, `ICE_VER`, `ICE_DMID`. OCI labels as in mxl-decklink. Image is `linux/amd64` only.

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
| MXL domain volume(s), tmpfs-backed | §8 |
| management network | NMOS + UI (host network recommended for DNS-SD) |

**No `privileged: true`.** The process runs as root inside the container by default (VFIO group nodes are root-owned on most hosts); a non-root mode is documented in the README (udev rule for `/dev/vfio/*`, group-owned domain directories). Files created in domains use mode `0664`/`0775` so other media functions in the same group can read them.

### 14.3 Preflight (`mxl-st2110-gateway --preflight`, also run at startup and served at `/api/preflight`)

Checks with actionable messages (each with a README anchor): hugepages mounted and free pages sufficient for the configured sessions (estimate per session); `/dev/vfio/vfio` present; each configured PCI address exists in `/sys/bus/pci/devices`, is bound to `vfio-pci`, has an IOMMU group whose node exists in `/dev/vfio`; device is an E810/E830 (vendor 0x8086, warn otherwise); capabilities present (`CAP_IPC_LOCK`, `CAP_SYS_NICE`, `CAP_SYS_TIME` if needed); MXL domains tmpfs (§8.3); kernel TAI offset (`adjtimex`) consistent with the PTP mode; lcores exist and are not shared with `app_cpus`. Startup aborts with exit code 78 on any hard failure; soft failures are warnings in the UI.

### 14.4 Signals and Exit Codes

SIGTERM/SIGINT → graceful shutdown ≤ 10 s (§8.4); 0 = normal / restart requested, 78 = configuration or environment error (do not restart-loop silently: log the reason every time), 1 = runtime failure.

---

## 15. Deployment

### 15.1 Docker Compose (`docker/docker-compose.yaml`, also in README)

```yaml
services:
  mxl-st2110-gateway:
    image: ghcr.io/<owner>/mxl-st2110-gateway:1      # or :nightly-dev
    container_name: mxl-st2110-gateway
    restart: unless-stopped
    init: true
    network_mode: host                # management NIC; DNS-SD; media ports are DPDK
    stop_grace_period: 15s
    ulimits:
      memlock: { soft: -1, hard: -1 }
    cap_add: [IPC_LOCK, SYS_NICE]     # + SYS_TIME only for ptp.mode=builtin_phc2sys
    devices:
      - /dev/vfio:/dev/vfio
    volumes:
      - ./config:/config                       # gateway.json + state/
      - /dev/hugepages:/dev/hugepages
      # MXL domain shared with other media functions on this host:
      # /run/mxl is a host tmpfs (see README "Host preparation")
      - type: bind
        source: /run/mxl/main
        target: /mxl/main
    environment:
      MXLGW_CONFIG: /config/gateway.json
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8080/livez"]
      interval: 10s
      timeout: 3s
      start_period: 60s
```

README explains: why host networking, how to create the host tmpfs (`/etc/fstab`: `tmpfs /run/mxl tmpfs size=8g,mode=1777 0 0`), an alternative compose `tmpfs:` volume when the domain is only shared inside one compose project (other services mount the same named tmpfs volume), and how other MXL media functions mount the same domain (read-only for readers is allowed by MXL).

### 15.2 Kubernetes (`deploy/k8s/`, first-class, also in README)

- **Device allocation:** SR-IOV Network Device Plugin (supports PFs, `drivers: ["vfio-pci"]`, `pciAddresses` selector). **One resource per port** so primary/redundant stay deterministic, e.g. `intel.com/e810_media_p` and `intel.com/e810_media_r`. The plugin injects `PCIDEVICE_INTEL_COM_E810_MEDIA_P=0000:31:00.0` (+ `_INFO` with the vfio mounts). The config accepts `"pci": "env:PCIDEVICE_INTEL_COM_E810_MEDIA_P"` for this. Ship `sriov-dp-configmap.yaml` as an example. PFs must be bound to `vfio-pci` on the node beforehand (README: `driverctl set-override`).
- **Pod:** `Deployment`, `replicas: 1`, `strategy: Recreate`, `nodeSelector`/affinity to the node with the card, `hostNetwork: true` (default; alternative: pod network + `Service` + static registry), `dnsPolicy: ClusterFirstWithHostNet`, `terminationGracePeriodSeconds: 15`.
- **Resources:** Guaranteed QoS — requests = limits, integer CPUs (static CPU manager policy recommended for lcore pinning), `hugepages-1Gi` (e.g. `4Gi`), `memory`, the two device resources.
- **Security:** `capabilities.add: [IPC_LOCK, SYS_NICE]` (+ `SYS_TIME` only for `builtin_phc2sys`), `privileged: false`, `allowPrivilegeEscalation: false`. `IPC_LOCK` covers memlock (no runtime ulimit change needed — **VERIFY** on the target containerd).
- **Volumes:** hugepages `emptyDir: {medium: HugePages-1Gi}` at `/dev/hugepages`; config on a **PersistentVolumeClaim** at `/config` (the gateway writes its config — a ConfigMap is read-only, so a ConfigMap MAY only seed the file via an `initContainer` that copies it if absent); MXL domain:
  - **default:** `hostPath` to a node tmpfs (`/run/mxl/main`, `type: DirectoryOrCreate`) so media functions in **other pods on the node** can use the domain;
  - **alternative:** `emptyDir: {medium: Memory, sizeLimit: …}` when all MXL consumers are containers of the **same pod** (emptyDir is not shared across pods).
- **Probes:** `startupProbe` `/livez` (failureThreshold covering ≥ 120 s DPDK init), `livenessProbe` `/livez`, `readinessProbe` `/readyz`.
- **PTP:** the media port runs MTL built-in PTP inside the pod; the node's `CLOCK_TAI` must be disciplined by the cluster (e.g. linuxptp DaemonSet / PTP operator) for `ptp.mode=builtin`.
- Files: `namespace.yaml`, `sriov-dp-configmap.yaml`, `pvc.yaml`, `configmap-seed.yaml`, `deployment.yaml`, `service.yaml`, `servicemonitor.yaml` (optional), `kustomization.yaml`, `README.md`. All MUST pass `kubeconform` in CI.

### 15.3 Host Preparation (README section, both deployment styles)

BIOS: VT-d on, SR-IOV on (if VFs), C-states limited; kernel cmdline `intel_iommu=on iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=<n>` (or 2M pages); bind media PFs to `vfio-pci` persistently; E810 NVM/firmware as recommended by MTL; for VF mode the patched host `ice` driver (MTL `doc/e800_series_drivers.md`); recommended `isolcpus`/`nohz_full`/`rcu_nocbs` for MTL lcores and irqbalance exclusion; host time sync for `CLOCK_TAI` (§5.2) with correct TAI offset (`ptp4l -f … ` + `phc2sys -a -r` or chrony with `leapsectz right/UTC`); host tmpfs for MXL domains. Works on Ubuntu 24.04 and Debian 13 hosts (container userland is independent of the host distro; host kernel needs VFIO).

---

## 16. CI/CD (GitHub Actions, GHCR)

### 16.1 `ci.yaml` (pull requests and pushes)

- Build the `build` stage (compiles + unit tests) with GHA cache.
- Lint: compiler warnings as errors; `clang-format --dry-run --Werror`.
- Check `patches/mtl/*.patch` applies to `MTL_REF`.
- Validate `config/examples/*.json` against the schema; regenerate the Grafana dashboard and diff; `kubeconform` on `deploy/k8s`; `docker compose config` on the compose file.
- Integration job (§17.3): kernel-socket loopback media test and AMWA nmos-testing suites.

### 16.2 `container.yaml` (build and push to `ghcr.io/${{ github.repository }}`)

Same structure as mxl-decklink `.github/workflows/container.yaml`:

- Triggers: push to `main`, tags `v*.*.*`, `workflow_dispatch`, and a nightly `schedule` (rebuild of `main`, picks up base-image security updates).
- `docker/metadata-action` tags:
  - release tag `v1.2.3` → `1.2.3`, `1.2`, `1` **and** `latest`;
  - push to `main` / nightly / manual → **`nightly-dev`** (the "dev latest" tag, always the newest successful `main` build);
  - every build → `git-<shortsha>`.
  - `flavor: latest=${{ startsWith(github.ref, 'refs/tags/') }}` so `latest` is never a dev build.
- `docker/build-push-action` with `cache-from/to: type=gha,mode=max`, `provenance: true`, `sbom: true`.
- Permissions: `contents: read`, `packages: write`.

### 16.3 Releases

- On a `v*.*.*` tag, after the image is pushed, a `release` job (`needs: build-and-push`, `contents: write`) creates the **GitHub Release** for that tag with notes from the matching `CHANGELOG.md` section, and attaches: `gateway-config.schema.json`, the Grafana dashboard JSON, `docker-compose.yaml`, a `k8s-manifests-<version>.tar.gz`, and the image digest. The image reference with digest is printed in the release notes.
- Version source of truth: the git tag; the binary embeds it (`--version`, `mxlgw_build_info`, `/api/status`); untagged builds report `0.0.0-dev+<sha>`.

---

## 17. Testing Strategy

### 17.1 Unit Tests (doctest, no hardware, run in the image build)

Config schema + semantic rules; ID derivation (§7.3); `rtpclock` (§5.4); `audioconv` (§6.2); `anc8331` (§6.3); `sdp_map` (SDP fixtures for 1080p50, 1080i50, 2160p50, 8 ch L24 1 ms, 16 ch L24 125 µs, ANC, with and without DUP groups, malformed SDPs); BCP-007-03 constraint/auto resolution helpers; domain bootstrap against a temporary tmpfs (skip with message if not mountable) and a non-tmpfs dir (must fail); config store atomic write/backup/ETag.

### 17.2 Hardware Abstraction

All MTL calls go through thin interfaces (`src/mtl/*.hpp`) so that unit tests and the mock can run without DPDK. A `nic.backend` setting selects `dpdk` (default, production) or `kernel` (MTL kernel-socket backend, `kernel:<ifname>` ports — MTL `doc/kernel_socket.md`; still needs hugepages). The kernel backend is **test-only**: no pacing guarantees, no HW PTP; the UI and `/readyz` show a permanent "test backend" warning, and `ptp.require_lock` is forced false.

### 17.3 Integration Tests (CI, GitHub-hosted Ubuntu runner)

- Runner prep: `sudo sysctl vm.nr_hugepages=1024`, create a veth pair with multicast routing, tmpfs for the domain.
- `tools/mxl-pattern-writer` (part of this repo): writes a v210 colour-bar pattern with a frame counter, a 1 kHz tone per channel, and SMPTE 12M timecode ANC into an MXL domain.
- `tests/integration/loopback.sh`: gateway instance with an **egress** group (pattern flows → 2110 on veth A) and an **ingest** group (2110 on veth B → new MXL flows); verify with `tools/mxl-verify` (frame counter continuity, tone frequency/level per channel, timecode continuity, audio/video alignment within ±1 audio block) and `mxl-info` / `mxl-data-probe`.
- `tests/integration/nmos-testing.sh`: start a Sony nmos-cpp registry container and the AMWA nmos-testing tool (pinned commit), run IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01 non-interactively against the gateway, fail on any failure, publish the JSON results as an artifact.

### 17.4 Hardware Acceptance (manual, documented in `docs/acceptance.md`)

On an E810 2×25G and a 2×100G host with a real PTP grandmaster and the operator's NMOS controller: §19 list, plus an ST 2110 analyser check of the egress (narrow pacing compliance, RTP offset, 2022-7 skew).

---

## 18. Non-Functional Requirements

- **Capacity targets** (measure and document in `docs/performance.md`; per direction, concurrently, zero packet loss and zero late frames over 24 h): E810 2×25G — 8× 1080p50 in + 8× 1080p50 out, each with 16 ch audio and ANC; E810 2×100G — 4× 2160p50 in + 4× 2160p50 out with audio/ANC. Report CPU cores used for conversion.
- **Robustness:** loss of one 2022-7 leg causes no frame loss; loss of PTP is reported within 2 s; a crashed reader in another container never affects the gateway; malformed RTP/SDP never crashes the process (fuzz `sdp_map` and `anc8331` parsers with libFuzzer in CI for 60 s).
- **Startup:** ready ≤ 60 s after container start on a prepared host (excluding PTP lock time).
- **Security:** no secrets in config; TLS optional; container runs without `privileged`; dependencies pinned; SBOM published.
- **Documentation:** README sections for overview, quick start (Compose), Kubernetes, host preparation, configuration reference (generated from the schema by `tools/gen_config_docs.py` into `docs/configuration.md`), admin UI, metrics, PTP modes and their side effects, limitations (one NIC, NIC is not a redundant element, fixed formats), troubleshooting (every preflight message), conformance results, license.

---

## 19. Acceptance Criteria (v1.0)

1. Fresh host following the README → container ready; setup mode works without a config file.
2. UI: create an ingest group "CAM 1" with 1 video + 2 audio (8 ch each) + 1 ANC and an egress group "PGM" likewise, redundancy on; config file contains both; export → import on a second instance reproduces identical NMOS ids with `keep_ids=true`.
3. Operator's NMOS controller connects an external 2110 source to the ingest receivers; MXL flows appear in the domain, `mxl-info` shows them active with correct descriptors; a third-party MXL reader displays correct video (proves real v210), audio and ANC.
4. Controller connects ingest MXL senders to egress MXL receivers (same or second gateway); egress output is verified on an ST 2110 analyser: narrow pacing compliant, 2022-7 both legs, lip-sync error ≤ 1 audio sample block versus source.
5. Pulling one network leg: no visible/audible error; metrics show leg loss.
6. GM failover: `mxlgw_ptp_gm_changes_total` increments, GM identity in UI changes, media continues.
7. Hand-editing the config + restart applies; UI save after external edit is blocked until resolved.
8. AMWA suites IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01: zero failures.
9. Restart with `resume_connections=true` restores all active connections without controller action.
10. Prometheus scrapes `/metrics`; the shipped Grafana dashboard renders all panels with data.
11. Tag `v1.0.0` produces images `1.0.0`, `1.0`, `1`, `latest` on GHCR and a GitHub Release with assets; a push to `main` updates `nightly-dev`.
12. Kubernetes manifests deploy successfully on a node with the SR-IOV device plugin; readiness turns green; another pod on the node reads the flows via the hostPath domain.

---

## 20. Risks and Open Items

| # | Item | Mitigation |
|---|---|---|
| R1 | `st20p` RX external-frame support with v210 output (§6.1) | VERIFY early (phase 2); fallback to MTL-owned frames + SIMD convert into grain (one extra memory pass) |
| R2 | MXL behaviour when writing indices outside the ring window (§5.6) | write a unit/integration test against MXL v1.1.0 before relying on it |
| R3 | Sync-group semantics with continuous (audio) flows (§5.7) | read MXL tests; fallback: per-essence waits with a common target index |
| R4 | `builtin_phc2sys` turns the host wall clock into TAI | default mode `builtin`; preflight check; prominent docs |
| R5 | nmos-cpp interfaces/ports overrides (§4.5, §7.1) | VERIFY in nmos-cpp `fe30384`; integration test with nmos-testing |
| R6 | DDP package discovery inside the container (§14.1) | bundle + log loaded version; fail on PMD "safe mode" |
| R7 | MTL patch drift on MTL upgrades | minimal patch, CI apply-check, upstream PR |
| R8 | Kernel-socket backend multicast behaviour on GitHub runners | veth pair with explicit multicast routes; fall back to self-hosted runner if flaky |
| R9 | Single NIC is a single point of failure | documented limitation; later stage multi-NIC (§21) |
| R10 | License | default MIT (as mxl-decklink); dependency licenses (BSD-3 MTL/DPDK, Apache-2.0 MXL/nmos-cpp) listed in `THIRD_PARTY_NOTICES.md` — owner to confirm |

---

## 21. Later Stage (explicitly out of v1, keep the design open for them)

Multiple port pairs / two NICs per container (lift the `port_pairs` limit, add `groups[].port_pair`); four-port cards as two independent 2022-7 pairs; ST 2110-22; sub-frame latency (MTL slice RX + MXL partial grain commits / `mxlFlowReaderGetGrainSlice`); MXL Fabrics; BCP-008-01/-02 status monitoring (fed by the 2022-7 leg counters); IS-08 audio channel mapping; IS-12 control; "follow SDP" dynamic formats; `video/v210a`; authenticated UI.

---

## 22. Glossary

**PF/VF** physical/virtual PCI function · **PMD** DPDK poll-mode driver · **lcore** DPDK logical core · **PHC** PTP hardware clock · **GM** grandmaster · **TAI** International Atomic Time (PTP timescale) · **grain** one discrete MXL ring-buffer entry (video frame/field, ANC frame) · **continuous flow** MXL sample-indexed ring buffer (audio) · **leg** one of the two 2022-7 paths.
