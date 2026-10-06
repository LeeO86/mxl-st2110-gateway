# Decisions

Owner decisions, deviations from `SPECIFICATION.md` and implementation choices that a reader of the spec would not expect. Each entry: date, context, decision, consequence. Section numbers refer to the spec (Draft 1.2).

## Owner decisions (2026-10-01)

Answers to the questions raised while reviewing Draft 1.0 / 1.1. They are folded into Draft 1.2; listed here with their consequences.

| Id | Context | Decision | Consequence |
|---|---|---|---|
| C1 | `node.http_port` 8080 collides with mxl-decklink under host networking | Keep 8080; solve co-location at deployment level (env override, Compose port mapping, reverse proxy) | `MXLGW_HTTP_PORT` alias; `node.public_address` / `public_port` feed nmos-cpp's `proxy_map`; preflight warns about sibling ports; fabrics examples use 8090 |
| C2 | Environment variables were bootstrap-only | Precedence **env > file > default** for every scalar of `node`, `nic`, `ptp`, `mxl` (mxl-decklink model) | Env-set keys are read-only in the UI, rejected by `/api` with a per-field error, never written back (also not generated ids) |
| C3 | BCP-007-03 says reject an inaccessible `mxl_domain_id`; mxl-fabrics-agent `MIRROR_MODE=on-demand` creates the mirror only after activation | Accept the unknown domain, log `mxl_domain_unknown` (rate-limited), wait | Deliberate deviation from BCP-007-03 (R11); the receiver's `mxl_domain_id` constraint is `{}`; BCP-007-03-01 does not exercise it; documented in `docs/conformance.md` |
| C4 | Fixed 500 ms polling for missing flows | Backoff 500 ms → 5 s, ±10 % jitter, while `master_enable` | `util::Backoff`; `mxl_st2110_gateway_mxl_flow_not_found_total` counts attempts |
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

### 2026-10-01 — DPDK drivers that need rdma-core are not built
- Context: the deps stage has `libibverbs` (pulled in by `libpcap-dev`), so DPDK built the mlx4/mlx5/mana drivers; the runtime image has no rdma-core (§14.1) and DPDK's EAL refuses to start when one PMD plugin cannot be loaded (`libmana.so.1: cannot open shared object file`, found by the kernel-backend loopback test on the runtime image).
- Decision: `-Ddisable_drivers=common/mlx5,net/mlx4,net/mlx5,net/mana,compress/mlx5,crypto/mlx5,regex/mlx5,vdpa/mlx5`; the runtime installs `libatomic1` (net/sfc); the runtime stage fails the image build if any shipped binary or PMD plugin has an unresolved library.
- Consequence: only Intel and other self-contained PMDs are in the image, which is all MTL needs on an E810.

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

### 2026-10-01 — Loopback test: bad audio blocks only when MTL's own counters explain them
- Context: on the kernel-socket backend one MTL scheduler issues ~200 000 `sendto` calls per second for the two video legs and reads the legs' sockets one after the other. On a loaded 4-vCPU runner MTL occasionally misses an audio frame's transmit time and drops it (`*_DROP_WHEN_LATE`, counted in `mxl_st2110_gateway_tx_late_frames_total`), or loses a packet on both legs (its "unrecovered (lost on both)" statistic), which `mxl-verify` sees as a bad audio block. The kernel backend has no pacing guarantees (§17.2, R8).
- Decision: `loopback.sh` accepts bad audio blocks only if late audio frames plus twice the both-legs-lost audio packets of the same window cover them (one lost 1 ms packet can touch two verify blocks); video, ANC, offsets and A/V alignment must always be exact.
- Consequence: the test stays strict about the gateway's data path while tolerating the test backend's scheduling; on DPDK hardware late frames and unrecovered packets must be zero (`docs/performance.md`).
- Addendum: the same rule covers video — invalid video grains (the ingest marks frames with lost packets invalid) are accepted only up to the incomplete, dropped and late video frames the gateways counted in the window; bars, frame counters and timecode must be exact. The rule lives in `tests/integration/lib.sh` (`verify_media`) and is used by `loopback.sh` and `late-flow.sh`.
- Addendum (2026-10-03): a ~150 ms stall of the ingest on a loaded runner (all three essences logged `source_clock_drift` at once) outlasted the 100 ms of audio frame buffers, and MTL dropped 28 blocks before st30p saw them. MTL counts those in `stat_slot_get_frame_fail` (per packet and leg), not in st30p's `stat_frames_dropped`, so the gateway exported no drop and the test failed with an allowance of 0. The gateway now adds those drops to `mxl_st2110_gateway_rx_frames_total{result="dropped"}` (audio as blocks: packets ÷ (packets per block × legs), rounded up; video once per frame), and `verify_media` credits twice the ingest audio blocks dropped in the window, as for both-legs-lost packets. On DPDK hardware they must be zero, like the other counters.

### 2026-10-01 — Kernel backend: MTL scheduler as a sleeping thread; patch 0004
- Context: the first CI run on GitHub's 4-vCPU runners lost most of the loopback media: two gateways each pinned a busy-polling MTL lcore, which left two CPUs for the veth softirqs, the ingest and egress workers, the pattern writers and the verifier. Ingest audio dropped blocks because its worker drained the 16 MTL frame buffers too late, video lost ~3 % of its packets on both legs.
- Decision: on the test-only kernel backend MTL runs its scheduler with `MTL_FLAG_TASKLET_THREAD | MTL_FLAG_TASKLET_SLEEP` (an ordinary thread that sleeps when idle). That exposed a lost-wakeup race in MTL's scheduler sleep (1 s stalls under load), fixed by `patches/mtl/0004-sch-sleep-lost-wakeup.patch`. Audio RX sessions keep 100 ms of frame buffers (at least 16) instead of a fixed 16 on every backend. The `dpdk` backend keeps pinned busy-polling lcores.
- Consequence: the loopback passes on this 4-vCPU VM with two CPUs saturated by other processes; patch 0004 is an upstream candidate.

### 2026-10-01 — Egress groups: one grain rate for video and ANC
- Context: one worker per group reads one grain of every discrete essence per cadence period (§3.6, §5.7). The spec does not forbid mixing rates within a group.
- Decision: semantic rule — all video and ANC essences of an egress group have the same grain rate (field rate for interlaced). Audio is independent (blocks of `block_us`). Ingest groups may mix rates.
- Consequence: per-field error `/groups/<g>/video|anc/<i>/rate`.

### 2026-10-01 — Q14 (no owner answer): SDP errors
- Context: nmos-cpp turns exceptions from its own capability check into HTTP 500.
- Decision (conservative): parse the SDP without nmos-cpp's caps check; the gateway's `validate_staged` maps a format mismatch to 400 with the list of differences; a syntactically malformed SDP stays 500 as nmos-cpp does.
- Consequence: recorded as open question O-1.

### 2026-10-01 — Receivers' group-hint role gets an " Input" suffix
- Context: §7.2 tags every Source, Flow, Sender and Receiver with `<group>:<Role> <n>`, so an essence's Receiver and Sender share a role. AMWA IS-04-01 test_23 requires roles to be unique within a group across Senders and Receivers, and §7.7 requires IS-04-01 to pass with zero failures.
- Decision (conservative, open question O-5): Sources, Flows, Senders and the MXL `flow_def.json` keep `<group>:<Role> <n>`; Receivers use `<group>:<Role> <n> Input` (e.g. `CAM 1:Video 1 Input`).
- Consequence: controllers still see one group per gateway group; the Sender and Receiver of an essence are distinguishable by role. Flow ids and resource ids are unchanged.

### 2026-10-01 — DNS-SD inside the container needs nss-mdns and the host's avahi sockets
- Context: nmos-cpp built against the Avahi compatibility library browses and resolves through the host's avahi-daemon (D-Bus), but resolves the registry's `.local` host name with the system `getaddrinfo` ("Using getaddrinfo, got no addresses for host"), which needs `libnss-mdns` and `/run/avahi-daemon/socket`.
- Decision: the runtime image installs `libnss-mdns`; the Compose files, the Kubernetes Deployments and the nmos-testing script mount `/run/dbus` and `/run/avahi-daemon` from the host (as mxl-decklink documents).
- Consequence: DNS-SD registry discovery works under host networking with the host's Avahi; without Avahi on the host use `node.registry.mode = "static"`.
- Addendum (first GitHub run): on AppArmor hosts (Ubuntu runners) dbus-daemon refuses D-Bus clients confined by Docker's `docker-default` profile, which has no D-Bus rules; nmos-cpp reports `DNSServiceBrowse reported error: -65553` (`kDNSServiceErr_Refused`). The Compose files run the gateway with `security_opt: ["apparmor=unconfined"]`, the Kubernetes manifests with `appArmorProfile: {type: Unconfined}`, the nmos-testing script likewise when AppArmor is enabled. The container stays unprivileged with only `IPC_LOCK` and `SYS_NICE`; a custom AppArmor profile allowing `dbus send … peer=(name=org.freedesktop.Avahi)` is the stricter alternative. The `DNSServiceCreateConnection … -65544` error logged at start comes from nmos-cpp's address-record registration, which Avahi's compatibility layer does not support; it is harmless.

### 2026-10-01 — PTP series only when MTL runs PTP
- Context: with `ptp.mode = external` or the kernel backend MTL has no PTP instance; exporting `mxl_st2110_gateway_ptp_locked 0` showed a red "UNLOCKED".
- Decision: all `mxl_st2110_gateway_ptp_*` series are absent in that case; the dashboard shows "external / no MTL PTP".
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
- Superseded in part on 2026-10-03: the ST 2110 node has its own port and `node.web_port` can move the gateway routes to a separate listener; without `web_port` the routes stay on the MXL node's port as described here.
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
- Consequence: a local CMake build reports `unknown` pins in `mxl_st2110_gateway_build_info` (MXL's own version is read at runtime).

### 2026-10-01 — MXL's own log output
- Context: §13 redirects MTL and DPDK logs into the gateway's JSON stream. MXL v1.1.0 logs through its private spdlog instance.
- Decision: not redirected; its level follows `MXL_LOG_LEVEL`.
- Consequence: rare MXL warnings appear as plain lines on stderr.

### 2026-10-01 — Fuzzing the SDP path through an uninstrumented nmos-cpp
- Context: nmos-cpp is built once with gcc in the deps stage; libFuzzer coverage comes only from instrumented code.
- Decision: `fuzz-sdp` instruments the gateway code and sanitizes the whole process; nmos-cpp's parser runs uninstrumented (crashes and aborts are still found, coverage guidance is weaker).
- Consequence: acceptable for §18 ("never crashes"); a dedicated nmos-cpp fuzz build would need a second nmos-cpp compilation.

### 2026-10-03 — Platform guideline G1–G14: two NMOS nodes in one process
- Context: the platform's MXL registry must not hold the gateway's ST 2110 resources (RTP Senders/Receivers), but a single nmos-cpp node registers everything in its model with one registry.
- Decision: the process runs two nmos-cpp nodes. The **MXL node** (`node.http_port` = `NMOS_PORT`) holds the Sources/Flows/MXL Senders of ingest groups and the MXL Receivers of egress groups and registers with `node.registry`. The **ST 2110 node** (`node.st2110.http_port`, default `NMOS_PORT + 1`) holds the RTP Receivers of ingest groups and the Sources/Flows/RTP Senders of egress groups, with its own `node.st2110.registry` (none by default) and `node.st2110.enabled` (default true). Ids: MXL node = `node.id` or UUIDv5(seed namespace, `"node"`); ST 2110 node = UUIDv5(MXL node id, `"st2110-node"`) or UUIDv5(seed namespace, `"st2110-node"`); each node has its own device, UUIDv5(node id, `"device"`).
- Consequence: an extra listening port (inside the platform's `P`/`P+1` reservation); controllers find RTP resources on the ST 2110 node. Each device now holds either the Receiver or the Sender of an essence, so the BCP-002-01 hints are `<group>:<Role> <n>` again for both (resolves O-5). nmos-testing runs IS-04/IS-05 against both nodes.

### 2026-10-03 — Standard environment names, aliases and conflicts
- Context: G1 requires the platform's names; deployed configurations use the `MXLGW_*` names.
- Decision: the platform names are canonical (`NMOS_*`, `MXL_*`, `WEB_PORT`, `SHUTDOWN_TIMEOUT_S`), the old names stay as aliases. Two variables of one setting with different values are a configuration error (exit 78) instead of "canonical wins".
- Consequence: no silent precedence between an old deployment variable and a new platform variable.

### 2026-10-03 — IP literals only, default host address
- Context: G5: hrefs, `api.endpoints`, IS-05 hrefs and SDP must carry IP literals from `NMOS_HOST_ADDRESS`, default "the first non-loopback IPv4". With host networking on a Kubernetes node the first interface is often a CNI bridge.
- Decision: `node.host_address` (aliases `MXLGW_NODE_HOST_ADDRESS`, `MXLGW_NODE_PUBLIC_ADDRESS`, file key `public_address`) must be an announceable IPv4 literal (no hostname, `0.0.0.0`, `127/8`, link-local, multicast). Default: the deprecated `management_addresses[0]` if set, else the IPv4 of the default-route interface, else the first non-loopback, non-link-local IPv4 (`src/util/net.cpp`). nmos-cpp runs with `href_mode = 2` (addresses) and `host_addresses` = the host address (plus deprecated management addresses). The SDP origin is the media port IP (nmos-cpp `get_origin_address`).
- Consequence: a deployed `public_address` hostname now fails with exit 78 (listed in the CHANGELOG); the default-route rule is a refinement of "first non-loopback" (open question O-7).

### 2026-10-03 — DNS-SD off by default
- Context: G4: `NMOS_DNS_SD` default false; off disables browsing and mDNS; no Avahi/D-Bus.
- Decision: `node.registry.dns_sd` (default null = true only for the deprecated `mode: "dns-sd"`). Off: nmos-cpp `pri` = `highest_pri` = `lowest_pri` = `no_priority` (VERIFIED in `src/nmos/node.cpp`); nmos-cpp connects to the DNS-SD daemon lazily, so nothing touches D-Bus. `node.registry.port` defaults to 3210; the query address/port are reported only (the gateway does not query).
- Consequence: a configuration without `registry` no longer waits for DNS-SD. The minimal configuration and the examples no longer contain `mode: "dns-sd"`; files that do keep DNS-SD.

### 2026-10-03 — Readiness and registration
- Context: G7: `/readyz` 200 only when serving and, with a registry configured, registered.
- Decision: registration is required per node when its registry is configured (`dns_sd` or an address); reasons `nmos_not_registered` (MXL node) and `st2110_nmos_not_registered`. `shutting_down` while stopping.
- Consequence: a node without a registry (peer to peer) is ready; before, the default DNS-SD made every gateway without a registry permanently not ready.

### 2026-10-03 — Seed-derived ids
- Context: G3: `NMOS_SEED` → UUIDv5 for every id and the default output domain id.
- Decision: seed namespace = UUIDv5(URL namespace, `"urn:x-mxl-st2110-gateway:seed:" + seed`). Essence id namespace = UUIDv5(seed namespace, essence `uid`) instead of the `uid` (all §7.3 derivations unchanged on top of it). Domain id without a configured one = UUIDv5(seed namespace, `"mxl-domain:" + name`). With a seed nothing is generated or written back (`node.id` is ignored with the warning `node_id_ignored`).
- Consequence: the same production seed and configuration give the same ids on any host; another seed gives other ids. Without a seed nothing changes.

### 2026-10-03 — Domain id mismatch
- Context: G2: an existing `domain_def.json` with another id must be an error and never be overwritten; §8.3 (owner decision C6) let the file win with a warning and wrote its id back.
- Decision: `domain_id_mismatch` is logged as an **error**, the file is kept and its id used; the write-back of C6 stays for ids that are neither from the environment nor from the seed.
- Consequence: the gateway still starts (the domain's identity is its file); the error is visible on every start while the id comes from the environment or the seed.

### 2026-10-03 — Shutdown, deregistration and cleanup
- Context: G8: SIGTERM → stop media and release MXL, deregister, optionally remove only the own output domain, exit 143 within `SHUTDOWN_TIMEOUT_S` (default 10).
- Decision: order: control thread stopped, groups removed (MTL sessions stopped, writers/readers released), every node resource erased (children first, the node last) so nmos-cpp's registration thread sends the DELETEs, waiting at most min(3 s, timeout/2) for the node's own DELETE, listeners closed, MXL instances and MTL released, then with `mxl.cleanup_on_exit` the configured domains removed — only if their `domain_def.json` carries the gateway's id, no other process holds a writer lock on a flow, the directory holds no nested domain and it is not the MXL root. A watchdog in `main.cpp` exits after `SHUTDOWN_TIMEOUT_S`. Exit 143 (SIGTERM) / 130 (SIGINT); `/api/restart` and import `?restart=true` still exit 0 and never remove domains.
- Consequence: a rolling update with cleanup enabled re-creates the domain (same id) on start; readers in other functions see the flows disappear during the restart.

### 2026-10-03 — Exit 75 for listeners
- Context: G6: a port that cannot be bound → exit 75.
- Decision: `nmosnode::ListenError` from the MXL node, the ST 2110 node and the web/setup listener → `EX_TEMPFAIL` (75). Other runtime failures keep exit 1.
- Consequence: orchestrators can tell port collisions from configuration errors (78).

### 2026-10-03 — Image user and labels
- Context: G11: uid 1000 (root only where hardware needs it), OCI labels incl. `io.dmf.mxl.revision`.
- Decision: `USER 1000:1000` (the base image's `ubuntu` user), `/config` owned by 1000. DPDK deployments run as `0:1000` because VFIO group device nodes are root-owned; group 1000 plus `umask 002` keep MXL files writable for the uid-1000 media functions. `MXL_REVISION` is a pin next to `MXL_REF` (Dockerfile + `ci.yaml`), checked against the cloned commit, and becomes `io.dmf.mxl.revision`; `VCS_REF` → `org.opencontainers.image.revision`.
- Consequence: setup mode and the mock backend run as uid 1000 (smoke and lifecycle tests); the kernel-backend tests run as `0:1000` (hugetlbfs).

### 2026-10-03 — CPUs from the affinity
- Context: platform scan: `nic.lcores`/`app_cpus` fixed in the configuration instead of following the kubelet cpuset.
- Decision: dpdk backend with `nic.lcores` unset: the first `nic.lcore_count` (default 4) CPUs of `sched_getaffinity`, keeping at least one for the gateway's threads; `nic.app_cpus` unset: the rest. kernel/mock backends keep MTL's own choice.
- Consequence: no NUMA preference for derived lcores (set `nic.lcores` explicitly for that).

### 2026-10-03 — Release v1.0.0 before the hardware acceptance
- Context: Phase 9 ties `v1.0.0` to the §19 hardware acceptance; the owner asked to release `v1.0.0` once G1–G14 are met.
- Decision: the owner's request wins; the open hardware items stay listed in `docs/acceptance.md` (O-4).
- Consequence: `v1.0.0` is a stable configuration/API/metrics contract; hardware findings are fixed in `1.x` releases.

### 2026-10-06 — TX pacing selectable
- Context: first run on the platform's E810-XXV hosts (NVM 5.01, ice PMD, DPDK 26.07, 8 TX queues): with MTL's `auto` pacing the rate-limiter test restarted the port, the restart failed (`Failed to add lan txq`, stack dump). After MTL's fallback to TSC, `mtl_init` failed in one run and started in another (card moved to another slot).
- Decision: `nic.tx_pacing` = `auto` (default, unchanged) | `rl` | `tsc`, passed as `mtl_init_params.pacing`; the kernel backend keeps `auto`.
- Consequence: such hosts run with `tsc` (software pacing, no NIC rate limiter); the cause in the ice PMD stays open. Only the host whose E810 came up in single VLAN mode failed; the other one (double VLAN mode, same NVM and DDP) passed the rate-limiter test with `auto`.

### 2026-10-06 — domain_def.json: only the id is required
- Context: on the platform the gateway skipped every domain written by mxl-test-player 1.0.3, mxl-color-corrector 1.0.4 and mxl-fabrics-agent 1.0.2 (`mxl_domain_skipped`, "missing required field 'description'"), so egress stayed in `waiting_for_flow`. Their `domain_def.json` has `id` and `label` but no `description` or `tags`, which BCP-007-03's schema requires. The other media functions read such files.
- Decision: reading needs only `id`; `label` and `description` default to `""`, `tags` to `{}`. A field that is present must still have the schema's type. The gateway's own `domain_def.json` keeps all four fields.
- Consequence: discovery finds domains of every writer, also files that already sit on a host's tmpfs (they are never rewritten). The writers are fixed separately.

## Dependencies

| Dependency | Why | License |
|---|---|---|
| nlohmann/json (Ubuntu `nlohmann-json3-dev`) | JSON everywhere outside nmos-cpp; ordered JSON keeps the user's key order in `gateway.json` | MIT |
| OpenSSL libcrypto | SHA-1 for UUIDv5 (§7.3), SHA-256 ETags | Apache-2.0 |
| doctest 2.4.12 (vendored `third_party/doctest`) | unit tests, as mxl-decklink | MIT |
| Vue 3, Vite, `@vitejs/plugin-vue`, `vite-plugin-singlefile`, Vitest (dev, `web/`) | admin UI built into one embedded HTML file (§11.1) | MIT |
| `@vue/test-utils`, `happy-dom` (dev, `web/`) | component tests of the UI (banners, group create/edit/delete) without a browser (Phase 7) | MIT |
| Python `jsonschema`, `PyYAML` (CI/tooling only) | `tools/validate_configs.py` | MIT |
| clang compiler-rt (deps image) | libFuzzer + sanitizers for `tests/fuzz` | Apache-2.0 WITH LLVM-exception |

All other dependencies are the pinned ones of §2 (MTL, DPDK, MXL, nmos-cpp and their transitive dependencies). See `THIRD_PARTY_NOTICES.md`.

## Open questions

| Id | Question | Conservative choice taken |
|---|---|---|
| O-1 | Q14: should a malformed SDP also be a 400 (it is a client error) instead of nmos-cpp's 500? | 500 kept (nmos-cpp behaviour); format mismatch is 400 |
| O-2 | §15.2: does `CAP_IPC_LOCK` alone lift `RLIMIT_MEMLOCK` enough for vfio DMA pinning on the target containerd, or does the runtime need a memlock ulimit? (**VERIFY** on the cluster) | Resolved 2026-10-06 on the platform's E810 hosts: with `CAP_IPC_LOCK` and a memlock limit of 8 MiB MTL started on vfio-pci. The entrypoint now warns only without `CAP_IPC_LOCK` |
| O-3 | §12.1 names (`mxl_st2110_gateway_*_ns`) are a public interface, but Phase 6 asks that `/metrics` "passes `promtool check metrics`", whose lint rejects abbreviated units. Rename to base units (`_seconds`) before v1.0? | Names kept as specified; `check-metrics.sh` tolerates only that finding |
| O-4 | `docs/acceptance.md` §19 items 3–6, 10–12 and 14 need the E810 hosts, a grandmaster and the operator's controller (Q12: manual) | Templates in `docs/acceptance.md` / `docs/performance.md` |
| O-5 | §7.2 gives Senders and Receivers of an essence the same group-hint role, which IS-04-01 rejects. Which role naming does the owner prefer for Receivers? | Resolved 2026-10-03 by the two NMOS nodes: each device holds either the Receiver or the Sender of an essence, so both use `<group>:<Role> <n>` |
| O-6 | Should the ST 2110 node register with the MXL node's registry when no ST 2110 registry is configured? | No: it runs peer to peer unless `node.st2110.registry` is set (the platform forbids ST 2110 resources in the MXL registry) |
| O-7 | G5 says "default the first non-loopback IPv4"; on Kubernetes nodes the first interface is often a CNI bridge | Default-route interface first, then the first non-loopback, non-link-local address |
| O-8 | Is a domain id mismatch (G2 "log an error") fatal? | Not fatal: error logged, `domain_def.json` kept and its id used |
| O-9 | Should `MXL_CLEANUP_ON_EXIT` also apply to `/api/restart` and import restarts? | No: only SIGTERM/SIGINT |
| O-10 | Can DPDK run as uid 1000 (VFIO device ownership from the security context, memlock without `CAP_IPC_LOCK`)? Untested without hardware | Examples run the gateway as `0:1000` |
| O-11 | Should `_ns` metric names move to base units before the v1.0 contract (O-3)? | Kept; a rename would be a 2.0 change |
