# mxl-st2110-gateway

A containerised gateway between **SMPTE ST 2110** networks and **[MXL](https://github.com/dmf-mxl/mxl)** shared-memory media exchange, controlled through **AMWA NMOS IS-04 / IS-05** and **BCP-007-03 (NMOS With MXL)**.

- **Ingest** (ST 2110 → MXL): ST 2110-20 video becomes real `video/v210` grains, ST 2110-30 audio `audio/float32`, ST 2110-40 ANC `video/smpte291` — with ST 2022-7 seamless protection on the input.
- **Egress** (MXL → ST 2110): MXL flows are sent as paced ST 2110 streams on both 2022-7 legs, lip-synced per group.
- Media I/O on an **Intel E810** through the [Media Transport Library](https://github.com/OpenVisualCloud/Media-Transport-Library) (DPDK); PTP runs inside MTL on both media ports with a BMCA across them.
- NMOS node via [Sony nmos-cpp](https://github.com/sony/nmos-cpp); admin web UI, REST API, Prometheus metrics and health checks on **one HTTP port**.
- Works with [mxl-decklink](https://github.com/LeeO86/mxl-decklink) on the same host and with [mxl-fabrics-agent](https://github.com/LeeO86/mxl-fabrics-agent) for host-to-host replication.

The behaviour is specified in [`SPECIFICATION.md`](SPECIFICATION.md); design decisions and deviations are in [`docs/decisions.md`](docs/decisions.md).

## Contents

- [Concepts](#concepts)
- [Quick start (Docker Compose)](#quick-start-docker-compose)
- [Host preparation](#host-preparation)
- [Kubernetes](#kubernetes)
- [Configuration](#configuration)
- [Admin UI and REST API](#admin-ui-and-rest-api)
- [PTP modes](#ptp-modes)
- [Port usage and co-location](#port-usage-and-co-location)
- [Multi-host operation with mxl-fabrics-agent](#multi-host-operation-with-mxl-fabrics-agent)
- [Metrics and Grafana](#metrics-and-grafana)
- [Limitations](#limitations)
- [Troubleshooting (preflight)](#troubleshooting-preflight)
- [Development](#development)
- [Conformance](#conformance)
- [License](#license)

## Concepts

- **Group** — the user-facing unit, e.g. "CAM 1": a direction (`ingest` or `egress`), an MXL domain, a redundancy flag and *N* video + *M* audio + *K* ANC **essences**. One group = one BCP-002-01 group hint.
- **Ingest essence** = NMOS Receiver (`rtp.mcast`) + NMOS Sender (`mxl`): an MTL RX session writes an MXL flow.
- **Egress essence** = NMOS Receiver (`mxl`) + NMOS Sender (`rtp.mcast`): an MXL reader feeds an MTL TX session.
- **Configured domain** — an MXL domain in `mxl.domains[]`, the only kind the gateway writes to. **Discovered** and **mirror** domains (created by mxl-fabrics-agent) found under `mxl.scan_path` can be read by egress receivers.
- All media timing is **TAI since the ST 2059-1 epoch**: an ingest grain's index comes from its RTP timestamp; egress RTP timestamps are the transmit time `grain time + output_delay` (default two grains).

## Quick start (Docker Compose)

Prepare the host first ([Host preparation](#host-preparation)). Then:

```bash
mkdir -p mxl-st2110-gateway/config && cd mxl-st2110-gateway
curl -fsSLO https://raw.githubusercontent.com/LeeO86/mxl-st2110-gateway/main/docker/docker-compose.yaml
docker compose up -d
```

[`docker/docker-compose.yaml`](docker/docker-compose.yaml):

```yaml
services:
  mxl-st2110-gateway:
    image: ghcr.io/leeo86/mxl-st2110-gateway:1   # or :nightly-dev
    container_name: mxl-st2110-gateway
    restart: unless-stopped
    init: true
    network_mode: host                # management NIC; DNS-SD; media ports are DPDK
    stop_grace_period: 15s
    ulimits:
      memlock: { soft: -1, hard: -1 }
    cap_add: [IPC_LOCK, SYS_NICE]     # + SYS_TIME only for ptp.mode=builtin_phc2sys
    devices:
      - /dev/vfio:/dev/vfio
    volumes:
      - ./config:/config                       # gateway.json + state/
      - /dev/hugepages:/dev/hugepages
      - type: bind                             # host MXL root (tmpfs), shared with other media functions
        source: /Volumes/mxl
        target: /Volumes/mxl
    environment:
      MXLGW_CONFIG: /config/gateway.json
      # MXLGW_HTTP_PORT: "8090"       # e.g. when mxl-decklink already uses 8080 on this host
    healthcheck:
      test: ["CMD", "curl", "-fsS", "http://127.0.0.1:8080/livez"]
      interval: 10s
      timeout: 3s
      start_period: 60s
```

Without a configuration file the gateway starts in **setup mode**: only the admin UI runs at `http://<host>:8080/admin/`, `/readyz` reports `unconfigured`. Configure the NIC (PCI addresses, IPs) on the *Network* tab, restart, then create groups on the *Groups* tab. Image tags: `1.2.3`, `1.2`, `1`, `latest` for releases; `nightly-dev` for the newest `main` build; `git-<sha>` for every build.

**Why host networking:** NMOS registry discovery uses DNS-SD (multicast) and the node advertises its management address; the media ports are owned by DPDK and are not visible to Docker networking anyway. With a bridge network, map the port and set `node.public_address` / `public_port` and a static registry ([Port usage](#port-usage-and-co-location)).

<a id="config-volume"></a>**The `/config` volume** must be writable: the gateway writes `gateway.json` (UI saves, generated ids, a `.bak` of the previous version) and `state/connections.json` (§7.6). Without it the container exits with code 78.

**MXL root.** `/Volumes/mxl` is a **tmpfs on the host** shared by every MXL container of the host: `/etc/fstab` entry `tmpfs /Volumes/mxl tmpfs size=8g,mode=1777 0 0` (the CBC `mxl-hands-on` convention, also used by mxl-decklink and mxl-fabrics-agent). The gateway creates its configured domain (e.g. `/Volumes/mxl/main`) with `domain_def.json` and `options.json` and never overwrites existing ones. Other media functions mount the same root (read-only is enough for readers) — each container may use its own container path; a domain's identity is its `domain_def.json` id, not its path. When the domain is only shared inside one Compose project, a Compose `tmpfs:` volume mounted by all services also works, but then no other container or mxl-fabrics-agent can see it.

**Non-root mode.** The process runs as root in the container because VFIO group nodes are root-owned on most hosts. To run as a user: a udev rule giving a group access to `/dev/vfio/*` (e.g. `SUBSYSTEM=="vfio", GROUP="vfio", MODE="0660"`), `user: "<uid>:<vfio-gid>"` in Compose, the MXL root writable for that group, and `IPC_LOCK` kept. Files the gateway creates in domains use modes `0664` / `0775`.

## Host preparation

Applies to Compose and Kubernetes nodes. Ubuntu 24.04 and Debian 13 hosts work (the container userland is independent; the kernel needs VFIO).

1. **BIOS:** VT-d on, SR-IOV on (only for VFs), C-states limited for latency.
2. **Kernel command line:** `intel_iommu=on iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=<n>` (or 2 MB pages). 4 GiB of hugepages covers several 1080p50 groups; the preflight estimates the need per session.
3. **Bind the media PFs to `vfio-pci` persistently**, e.g. `driverctl set-override 0000:31:00.0 vfio-pci` and `… 0000:31:00.1 vfio-pci`. Keep E810 NVM/firmware as recommended by MTL (`doc/e800_series_drivers.md`). For VF mode use MTL's patched host `ice` driver.
4. **CPU isolation (recommended):** `isolcpus` / `nohz_full` / `rcu_nocbs` for the MTL lcores (`nic.lcores`), exclude them from irqbalance, keep `nic.app_cpus` on the NIC's NUMA node.
5. **Time:** discipline `CLOCK_TAI` to the facility grandmaster **with the correct TAI offset** (37 s): `ptp4l -f …` + `phc2sys -a -r` on a kernel-owned port, or chrony with a PHC/PTP reference and `leapsectz right/UTC`. This is required on **every** host that writes, replicates or reads MXL flows — including hosts that only run receivers — because grain indices are TAI-based and replicated 1:1 by mxl-fabrics-agent. See [PTP modes](#ptp-modes).
6. **MXL root:** one host tmpfs for all MXL domains of the host (`/Volumes/mxl`, see above), mounted by every MXL container of the host at its configured root path (gateway: `mxl.scan_path`, mxl-fabrics-agent: `MXL_ROOT`).
7. **DNS-SD:** an Avahi daemon on the host (or a static registry).

The E810 **DDP package** is part of the image (`/lib/firmware/updates/intel/ice/ddp/ice.pkg`); a host `/lib/firmware` mount may override it.

## Kubernetes

Manifests in [`deploy/k8s`](deploy/k8s) ([README](deploy/k8s/README.md)): SR-IOV Network Device Plugin with one resource per media port (`intel.com/e810_media_p`, `intel.com/e810_media_r`, injected as `PCIDEVICE_INTEL_COM_E810_MEDIA_P` and referenced as `"pci": "env:PCIDEVICE_INTEL_COM_E810_MEDIA_P"`), a `Deployment` with `replicas: 1`, `strategy: Recreate`, host networking, Guaranteed QoS, `hugepages-1Gi`, `IPC_LOCK` + `SYS_NICE` (unprivileged), startup/liveness/readiness probes, the configuration on a PVC seeded once from a ConfigMap, and the node's MXL root as `hostPath`.

```bash
kubectl apply -k deploy/k8s
```

The two-node mxl-fabrics-agent scenario is in [`deploy/k8s/fabrics`](deploy/k8s/fabrics/README.md).

## Configuration

`/config/gateway.json` (path: `MXLGW_CONFIG`), validated against [`schema/gateway-config.schema.json`](schema/gateway-config.schema.json) and the semantic rules of §9.5. **Every setting is documented in [`docs/configuration.md`](docs/configuration.md)** (generated from the schema). Examples: [`config/examples`](config/examples).

- **Precedence:** environment variable > file > default for every scalar of `node`, `nic`, `ptp`, `mxl` (`MXLGW_NODE_LABEL`, `MXLGW_HTTP_PORT`, `MXLGW_NIC_PRIMARY_PCI`, `MXLGW_PTP_MODE`, `MXLGW_MXL_SCAN_PATH`, …). Environment-set keys are read-only in the UI and never written into the file.
- **UI changes:** groups and essences apply live (only the edited group is rebuilt); `node`, `nic`, `ptp` and `mxl` changes are saved and flagged *restart required*.
- **Hand edits** are allowed and take effect after a restart (container restart or `POST /api/restart`). While the file differs from what the gateway wrote, the UI shows "configuration changed on disk" and blocks saves until you restart or overwrite.
- **Import / export:** `GET /api/config/export` downloads the file; `POST /api/config/import?keep_ids=true|false` validates and writes it (never applied live). `keep_ids=false` regenerates all ids for cloning a gateway.
- An invalid file stops the gateway with **exit code 78** and every error printed (JSON pointer + message).

## Admin UI and REST API

`http://<host>:<http_port>/admin/` — a single embedded page, works offline, follows the browser's light/dark theme.

| Tab | Content |
|---|---|
| Dashboard | versions, readiness and reasons, PTP lock and grandmaster, MTL − host TAI offset, links, per-group tiles with essence state, bitrate, counters, 2022-7 leg health, resolved MXL domain per egress essence (mirror marked) |
| Groups | create by counts (video × n, audio × m, ANC × k), edit essences (format, payload type, legs, read offsets), duplicate, delete |
| NMOS | node/device ids, registration, all Senders/Receivers with active parameters and SDPs |
| Network | port pair form (PCI or interface, IP, netmask, gateway), bind mode, MAC, link, DDP package, counters |
| PTP | mode, domain, lock per port, BMCA selection, grandmaster data, offset and path delay sparklines |
| MXL | configured, discovered and mirror domains, flow browser, MXL Receiver resolution and read lag |
| Configuration | export / import with validation report, raw file, changed-on-disk resolution, restart, preflight, logs |

Every field shows its provenance (default / file / environment); server-side validation errors appear on the field. REST endpoints (`/api/status`, `/api/config` with `ETag` / `If-Match`, `/api/groups`, `/api/config/export|import|validate`, `/api/schema`, `/api/nic`, `/api/ptp`, `/api/domains`, `/api/flows?domain=<id>`, `/api/nmos`, `/api/preflight`, `/api/logs`, `/api/restart`) are described in SPECIFICATION.md §11.3. Mutating requests need `Content-Type: application/json` and are rejected cross-origin. There is no authentication: restrict the management network.

Health: `/livez` (process alive), `/readyz` (200 only when the config is valid, MTL is up, PTP is locked unless `require_lock=false`, the clock offset is within `ptp.max_offset_ns`, all configured domains are usable and the node is registered; essences waiting for flows do not count), `/statusz` (text).

## PTP modes

| `ptp.mode` | What happens | Use when |
|---|---|---|
| `builtin` (default) | MTL runs PTP on **both** media ports (patched MTL: domain filter, BMCA across ports, per-Announce grandmaster tracking). The host clock is **not touched**: discipline `CLOCK_TAI` separately (step 5 of host preparation). The gateway measures MTL time − host `CLOCK_TAI` every second (`mxlgw_clock_mtl_minus_host_tai_ns`); beyond `ptp.max_offset_ns` it is not ready. | normal and shared hosts, Kubernetes |
| `builtin_phc2sys` | Additionally MTL steers `CLOCK_REALTIME` to the PHC **without subtracting the UTC offset**: the wall clock shows TAI (37 s ahead of UTC). The kernel TAI offset must be 0 (checked at start), NTP/chrony must be off, `CAP_SYS_TIME` is needed. | dedicated single-purpose appliances only |
| `external` | No MTL PTP; the host `CLOCK_TAI` is the only time source. PTP metrics are absent. | VF deployments, hosts already PTP-locked through the kernel; always with the test-only kernel backend |

The UTC offset is only displayed; RTP, MTL and MXL all work on TAI.

## Port usage and co-location

Under host networking all containers of a host share one port space.

| Container | Default port(s) | Protocol | Purpose |
|---|---|---|---|
| mxl-st2110-gateway | `8080` (`node.http_port`, override `MXLGW_HTTP_PORT`) | TCP (HTTP/HTTPS) | IS-04 Node API, IS-05 Connection API, admin UI, `/api`, `/metrics`, health — the only host port the gateway opens |
| mxl-st2110-gateway | none | — | IS-07 Events WebSocket (`http_port + 1`), nmos-cpp Settings/Logging APIs and other optional APIs are disabled |
| mxl-st2110-gateway | none on the host stack | — | ST 2110 media and PTP run on DPDK-owned ports; only the test-only kernel backend uses UDP ports of the configured legs on its test interfaces |
| mxl-st2110-gateway | `5353/udp` via the host's Avahi | mDNS | DNS-SD (registry discovery), shared host daemon |
| mxl-decklink | `8080` | TCP | web UI, REST, health, metrics |
| mxl-decklink | `3212`, `3213` | TCP | NMOS Node/Connection API, IS-07 WebSocket |
| mxl-fabrics-agent | `8095` | TCP | UI, REST/control API, health, metrics |
| mxl-fabrics-agent | `3232`, `3233` | TCP | NMOS Node API, WebSocket |
| mxl-fabrics-agent | `23500`–`23599` | TCP / RDMA CM | fabric data ports (target pool) |

**With mxl-decklink on the same host** (both default to 8080), either (a) set `MXLGW_HTTP_PORT=8090` (free in the table) under host networking, (b) use a bridge network with a port mapping and set `node.public_port` / `node.public_address` so NMOS hrefs advertise the mapped port, or (c) put a reverse proxy in front and set `node.public_address` / `public_port` to the proxy. DNS-SD discovery needs host networking or `node.registry.mode = "static"`. The preflight warns when the port is in use or equals a sibling default.

## Multi-host operation with mxl-fabrics-agent

[mxl-fabrics-agent](https://github.com/LeeO86/mxl-fabrics-agent) runs one container per host and replicates the MXL flows that enabled MXL Receivers need from the host that writes them, using the MXL Fabrics API (RDMA). The gateway has no Fabrics code and needs no configuration for it:

- **Host A, gateway as MXL Sender (ingest):** flows are written into a configured domain directly under the host's MXL root, with a stable domain id (written back into the config) and stable flow ids.
- **Host B, gateway as MXL Receiver (egress):** a controller stages host A's `mxl_domain_id` and `mxl_flow_id`. The agent on host B creates `mirror-<domain-id>` next to the local domains; the receiver finds it by id, waits (`waiting_for_flow`, then `no_signal`) until grains arrive and starts without further action. Read a few grains behind the writer on host B (`read_offset_grains` or `mxl.default_read_offset_grains`; the example uses 2) and watch `mxlgw_mxl_read_lag_grains`.

| mxl-fabrics-agent §11 requirement | Gateway |
|---|---|
| Receivers resolve `mxl_domain_id` by scanning the MXL root; mirror domains are siblings of local domains | `mxl.scan_path`, domain discovery with re-scan before each lookup, no negative caching |
| Retry with backoff when a flow is not (yet) present | 500 ms → 5 s backoff while `master_enable` (also for unknown domains, accepted at activation) |
| Tolerate a flow that exists but has no new grains | `no_signal` with replacement data; not an error, readiness unaffected |
| No `MXL_ENABLE_FABRICS_OFI` | the image is built with Fabrics OFF |
| Read with a small latency offset on destination hosts | per-receiver `read_offset_grains` / `read_offset_ns` |

Further rules: the gateway never writes into mirror domains; new flow UUIDs (format changes) are published in NMOS before the new writer starts; **all hosts TAI-disciplined**; non-colliding ports.

**Demo.** Compose: [`docker/docker-compose.fabrics.yaml`](docker/docker-compose.fabrics.yaml) with profiles `host-a` and `host-b` and the configurations [`gateway.fabrics-host-a.json`](config/examples/gateway.fabrics-host-a.json) / [`gateway.fabrics-host-b.json`](config/examples/gateway.fabrics-host-b.json); Kubernetes: [`deploy/k8s/fabrics`](deploy/k8s/fabrics/README.md).

1. Both hosts: one NMOS registry, TAI-disciplined clocks, the MXL root tmpfs, RDMA links for the agents.
2. Host A: `docker compose -f docker-compose.fabrics.yaml --profile host-a up -d`; connect a 2110 source to the "CAM 1" receivers and enable its MXL Senders.
3. Host B: `docker compose -f docker-compose.fabrics.yaml --profile host-b up -d`.
4. PATCH host B's MXL Receiver (or use a controller):

   ```bash
   curl -X PATCH -H 'Content-Type: application/json' \
     http://host-b:8090/x-nmos/connection/v1.2/single/receivers/<receiver-id>/staged -d '{
       "master_enable": true, "activation": {"mode": "activate_immediate"},
       "transport_params": [{"mxl_domain_id": "a1a1a1a1-0000-4000-8000-00000000a001",
                             "mxl_flow_id": "<flow id of host A MXL Sender>"}]}'
   ```

5. Check `mxlgw_mxl_reader_info{domain_kind="mirror"}` and `mxlgw_mxl_read_lag_grains` on host B (Grafana row *Egress*).

## Metrics and Grafana

`/metrics` (Prometheus text format) on `node.http_port`; every metric is listed in [`docs/metrics.md`](docs/metrics.md). Metric names and labels are a stable public interface. Dashboard: [`monitoring/grafana/mxl-st2110-gateway.json`](monitoring/grafana/mxl-st2110-gateway.json) (Grafana ≥ 11; variables `datasource`, `instance`, `group`, `essence`; rows Overview, PTP, NIC, Ingest, Egress, NMOS, MXL domains). Scrape example: [`monitoring/prometheus/scrape-example.yaml`](monitoring/prometheus/scrape-example.yaml); Prometheus Operator: [`deploy/k8s/servicemonitor.yaml`](deploy/k8s/servicemonitor.yaml).

Logs are JSON lines on stdout (`ts`, `level`, `event`, fields; `MXLGW_LOG_FORMAT=text` for text) with stable event names (`essence_state`, `nmos_activation`, `mxl_flow_not_found`, `mxl_domain_discovered`, `ptp_gm_changed`, …). MTL and DPDK output is included with `component=mtl` / `dpdk`. The UI shows the last 500 lines.

## Limitations

- One NIC per container with one 2022-7 port pair; the NIC itself is not a redundant element.
- Fixed formats per essence: 1920×1080 or 3840×2160 (progressive; 1080i at 25/1 and 30000/1001), 10-bit 4:2:2; audio L16/L24 48 kHz with 1 ms or 125 µs packets; ANC per RFC 8331 with MTL's limits (20 packets, 8-bit UDW per frame). An SDP with another format is rejected (400); there is no "follow the SDP".
- No ST 2110-22, no 720p, no sample-rate conversion or drift compensation: sources must be locked to the same grandmaster (drift is reported as `degraded`).
- No authentication on the UI/API (management network only); TLS is optional.
- The `kernel` and `mock` backends are for tests only (no pacing, no hardware PTP).
- The gateway accepts an `mxl_domain_id` it cannot see yet and waits for it — a deliberate deviation from BCP-007-03 for mxl-fabrics-agent's on-demand mirroring ([`docs/conformance.md`](docs/conformance.md)).

## Troubleshooting (preflight)

`mxl-st2110-gateway --preflight` (also run at start and shown at `/api/preflight` and on the *Configuration* tab) checks the environment. A failure stops the start with exit code 78 (the reason is logged every time); warnings are shown in the UI. Each message links to one of these sections.

<a id="preflight-hugepages"></a>**hugepages** — `/dev/hugepages` is missing or not a hugetlbfs mount. Reserve hugepages on the kernel command line and mount them into the container (`-v /dev/hugepages:/dev/hugepages`, Kubernetes: `emptyDir: {medium: HugePages-1Gi}`).

<a id="preflight-hugepages-free"></a>**hugepages-free** — fewer free hugepages than the configured sessions need (estimate per session). Add pages (`hugepages=<n>` or `sysctl vm.nr_hugepages=<n>`), or reduce groups. A warning only on the kernel backend.

<a id="preflight-vfio"></a>**vfio** — `/dev/vfio/vfio` is not present in the container. Map `/dev/vfio` (`devices: [/dev/vfio:/dev/vfio]`; Kubernetes: the SR-IOV device plugin mounts the group nodes).

<a id="preflight-pci"></a>**pci** — per media port: the PCI address does not exist, is not bound to `vfio-pci` (`driverctl set-override <pci> vfio-pci`), has no IOMMU group (enable VT-d and `intel_iommu=on iommu=pt`), its `/dev/vfio/<group>` node is not mapped, or the device is not an Intel E810/E830 (warning). VF mode works but degrades PTP.

<a id="preflight-ddp"></a>**ddp** — the E810 DDP package is missing from the container (`/lib/firmware/updates/intel/ice/ddp/ice.pkg`). It ships with the image; check custom images or a host `/lib/firmware` mount that hides it. Without it the ice PMD refuses to start.

<a id="preflight-ifname"></a>**ifname** — kernel backend: the configured interface (`ifname`) does not exist in the container's network namespace.

<a id="preflight-rmem-max"></a>**rmem-max** — kernel backend: `net.core.rmem_max` is below 4194304; MTL's socket backend loses packets. `sysctl -w net.core.rmem_max=4194304`.

<a id="preflight-test-backend"></a>**test-backend** — `nic.backend` is `kernel` or `mock`: test-only, no pacing guarantees, no hardware PTP. Use `dpdk` in production.

<a id="preflight-cap-ipc-lock"></a>**cap-ipc-lock** — `CAP_IPC_LOCK` is missing (`cap_add: [IPC_LOCK]`); DPDK cannot lock DMA memory. Also set the memlock ulimit to unlimited in Docker.

<a id="preflight-cap-sys-nice"></a>**cap-sys-nice** — `CAP_SYS_NICE` is missing (`cap_add: [SYS_NICE]`): no NUMA memory policy or real-time priorities.

<a id="preflight-cap-sys-time"></a>**cap-sys-time** — `ptp.mode = builtin_phc2sys` needs `CAP_SYS_TIME` (`cap_add: [SYS_TIME]`).

<a id="preflight-domain"></a>**domain** — a configured MXL domain is not on a tmpfs/ramfs (an overlay or disk directory is refused because MXL readers in other containers would not share it), or its path is a `mirror-*` directory (mirror domains belong to mxl-fabrics-agent and are never written by the gateway). Mount the host MXL root tmpfs and point `mxl.domains[].path` below it.

<a id="preflight-scan-path"></a>**scan-path** — information about domains found under `mxl.scan_path` (discovered, mirror, conflicts, skipped). A warning if the path does not exist (no discovery) or two domains share an id (both are excluded from resolution).

<a id="preflight-tai-offset"></a>**tai-offset** — the kernel TAI offset (`adjtimex`). With `builtin` / `external` a value of 0 means `CLOCK_TAI` equals UTC: MXL grain indices are off by the leap seconds and replicated flows are misaligned between hosts — configure `ptp4l`/`phc2sys` or chrony with `leapsectz right/UTC`. With `builtin_phc2sys` the offset must be 0 (start refused otherwise).

<a id="preflight-http-port"></a>**http-port** — `node.http_port` is already in use (fail) or equals a default port of mxl-decklink / mxl-fabrics-agent (warning); see [Port usage](#port-usage-and-co-location).

<a id="preflight-lcores"></a>**lcores** — `nic.lcores` names a CPU that does not exist on this host.

Other frequent issues:

- **Essence `waiting_for_flow`** (egress): the MXL flow or domain does not exist (yet). It starts automatically once it appears; check the flow id, `mxl.scan_path` and, for remote flows, the agent. **`no_signal`**: the flow exists but no grains arrive in time.
- **Essence `error` / `format_mismatch`**: the flow or SDP format differs from the essence configuration (size, rate, scan, channels, packet time).
- **`/readyz` `clock_mismatch`**: MTL PTP time and host `CLOCK_TAI` differ by more than `ptp.max_offset_ns` — fix host time sync.
- **`/readyz` `nmos_not_registered`**: no registry found; check Avahi / DNS-SD or use a static registry.

## Development

```bash
docker build --target build -f docker/Dockerfile -t mxlgw-build .   # compile + unit tests
docker build -f docker/Dockerfile -t mxlgw:dev .                    # runtime image
tests/integration/smoke.sh mxlgw:dev                                # container smoke test
tests/integration/loopback.sh mxlgw:dev                             # kernel-backend media loopback (sudo, hugepages)
tests/integration/late-flow.sh mxlgw:dev                            # receiver before its flow, mirror domain
tests/integration/nmos-testing.sh mxlgw:dev                         # AMWA suites (sudo, avahi)
python3 monitoring/tools/gen_dashboard.py --check                   # dashboard up to date
python3 tools/gen_config_docs.py --check                            # docs/configuration.md up to date
cd web && npm ci && npm test && npm run dev                         # UI against a gateway on :8080
```

Build the slow `deps` stage once (`docker build --target deps -f docker/Dockerfile -t mxlgw-deps .`). A local CMake build needs MXL, MTL/DPDK and nmos-cpp under `CMAKE_PREFIX_PATH`; without them only the pure core and its unit tests are built. Contribution rules: [`AGENTS.md`](AGENTS.md).

## Conformance

AMWA nmos-testing IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 run in CI ([`tests/integration/nmos-testing.sh`](tests/integration/nmos-testing.sh)); results and documented warnings are in [`docs/conformance.md`](docs/conformance.md). Hardware acceptance: [`docs/acceptance.md`](docs/acceptance.md); capacity: [`docs/performance.md`](docs/performance.md).

## License

MIT — see [`LICENSE`](LICENSE). Third-party components and their licenses: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
