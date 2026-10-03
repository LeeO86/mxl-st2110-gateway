# mxl-st2110-gateway — Implementation Plan

Work strictly phase by phase. A phase is done only when **all** its acceptance criteria pass in CI (or, for hardware items, are recorded in `docs/acceptance.md`). Section numbers refer to `SPECIFICATION.md`.

Every **VERIFY** item touched in a phase MUST be resolved in that phase: confirm in the pinned source, then add a unit test or a code comment `// VERIFIED: <repo>@<pin> <file>:<line> — <finding>`. If a VERIFY fails, record the deviation in `docs/decisions.md` and apply the fallback named in the spec (§20).

---

## Phase 0 — Skeleton, pins, CI scaffolding

- Repository layout (§3.2), CMake/Ninja, C++20, `-Wall -Wextra -Werror`, doctest, clang-format config.
- `docker/Dockerfile` stages `webui`, `deps`, `build`, `runtime` (§14.1) with all pins as build args; `deps` builds DPDK 26.07 (MTL script), MTL v26.09, MXL v1.1.0 (Fabrics OFF, tools ON), nmos-cpp `fe30384`.
- `patches/mtl/0001-ptp-status-api.patch` placeholder + CI apply-check job.
- `.github/workflows/ci.yaml` (build + unit tests), `container.yaml` (tags per §16.2), `release.yaml` logic (§16.3, can be a job inside `container.yaml`).
- `LICENSE` (MIT, pending owner confirmation), `THIRD_PARTY_NOTICES.md`, `CHANGELOG.md`, `docs/decisions.md`, `docs/references.md`.

**Accept:** CI green on an empty `main.cpp` that prints versions of MTL, DPDK, MXL (`mxlGetVersion`) and nmos-cpp; image builds; a push to `main` publishes `nightly-dev` and `git-<sha>`; a test tag `v0.0.1` publishes `0.0.1`, `0.0`, `0`, `latest` and creates a GitHub Release with placeholder assets (delete afterwards).

## Phase 1 — Pure core (no MTL, no MXL I/O)

- `config/`: JSON Schema (§9.5), semantic validation, atomic store with `.bak`, ETag, mtime watch, import/export logic, `keep_ids`.
- `nmos/ids`: UUIDv5 derivation (§7.3).
- `timing/rtpclock`: §5.4 functions + all mandatory tests.
- `codec/audioconv` (§6.2), `codec/anc8331` (§6.3) with golden vectors.
- `mtl/sdp_map` (pure mapping from nmos-cpp `sdp_parameters` to an MTL-independent session description struct) with SDP fixtures (§17.1).
- libFuzzer targets for `sdp_map` and `anc8331`.

**Accept:** ≥ 90 % line coverage on these modules; fuzzers run 60 s each in CI without findings.

## Phase 2 — MXL bridge and domains

- `mxlbridge/domain`: bootstrap §8.3 (tmpfs/overlay check, `domain_def.json` adopt/create, `options.json`, GC), exit 78 paths.
- Writers/readers for video (grain), audio (samples, multi-buffer), ANC (grain); flow descriptor generation = IS-04 Flow body (§7.2); flow sync group wrapper.
- `tools/mxl-pattern-writer` and `tools/mxl-verify` (§17.3).
- Resolve VERIFY items R2, R3 (§20) with tests against real MXL.

**Accept:** pattern-writer → mxl-verify round trip in CI; `mxl-info` and `mxl-data-probe` read the flows; bootstrap tests: missing dir created, existing `domain_def.json` never modified, non-tmpfs path exits 78.

## Phase 3 — MTL instance, PTP, video both directions

- `mtl/instance`: EAL init, port pair (§4), backends `dpdk` / `kernel`, lcores, PF/VF detection, PTP modes (§5.2), clock supervision (§5.3).
- Implement the MTL PTP status patch (§5.5) incl. per-Announce GM tracking.
- Video ingest (§6.1) with v210 conversion into the grain (resolve R1) and video egress with user timestamp/pacing.
- Kernel-backend loopback test for video (`tests/integration/loopback.sh`, video only).

**Accept:** CI loopback video: frame counter continuous for 60 s, no unconverted RFC 4175 in grains (verify pixel values of the colour bars); unit test of the patch API via a stub; on hardware (record in `docs/acceptance.md`): PTP lock on E810 PF, GM identity shown, `mxlgw_clock_mtl_minus_host_tai_ns` within 10 µs with a correctly configured host.

## Phase 4 — Audio, ANC, ST 2022-7, egress sync

- Audio ingest/egress (§6.2), ANC ingest/egress (§6.3), egress sync groups and `output_delay` (§5.7), redundancy on both directions (§4.3), `update_source`/`update_destination` paths (§7.5).

**Accept:** CI loopback full group (1 V + 2 A × 8 ch + 1 ANC): tone frequency/level exact, timecode continuous, A/V alignment within ±1 audio block; leg-loss simulation on the kernel backend (drop leg R) keeps output intact and increments `mxlgw_rx_leg_seq_lost_total{leg="r"}`.

## Phase 5 — NMOS

- nmos-cpp node with all routes on one port (§7.1, §10), node interfaces from DPDK ports (§4.5), resources per group (§7.2), MXL IS-05 (§7.4), 2110 IS-05 incl. SDP parsing/generation (§7.5), format validation (§6.4), connection persistence (§7.6), live group add/edit/remove with re-registration (§9.3).
- `tests/integration/nmos-testing.sh` with pinned nmos-testing and a nmos-cpp registry.

**Accept:** IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01 — zero failures in CI; restart restores active connections; ids stable across restart and rename.

## Phase 6 — Operations

- `/livez`, `/readyz` (all reasons), `/statusz`, `/metrics` (§12.1, documented in `docs/metrics.md`), preflight (§14.3), structured logging incl. MTL log redirection (§13), Grafana generator + committed dashboard (§12.2), Prometheus scrape example, ServiceMonitor.

**Accept:** metrics endpoint passes `promtool check metrics`; dashboard JSON regenerates identically; every preflight failure has a test and a README anchor.

## Phase 7 — Admin web UI

- Vue 3 single-file UI (§11) with all tabs, group creation by counts, per-field server validation errors, import/export, changed-on-disk handling, restart, preflight view.
- REST API (§11.3) with ETag/If-Match and Origin check.

**Accept:** Playwright (or Vitest + API tests) covering: create/edit/delete group, import/export round trip, conflict 412, changed-on-disk banner; UI works with the `kernel` backend in CI.

## Phase 8 — Deployment and documentation

- `docker/docker-compose.yaml` (§15.1), `deploy/k8s/*` (§15.2) passing `kubeconform`, README (§18 documentation list), `docs/configuration.md` generated from the schema, host preparation guide (§15.3), conformance report, release assets (§16.3).

**Accept:** `docker compose config` and `kubeconform` green; README quick start reproduced on a clean host; `v1.0.0-rc1` tag produces all images and release assets.

## Phase 9 — Hardware acceptance and performance

- Execute §19 acceptance criteria on E810 2×25G and 2×100G with the operator's PTP and NMOS environment; measure §18 capacity targets; record in `docs/acceptance.md` and `docs/performance.md`; fix findings.

**Accept:** all §19 items pass → tag `v1.0.0`.

---

## Platform guideline G1–G14 (MXL PoC platform, v1.0.0 contract)

Audit of `main` at `bf66966` against the platform guideline, before the v1.0.0 work. Status: **met**, **gap** or **N/A**. Evidence is `file:line` at that commit.

| # | Requirement | Status | Evidence (bf66966) | Planned change |
|---|---|---|---|---|
| G1 | env > file > defaults; unknown env ignored; invalid value → exit 78 with a clear message | met | `src/config/env.cpp:242-287` (only listed variables are read), `env.cpp:256-259`, `src/app/application.cpp:226-231` | alias conflicts (canonical and alias set to different values) become exit 78 instead of "canonical wins" |
| G1 | every setting in one table (README or SPEC) | gap | `docs/configuration.md` has one table per section, none in README/SPEC | generated single settings table in README (`tools/gen_config_docs.py`) |
| G1 | own state only under one configurable dir (default `/config`) | met | `src/app/application.cpp:30-33` (`state/` beside the config file), `src/config/store.cpp:186-199`, `MXLGW_CONFIG` | document |
| G1 | secrets never logged | met | no secrets in the configuration; TLS key is a file path (`schema/gateway-config.schema.json:209-216`) | document |
| G2 | `MXL_DOMAIN_SCAN_PATH` (default `/Volumes/mxl`) | met | `src/config/env.cpp:80` (alias), `src/config/config.hpp:183` | make it the canonical name |
| G2 | `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`; create the domain if missing | gap | only `MXLGW_MXL_DOMAIN_<NAME>_*` for domains already in the file (`env.cpp:307-327`); creation `src/mxlbridge/bootstrap.cpp:92-98` | map to `mxl.domains[0]` (created as `main` when the file has none) |
| G2 | existing `domain_def.json` with another id → error, not overwritten | gap | `bootstrap.cpp:106-114` logs a **warning** | log `domain_id_mismatch` as error; never overwrite |
| G2 | never write into another function's domain; never rewrite domain files on every start | met | `bootstrap.cpp:67-90` (mirror refusal), `bootstrap.cpp:100-140`, `142-173` (create only if missing) | — |
| G2 | `history_duration` configurable | met | `schema/gateway-config.schema.json:458-467`, `env.cpp:324` | add `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS` |
| G3 | `NMOS_SEED` → UUIDv5 for node, device, sources, flows, senders, receivers, default output domain id | gap | node id from `node.id` (`src/nmos/node.cpp:190-191`), essence ids from config uids (`src/nmos/ids.cpp:8-16`), domain id generated (`bootstrap.cpp:122`) | `node.seed`: seed namespace for every id, no write-back of seed-derived ids |
| G3 | `NMOS_LABEL` = node label and device label prefix | gap | `MXLGW_NODE_LABEL` only (`env.cpp:57`), `node.cpp:536-539` | `NMOS_LABEL` canonical, `MXLGW_NODE_LABEL` alias |
| G3 | `NMOS_TAGS` JSON object on node and device; BCP-002 group hints kept | gap | group hints `node.cpp:106-113`; no node/device tags | `node.tags` (`NMOS_TAGS`) |
| G4 | `NMOS_REGISTRY_ADDRESS`/`_PORT`, `NMOS_QUERY_ADDRESS` (default registry address), `NMOS_QUERY_PORT` (default port + 1) | gap | `MXLGW_NODE_REGISTRY_*` (`env.cpp:65-67`), static only with `mode: static` (`node.cpp:462-467`) | standard names canonical, query settings reported in `/api/nmos` |
| G4 | `NMOS_DNS_SD` default false; off = no browse, no mDNS; no Avahi/D-Bus needed | gap | default `dns-sd` (`config.hpp:192`); `pri`/`highest_pri` never set (`node.cpp:420-479`); Avahi mounts in `docker/docker-compose.yaml:36-39`, `deploy/k8s/deployment.yaml:57-61,80-81,100-108` | `node.registry.dns_sd` (default false, legacy `mode` maps onto it); off → `pri` = `highest_pri` = `no_priority`; drop the Avahi mounts |
| G5 | announce IP literals only, from `NMOS_HOST_ADDRESS`, default first non-loopback IPv4; old settings as aliases | gap | `node.cpp:438-452`: empty management addresses → nmos-cpp announces every interface address (CNI included); `public_address` may be a hostname (`schema:124-136`); `href_mode` not set | `node.host_address` (IPv4 literal, validated), `href_mode` = addresses, `public_address`/`MXLGW_NODE_PUBLIC_ADDRESS` as aliases |
| G6 | every listening port configurable by env, no hard-coded ports, two instances per host | met | single port `node.http_port` (`env.cpp:59`), optional APIs disabled (`node.cpp:431-437`) | add `NMOS_PORT`, `WEB_PORT` (optional separate UI/API port) and the ST 2110 node port |
| G6 | port cannot be bound → exit 75 | gap | `application.cpp:366-370` returns 1 | exit 75 (`EX_TEMPFAIL`) for every listener |
| G7 | `/livez`; `/readyz` 200 only when serving and, with a registry configured, registered | gap | `src/ops/webapi.cpp:159-165`; `src/ops/health.cpp:51` requires registration whenever NMOS runs, also without a registry | registration required only for a configured registry (per node) |
| G7 | `/metrics` with prefix `mxl_st2110_gateway_` | gap | prefix `mxlgw_` (`src/ops/metrics_export.cpp`) | rename (code, docs, dashboard, tests) |
| G8 | SIGTERM → stop media and release MXL, deregister, optional own-domain cleanup (`MXL_CLEANUP_ON_EXIT`), exit 143 within `SHUTDOWN_TIMEOUT_S` (default 10) | gap | `src/main.cpp:65-86` exits 0; `application.cpp:409-449` sets `model.shutdown` only (no DELETE); no cleanup; no timeout | ordered shutdown, resources erased before shutdown (DELETEs), cleanup of configured domains, watchdog, exit 143 (SIGINT 130) |
| G9 | Senders report active `mxl_domain_id`/`mxl_flow_id`; Receivers take a staged PATCH; `master_enable=false` stops reading; connection survives restart (SHOULD) | met | `node.cpp:757-783`, `787-829`, `594-623` (`resume_connections`); BCP-007-03-01 in CI (`tests/integration/nmos-testing.sh`) | restart persistence covered by the new lifecycle test |
| G10 | `GET /api/v1/config/export`, `POST /api/v1/config/import` restore the configuration; secrets omitted unless requested | gap | `/api/config/export|import` only (`webapi.cpp:222-249`); no secrets in the file | `/api/v1/*` aliases of every `/api` route; import `?restart=true`; document "no secrets" |
| G11 | CI pushes `ghcr.io/leeo86/mxl-st2110-gateway`: main → `git-<sha7>` + `nightly-dev`; `vX.Y.Z` → `X.Y.Z`, `X.Y`, `X` | met | `.github/workflows/container.yaml:57-72` (also `latest` on releases) | — |
| G11 | runtime uid 1000 (root only where hardware needs it, documented) | gap | no `USER` in `docker/Dockerfile:158-204` | `USER 1000:1000`; DPDK deployments run as root (VFIO), documented |
| G11 | OCI labels `source`, `revision`, `licenses`, `io.dmf.mxl.revision` | gap | `Dockerfile:196-200` (no source/revision outside CI, no `io.dmf.mxl.revision`) | build args `VCS_REF`, `MXL_REVISION` (pinned, verified against the clone) |
| G11 | tags never moved; examples reference existing tags only | gap | manifests use the moving `nightly-dev` (`deploy/k8s/deployment.yaml:28,37`) | reference `:1.0.0` |
| G12 | k8s example: host network allowed, standard env names, probes `/livez` `/readyz`, `terminationGracePeriodSeconds` > `SHUTDOWN_TIMEOUT_S`, MXL root hostPath, writable `/config`, no `hostIPC`, minimal capabilities | gap | `deployment.yaml:23,25,64-75,77-108`; legacy env names, AppArmor unconfined and D-Bus/Avahi mounts | standard env, no Avahi/AppArmor, `SHUTDOWN_TIMEOUT_S` |
| G13 | README settings table, ports, exit codes 0/75/78/143, API list, platform section; CHANGELOG 1.0.0; SPEC matches code | gap | README has none of these sections | write them |
| G14 | unit tests for config and new behaviour; integration test start → ready → SIGTERM → deregistered, own domain removed; CI green | gap | `tests/unit/test_env.cpp`, `test_config.cpp`; no lifecycle test | unit tests + `tests/integration/lifecycle.sh` with a mock registry |
| — | ST 2110-side NMOS resources must not register with the platform's MXL registry | gap | one nmos-cpp node holds RTP and MXL resources (`node.cpp:625-856`) | two nodes in one process: MXL node (platform registry) and ST 2110 node (own port, own optional registry) |
| — | `nic.lcores`/`app_cpus` from the kubelet cpuset | gap | fixed in the config (`src/mtl/mtl_backend.cpp:412-416`) | dpdk backend: empty `nic.lcores` → first `nic.lcore_count` CPUs of `sched_getaffinity`, `app_cpus` = the rest |
