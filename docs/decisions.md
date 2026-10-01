# Decisions

Owner decisions, deviations from `SPECIFICATION.md` and implementation choices that a reader of the spec would not expect. Each entry: date, context, decision, consequence. Section numbers refer to the spec (Draft 1.2).

## Owner decisions (2026-10-01)

Answers to the questions raised while reviewing Draft 1.0 / 1.1. They are folded into Draft 1.2; listed here with their consequences.

| Id | Context | Decision | Consequence |
|---|---|---|---|
| C1 | `node.http_port` 8080 collides with mxl-decklink under host networking | Keep 8080; solve co-location at deployment level (env override, Compose port mapping, reverse proxy) | `MXLGW_HTTP_PORT` alias; `node.public_address` / `public_port` feed nmos-cpp's `proxy_map`; preflight warns about sibling ports; fabrics examples use 8090 |
| C2 | Environment variables were bootstrap-only | Precedence **env > file > default** for every scalar of `node`, `nic`, `ptp`, `mxl` (mxl-decklink model) | Env-set keys are read-only in the UI, rejected by `/api` with a per-field error, never written back (also not generated ids) |
| C3 | BCP-007-03 says reject an inaccessible `mxl_domain_id`; mxl-fabrics-agent `MIRROR_MODE=on-demand` creates the mirror only after activation | Accept the unknown domain, log `mxl_domain_unknown` (rate-limited), wait | Deliberate deviation from BCP-007-03 (R11); the receiver's `mxl_domain_id` constraint is `{}`; BCP-007-03-01 does not exercise it; documented in `docs/conformance.md` |
| C4 | Fixed 500 ms polling for missing flows | Backoff 500 ms → 5 s, ±10 % jitter, while `master_enable` | `util::Backoff`; `mxlgw_mxl_flow_not_found_total` counts attempts |
| C5 / Q8 | Flow id only from the essence uid kept the id across format changes | Flow id = UUIDv5(essence uid, `"flow:" + canonical format`) | A format edit mints a new flow; NMOS is updated before the new writer commits (§7.3) |
| C6 | Config id vs. existing `domain_def.json` | `domain_def.json` wins; the adopted id is written back to the config | `domain_id_mismatch` warning, then write-back; skipped when the id comes from the environment |
| C7 | Host MXL root path | `/Volumes/mxl` like the siblings; each container maps it to its own path | Identity always comes from `domain_def.json`, never from a path |
| Q1 | RTP timestamp of egress essences | Transmit time `T(i) + output_delay` for every essence (lip-sync by the common delay) | Verified in the loopback test: ingest index = source index + 2 grains, A/V misalignment 0 samples |
| Q2 | Default output delay | Two grains | `output_delay_ns: null` = 2 × cadence; minimum one grain + largest read offset + 2 ms |
| Q3 | Data missing at the deadline | Black / silence / empty ANC; per group `missing_data: "repeat"` | Repeat copies the last good grain once; audio and ANC are never repeated |
| Q4 | 720p | Dropped from v1 | Size rule 1920×1080 or 3840×2160 |
| Q5 | PTP on 2022-7 | Patches 0001–0003; PTP on both ports with BMCA across them | See "MTL patches" below |
| Q6 | MAC / link for NMOS interfaces and metrics | DPDK ethdev API (`rte_eth_macaddr_get`, `rte_eth_link_get_nowait`); kernel backend: sysfs | `src/mtl/ethdev.cpp` is the only TU that sees DPDK headers |
| Q7 | `enabled` and first start | Without saved state the gateway stages and activates its defaults: ST 2110 legs from `defaults.legs` and MXL Senders enabled, MXL Receivers `auto`/`null` and disabled; with saved state (`resume_connections`) `state/connections.json` is restored | Runs stand-alone without a controller; disabled groups register no resources |
| Q9 | Garbage collection at start | Only the gateway's own stale flows; domain-wide only with `gc_on_start` | `removeStaleFlows` tests the writer lock with `flock(LOCK_EX\|LOCK_NB)` |
| Q10 | ANC of interlaced formats | Field rate; MTL's 20-packet / 8-bit UDW limits accepted | ANC grain rate = 2 × frame rate for interlaced groups |
| Q11 | Delivery | One full implementation on the branch, then fixes | — |
| Q12 | Hardware tests | Stay manual | `docs/acceptance.md`, `docs/performance.md` are templates to fill on the E810 hosts |
| Q13 | nmos-testing registry | Mock registry over multicast DNS-SD, no registry container | `tests/integration/nmos-testing.sh` needs avahi-daemon on the host |

## Implementation decisions and deviations

### 2026-10-01 — DPDK built with `-Dplatform=generic`
- Context: MTL's `script/build_dpdk.sh` builds for `-march=native` of the build host.
- Decision: same steps (MTL DPDK patches, meson) but `-Dplatform=generic` (also in §14.1).
- Consequence: the image runs on any x86-64 host; MTL keeps its runtime SIMD dispatch. Only `src/mtl/ethdev.cpp` is compiled with DPDK's pkg-config flags.

### 2026-10-01 — Test-only `mock` media backend
- Context: unit tests, the whole-application tests and the NMOS conformance run need a gateway without DPDK, hugepages or packets. §17.2 names only `dpdk` and `kernel`.
- Decision: `nic.backend = "mock"`: no network; TX sessions deliver frames to RX sessions of the same process whose group address and port match, at the frame's transmit time (`CLOCK_TAI`). It is in the schema and flagged test-only in preflight, the UI and the `/readyz` warnings.
- Consequence: `tests/unit/test_pipeline.cpp` and `test_application.cpp` run real MXL flows through ingest and egress without a NIC; `smoke.sh` needs no hugepages. The conformance run uses the kernel backend as §7.7 asks.

### 2026-10-01 — A cleanly stopped writer deletes its flow (`no_signal`, reason `flow_removed`)
- Context: §5.8 expects a stopped source to leave the flow in place so the reader sees "no new grains". In MXL v1.1.0 readers open flows read-only without a lock (`lib/internal/src/Instance.cpp:135`, `SharedMemory.cpp:56`), so the last writer's clean release deletes the flow directory.
- Decision: when a flow that was attached disappears, the essence goes to `no_signal` (reason `flow_removed`), keeps retrying with backoff and re-attaches when the flow is re-created. A flow that never existed stays `waiting_for_flow`.
- Consequence: matches §19 item 13 ("`no_signal`, not `error`"); `late-flow.sh` checks the sequence.

### 2026-10-01 — MTL patches: opt-in flags instead of changed defaults
- Context: §5.5 proposed `ptp_domain = -1` for "accept any domain" and one patch for dual-port BMCA.
- Decision: 0002 adds `MTL_FLAG_PTP_DOMAIN_FILTER` (bit 56) plus `uint8_t ptp_domain`; without the flag MTL behaves as upstream. 0003 is split into `MTL_FLAG_PTP_BMCA` (57, BMCA on Announce, parent re-selection) and `MTL_FLAG_PTP_DUAL_PORT` (58, PTP instances on both ports, the selected one steers the PHC). Feature macros `MTL_HAS_PTP_STATUS`, `MTL_HAS_PTP_DOMAIN_FILTER`, `MTL_HAS_PTP_DUAL_PORT` let the gateway build against an unpatched MTL.
- Consequence: the patches are upstreamable as independent features (`docs/upstream/mtl-ptp-status.md`); bits 56–58 must be re-checked on every `MTL_REF` bump (R7).

### 2026-10-01 — MTL RX queue budget from the configuration
- Context: every MTL RX queue allocates about 6.6 MB of mbufs; the default queue count exhausted the mempool on small hugepage pools.
- Decision: `queueBudget()` sizes `rx_queues_cnt` / `tx_queues_cnt` with MTL's `st_rx/tx_sessions_queue_cnt()` from the configured sessions plus a headroom of 4 sessions per essence type and direction (1 on the kernel backend) for groups added live.
- Consequence: sessions beyond the headroom fail to start (`ingest_receiver_failed` / `egress_sender_failed`, essence state `error` on ingest) until a restart re-sizes the queues.

### 2026-10-01 — Kernel-backend tests need a second network namespace
- Context: with both veth ends in one namespace the kernel drops the multicast as martian (source address is local).
- Decision: the ingest side of `loopback.sh` / `late-flow.sh` runs in the network namespace of a helper container.
- Consequence: tests need `sudo` for `ip`/`nsenter`; documented in `tests/integration/lib.sh`.

### 2026-10-01 — Egress groups: one grain rate for video and ANC
- Context: one worker per group reads one grain of every discrete essence per cadence period (§3.6, §5.7). The spec does not forbid mixing rates within a group.
- Decision: semantic rule — all video and ANC essences of an egress group have the same grain rate (field rate for interlaced). Audio is independent (blocks of `block_us`). Ingest groups may mix rates.
- Consequence: per-field error `/groups/<g>/video|anc/<i>/rate`.

### 2026-10-01 — Q14 (no owner answer): SDP errors
- Context: nmos-cpp turns exceptions from its own capability check into HTTP 500.
- Decision (conservative): parse the SDP without nmos-cpp's caps check; the gateway's `validate_staged` maps a format mismatch to 400 with the list of differences; a syntactically malformed SDP stays 500 as nmos-cpp does.
- Consequence: recorded as open question O-1.

### 2026-10-01 — PTP series only when MTL runs PTP
- Context: with `ptp.mode = external` or the kernel backend MTL has no PTP instance; exporting `mxlgw_ptp_locked 0` showed a red "UNLOCKED".
- Decision: all `mxlgw_ptp_*` series are absent in that case; the dashboard shows "external / no MTL PTP".
- Consequence: alert rules should use `absent()` only together with the configured mode.

### 2026-10-01 — Imports are blocked after a hand edit
- Context: §9.2 blocks "UI saves" while the file changed on disk; §9.4 does not say whether an import is such a save.
- Decision: `POST /api/config/import` returns 409 like every other write until the operator restarts or overwrites.
- Consequence: an import can never silently discard a hand edit.

### 2026-10-01 — nmos-cpp CMake package workarounds
- Context: nmos-cpp at `fe30384` installs a CMake package whose config misses `find_dependency(websocketpp)` and references `include/third_party`, which `cmake --install` does not create.
- Decision: `cmake/MxlgwDependencies.cmake` calls `find_package(websocketpp)` first; the Dockerfile copies `Development/third_party/{jwt-cpp,nlohmann}` into `/opt/nmos-cpp/include/third_party`.
- Consequence: drop both when nmos-cpp fixes its package.

### 2026-10-01 — Single-port HTTP with nmos-cpp
- Context: §10 wants every route on `node.http_port`.
- Decision: mount the gateway router on nmos-cpp's node listener (`server.api_routers[{{}, node_port}]`) after removing nmos-cpp's catch-all handler, then re-add it (VERIFIED in `src/nmos/http_adapter.hpp`). Setup mode uses a bare cpprest listener with the same router.
- Consequence: `/x-nmos/…` stays nmos-cpp's; the gateway routes never shadow it.

### 2026-10-01 — The Kubernetes fabrics scenario is a self-contained kustomization
- Context: §15.2 asks for `deploy/k8s/fabrics/` as an overlay of `deploy/k8s/`; kustomize refuses an overlay inside its own base directory (cycle).
- Decision: `deploy/k8s/fabrics/kustomization.yaml` defines both gateways itself (node A ingest, node B egress); the SR-IOV ConfigMap stays in the base.
- Consequence: changes to `deploy/k8s/deployment.yaml` must be mirrored in `fabrics/gateway-*.yaml`; both pass kubeconform in CI.

### 2026-10-01 — Pins only in the Dockerfile and `ci.yaml`
- Context: AGENTS.md hard rule.
- Decision: CMake reports the pins it is given (`-DMXLGW_PIN_*`, passed by the Dockerfile build stage) and defaults to `unknown`; `nmos-testing.sh` reads `NMOS_TESTING_REF` from `ci.yaml`; `container.yaml` uses the Dockerfile defaults; a CI step compares both files.
- Consequence: a local CMake build reports `unknown` pins in `mxlgw_build_info` (MXL's own version is read at runtime).

### 2026-10-01 — MXL's own log output
- Context: §13 redirects MTL and DPDK logs into the gateway's JSON stream. MXL v1.1.0 logs through its private spdlog instance.
- Decision: not redirected; its level follows `MXL_LOG_LEVEL`.
- Consequence: rare MXL warnings appear as plain lines on stderr.

### 2026-10-01 — Fuzzing the SDP path through an uninstrumented nmos-cpp
- Context: nmos-cpp is built once with gcc in the deps stage; libFuzzer coverage comes only from instrumented code.
- Decision: `fuzz-sdp` instruments the gateway code and sanitizes the whole process; nmos-cpp's parser runs uninstrumented (crashes and aborts are still found, coverage guidance is weaker).
- Consequence: acceptable for §18 ("never crashes"); a dedicated nmos-cpp fuzz build would need a second nmos-cpp compilation.

## Dependencies

| Dependency | Why | License |
|---|---|---|
| nlohmann/json (Ubuntu `nlohmann-json3-dev`) | JSON everywhere outside nmos-cpp; ordered JSON keeps the user's key order in `gateway.json` | MIT |
| OpenSSL libcrypto | SHA-1 for UUIDv5 (§7.3), SHA-256 ETags | Apache-2.0 |
| doctest 2.4.12 (vendored `third_party/doctest`) | unit tests, as mxl-decklink | MIT |
| Vue 3, Vite, `@vitejs/plugin-vue`, `vite-plugin-singlefile`, Vitest (dev, `web/`) | admin UI built into one embedded HTML file (§11.1) | MIT |
| Python `jsonschema`, `PyYAML` (CI/tooling only) | `tools/validate_configs.py` | MIT |
| clang compiler-rt (deps image) | libFuzzer + sanitizers for `tests/fuzz` | Apache-2.0 WITH LLVM-exception |

All other dependencies are the pinned ones of §2 (MTL, DPDK, MXL, nmos-cpp and their transitive dependencies). See `THIRD_PARTY_NOTICES.md`.

## Open questions

| Id | Question | Conservative choice taken |
|---|---|---|
| O-1 | Q14: should a malformed SDP also be a 400 (it is a client error) instead of nmos-cpp's 500? | 500 kept (nmos-cpp behaviour); format mismatch is 400 |
| O-2 | §15.2: does `CAP_IPC_LOCK` alone lift `RLIMIT_MEMLOCK` enough for vfio DMA pinning on the target containerd, or does the runtime need a memlock ulimit? (**VERIFY** on the cluster) | Manifests rely on `IPC_LOCK`; the entrypoint warns when memlock is not unlimited |
| O-3 | §12.1 names (`mxlgw_*_ns`) are a public interface, but Phase 6 asks that `/metrics` "passes `promtool check metrics`", whose lint rejects abbreviated units. Rename to base units (`_seconds`) before v1.0? | Names kept as specified; `check-metrics.sh` tolerates only that finding |
| O-4 | `docs/acceptance.md` §19 items 3–6, 10–12 and 14 need the E810 hosts, a grandmaster and the operator's controller (Q12: manual) | Templates in `docs/acceptance.md` / `docs/performance.md` |
