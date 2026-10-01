# Performance

Capacity targets of SPECIFICATION.md §18, measured on the acceptance hosts (`docs/acceptance.md`). Each run: 24 h, per direction concurrently, zero packet loss (`mxlgw_rx_leg_seq_lost_total` and `mxlgw_nic_rx_missed_total` flat) and zero late frames (`mxlgw_tx_late_frames_total`, `mxlgw_mxl_read_timeouts_total` flat).

## Targets

| Host | Ingest | Egress | Audio / ANC per group |
|---|---|---|---|
| E810 2×25G | 8 × 1080p50 | 8 × 1080p50 | 16 ch L24 1 ms + ANC |
| E810 2×100G | 4 × 2160p50 | 4 × 2160p50 | 16 ch L24 1 ms + ANC |

## Results

| Date | Version | Host | Load | Duration | Packet loss | Late frames | MTL lcores | Conversion cores (`app_cpus` load) | Max `mxlgw_ingest_origin_age_ns` | Min `mxlgw_egress_lead_ns` | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| | | | | | | | | | | | |

## How to measure

1. Configure the groups through the UI (counts dialog, 1080p50 default profile), redundancy on, `nic.lcores` and `nic.app_cpus` on isolated cores of the NIC's NUMA node.
2. Feed the ingest receivers from a generator or analyser; connect egress receivers to the ingest MXL Senders (or `tools/mxl-pattern-writer` flows).
3. Scrape `/metrics` every 10 s; the Grafana dashboard rows *NIC*, *Ingest* and *Egress* show every counter above.
4. Record CPU per thread with `pidstat -t -p $(pidof mxl-st2110-gateway) 10` (lcores, `egress-*`, `ingest-*` workers).

The kernel-socket backend used in CI is not representative (no pacing, kernel copies); never use it for these numbers.
