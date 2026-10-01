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
