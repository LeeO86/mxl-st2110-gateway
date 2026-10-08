# MTL patches

Applied in order to MTL `v26.09` by `docker/Dockerfile` (`git apply --check` first). CI fails if a patch does not apply or MTL does not build with them.

| Patch | Purpose | New API | Spec |
|---|---|---|---|
| `0001-ptp-status-api.patch` | Status of every PTP instance: lock state, parent and grandmaster data, offset and path-delay statistics, cumulative counters, GM change counter. Every Announce of the parent is parsed, so a grandmaster change behind it is seen. Snapshot published under a writer spinlock + seqlock. | `struct mtl_ptp_status`, `mtl_ptp_get_status()`, `MTL_HAS_PTP_STATUS` | §5.5 |
| `0002-ptp-domain-filter.patch` | Ignore PTP messages of other domains. | `MTL_FLAG_PTP_DOMAIN_FILTER`, `mtl_init_params.ptp_domain`, `MTL_HAS_PTP_DOMAIN_FILTER` | §5.5 |
| `0003-ptp-dual-port-bmca.patch` | Best-master selection per instance and parent re-selection after 3 missed Announce intervals; PTP on both ports of a 2022-7 pair with a BMCA across them, only the selected instance steers the shared PHC and answers `mtl_ptp_read_time`. | `MTL_FLAG_PTP_BMCA`, `MTL_FLAG_PTP_DUAL_PORT`, `MTL_HAS_PTP_DUAL_PORT` | §4.3, §5.5 |
| `0004-sch-sleep-lost-wakeup.patch` | Bug fix: with `MTL_FLAG_TASKLET_SLEEP` the scheduler armed its wake-up alarm before taking the wake mutex, so an alarm that expired first (loaded host) was lost and the scheduler slept for the 1 s safety timeout, stalling every session. The alarm is now armed with the mutex held. | — | §17.2 (the gateway uses tasklet sleep on the test-only kernel backend) |
| `0005-main-lcore-remap.patch` | Bug fix: with DPDK ≥ 25.11 MTL passes `--remap-lcore-ids`, which numbers the `-l` CPUs from 0 in ascending order, but gave `--main-lcore` the CPU number: any `main_lcore` other than CPU 0 failed with "Main lcore is not enabled for DPDK". It now passes the renumbered id. Needed for a main lcore inside a Kubernetes pod's exclusive CPUs (gateway 1.0.10). | – | §17 |

All behaviour changes of 0001–0003 are opt-in through new flags; without them MTL behaves as `v26.09`. The gateway detects the patches through the `MTL_HAS_*` macros and falls back to "PTP status unavailable" without them.

Regenerating: apply the patches to a `v26.09` checkout, edit, and export each one with `git diff` against the previous commit. Upstream rationale: `docs/upstream/mtl-ptp-status.md`.
