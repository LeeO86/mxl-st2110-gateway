# Metrics

`GET /metrics` on the web port (`node.web_port`, default `node.http_port`) serves the Prometheus text exposition format 0.0.4 (SPECIFICATION.md §12.1). Every name starts with `mxl_st2110_gateway_` (until v1.0.0: `mxlgw_`). Metric names and labels are a **public interface**: they stay stable within a major version; any change is listed in `CHANGELOG.md`. The Grafana dashboard `monitoring/grafana/mxl-st2110-gateway.json` is generated from the same names (`monitoring/tools/gen_dashboard.py`, which refuses unknown names).

## Labels

| Label | Values |
|---|---|
| `group`, `essence` | group / essence labels from the configuration |
| `uid` | essence `uid` (stable across renames) |
| `type` | `video`, `audio`, `anc` |
| `direction` | `ingest` (ST 2110 → MXL), `egress` (MXL → ST 2110) |
| `port` | `p` (primary) or `r` (redundant) media port |
| `leg` | `p` or `r` ST 2022-7 leg of an essence |
| `stat` | `min` / `max` over the last 60 s; the series without `stat` is the last value |
| `node` | `mxl` (MXL node, MXL registry) or `st2110` (ST 2110 node) |

"Essence labels" below means `group, essence, uid, type, direction`.

## Process and build

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_build_info` | gauge = 1 | `version, mtl, dpdk, mxl, nmos_cpp` | gateway version (git tag or `0.0.0-dev+<sha>`) and dependency pins |
| `mxl_st2110_gateway_ready` | gauge | — | 1 when `/readyz` is 200 |
| `mxl_st2110_gateway_restart_required` | gauge | — | 1 when persisted changes (node, nic, ptp, mxl, import) need a restart |

## PTP and clock

All `mxl_st2110_gateway_ptp_*` series exist only while MTL runs PTP (`ptp.mode` `builtin` or `builtin_phc2sys` on the `dpdk` backend). With `ptp.mode = external` or the kernel backend they are absent.

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_ptp_locked` | gauge | `port` | 1 when that port's PTP instance is locked (both ports run PTP with redundancy, §4.3) |
| `mxl_st2110_gateway_ptp_selected` | gauge | `port` | 1 for the port whose PTP instance steers the PHC (dual-port BMCA, §5.5) |
| `mxl_st2110_gateway_ptp_selection_changes_total` | counter | — | BMCA selection changes between the ports |
| `mxl_st2110_gateway_ptp_info` | gauge = 1 | `port, gm_identity, parent_port_identity, domain, bind_mode` | grandmaster and parent seen on the port; replaced when they change |
| `mxl_st2110_gateway_ptp_offset_ns` | gauge | `port` [, `stat`] | offset from master |
| `mxl_st2110_gateway_ptp_path_delay_ns` | gauge | `port` [, `stat`] | mean path delay |
| `mxl_st2110_gateway_ptp_utc_offset_seconds` | gauge | `port` | UTC offset announced by the grandmaster (display only; never used in the media path) |
| `mxl_st2110_gateway_ptp_gm_changes_total` | counter | `port` | grandmaster changes seen in Announce messages |
| `mxl_st2110_gateway_ptp_sync_total` | counter | `port` | Sync messages processed |
| `mxl_st2110_gateway_ptp_errors_total` | counter | `port, kind` | `kind` = `rx_sync`, `tx_sync`, `result`, `timeout` |
| `mxl_st2110_gateway_clock_mtl_minus_host_tai_ns` | gauge | [`stat`] | MTL PTP time minus host `CLOCK_TAI` (§5.3); `/readyz` fails beyond `ptp.max_offset_ns` |

## NIC

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_nic_link_up` | gauge | `port` | link state (DPDK `rte_eth_link_get_nowait`; kernel backend: sysfs) |
| `mxl_st2110_gateway_nic_link_speed_mbps` | gauge | `port` | link speed |
| `mxl_st2110_gateway_nic_info` | gauge = 1 | `port, mac, pci, driver, ddp_package` | `ddp_package` is parsed from the ice PMD's "Active package is" log line |
| `mxl_st2110_gateway_nic_rx_packets_total`, `mxl_st2110_gateway_nic_tx_packets_total` | counter | `port` | packets (MTL `mtl_get_port_stats`) |
| `mxl_st2110_gateway_nic_rx_bytes_total`, `mxl_st2110_gateway_nic_tx_bytes_total` | counter | `port` | bytes |
| `mxl_st2110_gateway_nic_rx_errors_total` | counter | `port` | erroneous packets |
| `mxl_st2110_gateway_nic_rx_missed_total` | counter | `port` | packets dropped by the NIC (no RX descriptor / mbuf) |

## Essences

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_essence_state` | gauge | essence labels, `state` | 1 for the current state, 0 for the others; `state` ∈ `idle`, `waiting_for_flow`, `no_signal`, `running`, `degraded`, `error` |

### Ingest (ST 2110 → MXL)

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_rx_frames_total` | counter | essence labels, `result` | `complete`, `incomplete` (written with missing packets), `dropped` (not written: discarded by the gateway, or by MTL because the ingest worker held every frame buffer; MTL counts audio drops per packet and leg, the gateway reports them as blocks, rounded up) |
| `mxl_st2110_gateway_rx_leg_packets_total` | counter | essence labels, `leg` | packets per ST 2022-7 leg |
| `mxl_st2110_gateway_rx_packets_total` | counter | essence labels | packets after the 2022-7 merge |
| `mxl_st2110_gateway_rx_leg_seq_lost_total` | counter | essence labels, `leg` | sequence gaps on a leg (loss on one leg is harmless while the other is clean) |
| `mxl_st2110_gateway_ingest_origin_age_ns` | gauge | essence labels | TAI now minus the origin time of the last frame written |
| `mxl_st2110_gateway_mxl_grains_written_total` | counter | essence labels | grains committed (video, ANC) |
| `mxl_st2110_gateway_mxl_samples_written_total` | counter | essence labels | audio samples committed |
| `mxl_st2110_gateway_mxl_write_errors_total` | counter | essence labels | writes MXL rejected (e.g. index not after the last committed one, §5.6) |

### Egress (MXL → ST 2110)

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_mxl_grains_read_total` | counter | essence labels | grains (video, ANC) or audio blocks read |
| `mxl_st2110_gateway_mxl_read_timeouts_total` | counter | essence labels | grains/blocks with no data by their deadline (replacement data sent, §5.7) |
| `mxl_st2110_gateway_mxl_late_reads_total` | counter | essence labels | data that became available only after its deadline and was skipped |
| `mxl_st2110_gateway_mxl_grains_invalid_total` | counter | essence labels | grains read with `MXL_GRAIN_FLAG_INVALID` |
| `mxl_st2110_gateway_mxl_flow_not_found_total` | counter | essence labels | reader attempts that found no flow or no domain (§5.8 backoff 500 ms → 5 s) |
| `mxl_st2110_gateway_mxl_read_lag_grains` | gauge | essence labels | writer head index minus read index; audio in grains of the group cadence |
| `mxl_st2110_gateway_mxl_reader_info` | gauge = 1 | essence labels, `domain_id, domain_path, domain_kind, flow_id` | resolved domain of an enabled MXL Receiver; `domain_kind` ∈ `configured`, `discovered`, `mirror`; replaced when the resolution changes |
| `mxl_st2110_gateway_tx_frames_total` | counter | essence labels | frames/blocks handed to MTL |
| `mxl_st2110_gateway_tx_late_frames_total` | counter | essence labels | frames that missed their transmit time |
| `mxl_st2110_gateway_egress_lead_ns` | gauge | essence labels | time from data available to the TX deadline; negative = late |

## NMOS and MXL domains

| Metric | Type | Labels | Meaning |
|---|---|---|---|
| `mxl_st2110_gateway_nmos_registered` | gauge | `node` | 1 while the NMOS node is registered with its registry (one series per running node) |
| `mxl_st2110_gateway_nmos_activations_total` | counter | `kind, transport, result` | IS-05 activations; `kind` ∈ `sender`, `receiver`; `transport` = the IS-05 transport URN; `result` ∈ `ok`, `unknown_resource` (staging errors are rejected by IS-05 before activation and not counted) |
| `mxl_st2110_gateway_mxl_domain_bytes` | gauge | `domain, kind` | configured domain filesystem usage, `kind` ∈ `used`, `free` |
| `mxl_st2110_gateway_mxl_domain_flows` | gauge | `domain` | flows in a configured domain |
| `mxl_st2110_gateway_mxl_discovered_domains` | gauge | `kind` | domains under `mxl.scan_path`: `discovered`, `mirror`, `conflict` (duplicate ids, excluded from resolution) |

## Linting

`tests/integration/check-metrics.sh <url>` runs `promtool check metrics`. promtool's "abbreviated units" lint flags the `_ns` names above; they are kept as specified (open question O-3 in `docs/decisions.md`), every other finding fails CI.
