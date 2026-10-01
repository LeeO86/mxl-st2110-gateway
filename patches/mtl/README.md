# MTL patches

Applied in order to MTL `v26.09` by `docker/Dockerfile` (`git apply --check` first). CI fails if a patch does not apply.

| Patch | Purpose | Spec |
|---|---|---|
| `0001-ptp-status-api.patch` | `mtl_ptp_get_status()`: lock state, parent/grandmaster data, offset/path-delay statistics, GM change counter; every Announce parsed | §5.5 |
| `0002-ptp-domain-filter.patch` | `mtl_init_params.ptp_domain`: ignore PTP messages of other domains | §5.5 |
| `0003-ptp-dual-port-bmca.patch` | PTP on both ports of a 2022-7 pair, BMCA across both, parent re-selection after Announce timeout | §5.5 |

Upstream rationale: `docs/upstream/mtl-ptp-status.md`.
