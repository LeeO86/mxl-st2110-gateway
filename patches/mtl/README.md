# MTL patches

Applied in order to MTL `v26.09` by `docker/Dockerfile` (`git apply --check` first). CI fails if a patch does not apply or MTL does not build with them.

| Patch | Purpose | New API | Spec |
|---|---|---|---|
| `0001-ptp-status-api.patch` | Status of every PTP instance: lock state, parent and grandmaster data, offset and path-delay statistics, cumulative counters, GM change counter. Every Announce of the parent is parsed, so a grandmaster change behind it is seen. Snapshot published under a writer spinlock + seqlock. | `struct mtl_ptp_status`, `mtl_ptp_get_status()`, `MTL_HAS_PTP_STATUS` | §5.5 |
| `0002-ptp-domain-filter.patch` | Ignore PTP messages of other domains. | `MTL_FLAG_PTP_DOMAIN_FILTER`, `mtl_init_params.ptp_domain`, `MTL_HAS_PTP_DOMAIN_FILTER` | §5.5 |
| `0003-ptp-dual-port-bmca.patch` | Best-master selection per instance and parent re-selection after 3 missed Announce intervals; PTP on both ports of a 2022-7 pair with a BMCA across them, only the selected instance steers the shared PHC and answers `mtl_ptp_read_time`. | `MTL_FLAG_PTP_BMCA`, `MTL_FLAG_PTP_DUAL_PORT`, `MTL_HAS_PTP_DUAL_PORT` | §4.3, §5.5 |

All behaviour changes are opt-in through new flags; without them MTL behaves as `v26.09`. The gateway detects the patches through the `MTL_HAS_*` macros and falls back to "PTP status unavailable" without them.

Regenerating: apply the patches to a `v26.09` checkout, edit, and export each one with `git diff` against the previous commit. Upstream rationale: `docs/upstream/mtl-ptp-status.md`.
