# Upstream proposal: PTP status, domain filter and dual-port BMCA for MTL

Prepared from `patches/mtl/0001`–`0003` (against `v26.09`) for three separate pull requests (0004, the scheduler lost-wakeup fix, is a fourth, independent one) to `OpenVisualCloud/Media-Transport-Library`.

## Motivation

Broadcast facilities run several PTP domains and redundant ST 2022-7 networks. An application built on MTL's built-in PTP needs to know whether the clock is locked, to which grandmaster, and how good the lock is, and it must keep its lock when the grandmaster fails over or one network leg is lost. In `v26.09`:

- The public API exposes only `mtl_ptp_read_time[_raw]` and the `ptp_sync_notify` callback; lock state, parent and grandmaster data live only in the internal `struct mt_ptp_impl`.
- `ptp_parse_announce` handles only the first Announce (`if (!ptp->master_initialized)`); Sync/Follow_Up/Delay_Resp from any other port identity are dropped in `mt_ptp_parse`, so the stack never re-locks to a new parent, and a grandmaster change behind the parent is invisible.
- `domain_number` is recorded but not filtered.
- PTP runs on `MTL_PORT_P` only (other ports only with RX timestamp offload) and `mtl_ptp_read_time` always reads `MTL_PORT_P`, so losing the primary leg loses PTP.

## PR 1 — `mtl_ptp_get_status()`

- `struct mtl_ptp_status` and `int mtl_ptp_get_status(mtl_handle, enum mtl_port, struct mtl_ptp_status*)` in `include/mtl_api.h`, guarded by `MTL_HAS_PTP_STATUS`.
- Every Announce of the current parent updates the grandmaster fields; a changed grandmaster identity increments `gm_change_count` and is logged.
- `ptp_stat_clear` keeps the closing statistics window (min/max/avg of offset and path delay) and accumulates cumulative counters before it resets the period counters.
- The snapshot is published by the PTP threads (RX tasklet / CNI, alarm thread, stat thread) under a writer spinlock and read lock-free through a sequence counter, so readers never block PTP processing.

## PR 2 — domain filter

- `MTL_FLAG_PTP_DOMAIN_FILTER` plus `uint8_t mtl_init_params.ptp_domain`. With the flag, `mt_ptp_parse` ignores messages of other domains and counts them in the periodic stat output.
- A flag instead of a sentinel value keeps zero-initialised parameter structs of existing applications behaving as before (they would otherwise start filtering domain 0).

## PR 3 — BMCA and dual-port PTP

- `MTL_FLAG_PTP_BMCA`: each instance keeps the best Announce data set of the domain (IEEE 1588-2008 §9.3.4 order: priority1, clockClass, clockAccuracy, offsetScaledLogVariance, priority2, grandmasterIdentity, stepsRemoved, sender port identity) and switches parent when a better one appears. A 1 s alarm drops a parent that sent no Announce for 3 announce intervals (`logMessageInterval` of its Announce), so the next Announce re-selects. A parent change restarts the offset measurement.
- `MTL_FLAG_PTP_DUAL_PORT` (implies BMCA): instances on `MTL_PORT_P` and `MTL_PORT_R`. The selection across both ports is kept in the primary instance; ties keep the current selection. Only the selected instance steers the PHC (`ptp_adjust_delta`), the other one measures and reports its own offset. `mtl_ptp_read_time` reads through the selected port; both ports' `ptp_get_time_fn` point to the shared PHC once either has a parent. Selection changes are counted (`mtl_ptp_status.selection_changes`) and logged.
- The comparison and timeout logic is mirrored by the gateway in `src/timing/ptp.cpp` and unit-tested there with the same vectors (`tests/unit/test_ptp.cpp`).

## Testing

- CI: `git apply --check` of all three patches on `v26.09` and a full MTL build with `-Werror` (Docker `deps` stage).
- Hardware (recorded in `docs/acceptance.md`): lock on an E810 PF, grandmaster identity reported, grandmaster failover with and without parent change, pulling either 2022-7 leg (`selected` moves when the selected leg is pulled, PTP stays locked).
