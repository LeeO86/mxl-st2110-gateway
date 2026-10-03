# Acceptance record

Record of SPECIFICATION.md §19 (v1.0 acceptance) and the hardware items of the implementation phases. Hardware tests are manual (owner decision Q12). Fill in one row per run: date, gateway version (`mxl_st2110_gateway_build_info`), host, result, evidence (log, screenshot, analyser report).

## Test hosts

| Host | CPU / RAM | NIC (NVM, DDP) | OS / kernel | Grandmaster | NMOS registry / controller |
|---|---|---|---|---|---|
| _A_ | | E810 2×25G | | | |
| _B_ | | E810 2×100G | | | |
| lab-1 (iptv-web-lab-1) | 2× Xeon Gold 6136, 187 GB | none (BCM57412 2×10G without link, no E810); kernel backend on veth only | Ubuntu 24.04, 6.8.0-137, Docker 29.6.1 | none (chrony TAI offset 37 s) | nmos-cpp registry for other tests |

## Automated in CI (no hardware)

| Item | Evidence |
|---|---|
| Phase 2: pattern writer → `mxl-verify` round trip, bootstrap rules | `mxl-tests` in the image build |
| Phase 3/4: kernel-backend loopback (v210 bars and frame counter for 60 s, tones, timecode, A/V alignment, 2 × 8 ch audio, leg-R loss) | `tests/integration/loopback.sh` artifact `loopback-verify*.json` |
| §19.8: IS-04-01, IS-05-01, IS-05-02, BCP-007-03-01 | `tests/integration/nmos-testing.sh` artifacts, `docs/conformance.md` |
| §19.13: receiver activated before its flow exists | `tests/integration/late-flow.sh` |
| §19.7 (UI part), §19.2 (export/import ids) | `app-tests`, `web` Vitest, `tests/unit/test_webapi.cpp` |
| Phase 6: `/metrics` lint, dashboard regenerates identically | `check-metrics.sh`, `gen_dashboard.py --check` |

## §19 acceptance criteria

| # | Criterion | Date | Version | Host | Result | Evidence / notes |
|---|---|---|---|---|---|---|
| 1 | Fresh host per README → container ready; setup mode without a config file | 2026-10-03 | 1.0.0 (image `ghcr.io/leeo86/mxl-st2110-gateway:1.0.0`) | lab-1 | Pass (mock backend) | `tests/integration/smoke.sh`: setup mode, exit 78 paths, configured gateway, exit 143. No E810, so not the README host setup |
| 2 | UI: ingest "CAM 1" (1 V + 2 A × 8 ch + 1 ANC) and egress "PGM", redundancy on; export → import on a second instance with `keep_ids=true` reproduces the NMOS ids | | | | | |
| 3 | Controller connects an external 2110 source; `mxl-info` shows the flows; a third-party MXL reader shows correct video (real v210), audio and ANC | | | | | |
| 4 | Ingest MXL Senders → egress MXL Receivers; analyser: narrow pacing compliant, 2022-7 both legs, lip-sync ≤ 1 audio block | | | | | |
| 5 | Pull one network leg: no visible/audible error, leg loss in metrics, PTP stays locked via the other port (`mxl_st2110_gateway_ptp_selected` moves if needed) | | | | | |
| 6 | GM failover with and without a parent change: `mxl_st2110_gateway_ptp_gm_changes_total` increments, UI shows the new GM, re-lock, media continues | | | | | |
| 7 | Hand edit + restart applies; UI save after an external edit is blocked until resolved | | | | | |
| 8 | AMWA suites: zero failures | | | | | CI |
| 9 | Restart with `resume_connections=true` restores all connections | 2026-10-03 | 1.0.0 | lab-1 | Pass (mock backend) | `tests/integration/lifecycle.sh`: MXL Receiver connection restored after a restart |
| 10 | Prometheus scrapes `/metrics`; the shipped dashboard renders all panels with data | | | | | |
| 11 | Tag `v1.0.0` → images `1.0.0`, `1.0`, `1`, `latest` + GitHub Release with assets; push to `main` → `nightly-dev` | | | | | |
| 12 | Kubernetes with the SR-IOV device plugin: readiness green; another pod reads the flows through the hostPath domain | | | | | |
| 13 | Late flow: starts automatically, `no_signal` on writer stop, resumes on restart | 2026-10-03 | 1.0.0 | lab-1 | Pass (kernel backend) | CI `late-flow.sh`, repeated on lab-1 with the published image |
| 14 | *(optional)* Multi-host with mxl-fabrics-agent: host A ingest flow received on host B through a mirror domain; `mxl_st2110_gateway_mxl_reader_info{domain_kind="mirror"}` | | | | | |

## Phase 3 hardware items

| Item | Date | Result | Notes |
|---|---|---|---|
| PTP lock on an E810 PF, GM identity shown in UI and `mxl_st2110_gateway_ptp_info` | | | |
| `mxl_st2110_gateway_clock_mtl_minus_host_tai_ns` within 10 µs with a correctly disciplined host | | | |
| DDP package loaded (`mxl_st2110_gateway_nic_info{ddp_package=…}`, no `ddp_safe_mode` log) | | | |
| `IPC_LOCK` sufficient for vfio pinning on the target containerd (open question O-2) | | | |

## Lab run 2026-10-03 (lab-1, no E810)

`smoke.sh`, `lifecycle.sh`, `late-flow.sh` and `loopback.sh` passed against the published `1.0.0` image on lab-1. `loopback.sh` (kernel backend, two veth legs, 60 s) verified the v210 bars and frame counter, tones, timecode, A/V alignment and the second 8-channel essence; dropping 5 % of leg R left the output intact while `rx_leg_seq_lost_total{leg="r"}` grew from 5498 to 81193. The kernel backend has no pacing and no PTP, so these runs say nothing about §19 items 3–6, 12 or the Phase 3 items. `nmos-testing.sh` was not repeated (CI).
