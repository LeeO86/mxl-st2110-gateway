# mxl-st2110-gateway

A containerised gateway between **SMPTE ST 2110** networks and **[MXL](https://github.com/dmf-mxl/mxl)** shared-memory media exchange, controlled through **AMWA NMOS IS-04 / IS-05** and **BCP-007-03 (NMOS With MXL)**.

- **Ingest** (ST 2110 → MXL): ST 2110-20 video becomes real `video/v210` grains, ST 2110-30 audio `audio/float32`, ST 2110-40 ANC `video/smpte291` — with ST 2022-7 seamless protection on the input.
- **Egress** (MXL → ST 2110): MXL flows are sent as paced ST 2110 streams on both 2022-7 legs, lip-synced per group.
- Media I/O on an **Intel E810** through the [Media Transport Library](https://github.com/OpenVisualCloud/Media-Transport-Library) (DPDK); PTP runs inside MTL on both media ports with a BMCA across them.
- Two NMOS nodes via [Sony nmos-cpp](https://github.com/sony/nmos-cpp): the **MXL node** (MXL Senders/Receivers, registers with the MXL registry) and the **ST 2110 node** (RTP Senders/Receivers, its own port and registry); admin web UI, REST API, Prometheus metrics and health checks on the MXL node's port.
- Works with [mxl-decklink](https://github.com/LeeO86/mxl-decklink) on the same host and with [mxl-fabrics-agent](https://github.com/LeeO86/mxl-fabrics-agent) for host-to-host replication.

The behaviour is specified in [`SPECIFICATION.md`](SPECIFICATION.md); design decisions and deviations are in [`docs/decisions.md`](docs/decisions.md). Since **v1.0.0** the settings, APIs, metrics and behaviour in this README are a stable contract; a breaking change needs v2.

## Contents

- [Concepts](#concepts)
- [Quick start (Docker Compose)](#quick-start-docker-compose)
- [Running on the MXL PoC platform](#running-on-the-mxl-poc-platform)
- [Host preparation](#host-preparation)
- [Kubernetes](#kubernetes)
- [Configuration](#configuration)
- [Admin UI and REST API](#admin-ui-and-rest-api)
- [Exit codes and shutdown](#exit-codes-and-shutdown)
- [Users and permissions](#users-and-permissions)
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
- **Ingest essence** = NMOS Receiver (`rtp.mcast`, ST 2110 node) + NMOS Sender (`mxl`, MXL node): an MTL RX session writes an MXL flow.
- **Egress essence** = NMOS Receiver (`mxl`, MXL node) + NMOS Sender (`rtp.mcast`, ST 2110 node): an MXL reader feeds an MTL TX session.
- **Two NMOS nodes** in one process: the MXL node (`NMOS_PORT`) registers with the MXL registry (`NMOS_REGISTRY_ADDRESS`), the ST 2110 node (`NMOS_PORT + 1`) with the facility's ST 2110 registry if one is configured (`MXLGW_NODE_ST2110_REGISTRY_ADDRESS`), else it runs peer to peer. ST 2110 resources never appear in the MXL registry.
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
    image: ${MXLGW_IMAGE:-ghcr.io/leeo86/mxl-st2110-gateway:1.0.9}
    container_name: mxl-st2110-gateway
    restart: unless-stopped
    init: true
    network_mode: host                # media ports are DPDK-owned; NMOS announces the host's address
    stop_grace_period: 15s            # > SHUTDOWN_TIMEOUT_S
    user: "0:1000"                    # root for DPDK's VFIO device nodes, group 1000 shared with other media functions
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
      NMOS_SEED: gw-studio1                    # every NMOS id and the output domain id derive from it
      NMOS_LABEL: GW-STUDIO1
      NMOS_REGISTRY_ADDRESS: "10.10.0.5"       # static registry of the MXL node; unset = no registry
      NMOS_REGISTRY_PORT: "3210"
      NMOS_PORT: "8080"                        # MXL node + UI/API/metrics/health; ST 2110 node on 8081
      MXL_DOMAIN_SCAN_PATH: /Volumes/mxl
      MXL_OUTPUT_DOMAIN_DIR: /Volumes/mxl/gw-studio1
      SHUTDOWN_TIMEOUT_S: "10"
    healthcheck:
      test: ["CMD-SHELL", "curl -fsS http://127.0.0.1:$${WEB_PORT:-$${NMOS_PORT:-8080}}/livez"]
      interval: 10s
      timeout: 3s
      start_period: 60s
```

Without a configuration file the gateway starts in **setup mode**: only the admin UI runs at `http://<host>:8080/admin/`, `/readyz` reports `unconfigured`. Configure the NIC (PCI addresses, IPs) on the *Network* tab, restart, then create groups on the *Groups* tab. Image tags: `1.2.3`, `1.2`, `1`, `latest` for releases (never moved, except the floating `1.2`, `1` and `latest`); `nightly-dev` for the newest `main` build; `git-<sha7>` for every commit pushed to `main` (written once, never moved).

**Why host networking:** the media ports are owned by DPDK and are not visible to Docker networking, and both NMOS nodes announce the host's IPv4 address (`NMOS_HOST_ADDRESS`, default the default-route interface's address). With a bridge network, map `NMOS_PORT` and `NMOS_PORT + 1` and set `NMOS_HOST_ADDRESS` (and `node.public_port` behind a port mapping) to what controllers reach ([Port usage](#port-usage-and-co-location)). DNS-SD is off by default, so no Avahi or D-Bus is needed; with `NMOS_DNS_SD=true` mount `/run/dbus` and `/run/avahi-daemon` and, on AppArmor hosts, add `security_opt: ["apparmor=unconfined"]`.

<a id="config-volume"></a>**The `/config` volume** must be writable (for uid 1000, or for the user the container runs as): the gateway writes `gateway.json` (UI saves, generated ids, a `.bak` of the previous version) and `state/connections.json` (§7.6) — its only state. Without it the container exits with code 78. The directory is the parent of `MXLGW_CONFIG` (default `/config/gateway.json`).

**MXL root.** `/Volumes/mxl` is a **tmpfs on the host** shared by every MXL container of the host: `/etc/fstab` entry `tmpfs /Volumes/mxl tmpfs size=8g,mode=1777 0 0` (the CBC `mxl-hands-on` convention, also used by mxl-decklink and mxl-fabrics-agent; `uid=1000,gid=1000,mode=0775` works as well). The gateway creates its output domain (`MXL_OUTPUT_DOMAIN_DIR`, e.g. `/Volumes/mxl/gw-studio1`) with `domain_def.json` and `options.json` and never overwrites existing ones. Other media functions mount the same root — each container may use its own container path; a domain's identity is its `domain_def.json` id, not its path. When the domain is only shared inside one Compose project, a Compose `tmpfs:` volume mounted by all services also works, but then no other container or mxl-fabrics-agent can see it.

## Running on the MXL PoC platform

The platform starts media functions with a shared MXL root (`/Volumes/mxl`, hostPath), uid/gid 1000, no `hostIPC`, one static NMOS registry (Registration `P`, Query `P+1`) without DNS-SD, and `NMOS_SEED=<production>-<function>`. For this gateway:

| Platform setting | Variable | Gateway behaviour |
|---|---|---|
| identity | `NMOS_SEED`, `NMOS_LABEL`, `NMOS_TAGS` | all NMOS ids (both nodes, devices, sources, flows, senders, receivers) and the output domain id are UUIDv5 of the seed; label = MXL node label and device label prefix (ST 2110 node: `<label> ST 2110`); tags on both nodes and devices |
| registry | `NMOS_REGISTRY_ADDRESS`, `NMOS_REGISTRY_PORT`, `NMOS_QUERY_ADDRESS` (default registry address), `NMOS_QUERY_PORT` (default port + 1), `NMOS_DNS_SD=false` | the MXL node registers there; the ST 2110 node does **not** (only with `MXLGW_NODE_ST2110_REGISTRY_ADDRESS`); no browsing, no mDNS, no Avahi/D-Bus |
| addresses | `NMOS_HOST_ADDRESS` | IPv4 literal in every href, `api.endpoints` and IS-05 href; default the default-route interface's address, else the first non-loopback IPv4 |
| ports | `NMOS_PORT` (MXL node, UI, API, metrics, health), `WEB_PORT` (optional separate UI/API port) | the ST 2110 node listens on `NMOS_PORT + 1` (`MXLGW_NODE_ST2110_HTTP_PORT`); no WebSocket port is opened; a port that cannot be bound exits 75 |
| MXL | `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_OUTPUT_DOMAIN_ID`, `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS` | the output domain is created if missing; an existing `domain_def.json` with another id is logged as an error and never overwritten; the gateway only writes into its own domain |
| lifecycle | `SHUTDOWN_TIMEOUT_S` (10), `MXL_CLEANUP_ON_EXIT` (false) | SIGTERM: media stopped, MXL released, every resource DELETEd from the registry, own output domain removed when enabled, exit 143 within the timeout |
| health | `/livez`, `/readyz` | ready only when serving and, with a registry configured, registered |
| export/import | `GET /api/v1/config/export`, `POST /api/v1/config/import?restart=true` | the configuration file holds no secrets, so the export is complete; the import restores and restarts |
| user | uid/gid 1000 | the image runs as 1000:1000; with the **dpdk** backend the container must run as root (`runAsUser: 0`, `runAsGroup: 1000`) because DPDK opens the root-owned VFIO device nodes ([Users and permissions](#users-and-permissions)) |
| network | host network | needed for production (DPDK NIC); allowed for the gateway by the platform |

Example: [`deploy/k8s/deployment.yaml`](deploy/k8s/deployment.yaml) (standard variables, probes, `terminationGracePeriodSeconds: 15` > `SHUTDOWN_TIMEOUT_S`, MXL root hostPath, writable `/config`, `IPC_LOCK` + `SYS_NICE` only).

## Host preparation

Applies to Compose and Kubernetes nodes. Ubuntu 24.04 and Debian 13 hosts work (the container userland is independent; the kernel needs VFIO).

1. **BIOS:** VT-d on, SR-IOV on (only for VFs), C-states limited for latency.
2. **Kernel command line:** `intel_iommu=on iommu=pt default_hugepagesz=1G hugepagesz=1G hugepages=<n>` (or 2 MB pages). 4 GiB of hugepages covers several 1080p50 groups; the preflight estimates the need per session.
3. **Bind the media PFs to `vfio-pci` persistently**, e.g. `driverctl set-override 0000:31:00.0 vfio-pci` and `… 0000:31:00.1 vfio-pci`. Keep E810 NVM/firmware as recommended by MTL (`doc/e800_series_drivers.md`). For VF mode use MTL's patched host `ice` driver.
4. **CPU isolation (recommended):** `isolcpus` / `nohz_full` / `rcu_nocbs` for the MTL lcores (`nic.lcores`), exclude them from irqbalance, keep `nic.app_cpus` on the NIC's NUMA node.
5. **Time:** discipline `CLOCK_TAI` to the facility grandmaster **with the correct TAI offset** (37 s): `ptp4l -f …` + `phc2sys -a -r` on a kernel-owned port, or chrony with a PHC/PTP reference and `leapsectz right/UTC`. This is required on **every** host that writes, replicates or reads MXL flows — including hosts that only run receivers — because grain indices are TAI-based and replicated 1:1 by mxl-fabrics-agent. See [PTP modes](#ptp-modes).
6. **MXL root:** one host tmpfs for all MXL domains of the host (`/Volumes/mxl`, see above), mounted by every MXL container of the host at its configured root path (gateway: `mxl.scan_path`, mxl-fabrics-agent: `MXL_ROOT`).
7. **Registry:** a static registry (`NMOS_REGISTRY_ADDRESS`, `NMOS_REGISTRY_PORT`) needs nothing on the host. Only for DNS-SD (`NMOS_DNS_SD=true`): an Avahi daemon on the host (`apt install avahi-daemon`), used through the mounted `/run/dbus` and `/run/avahi-daemon` sockets; on AppArmor hosts (Ubuntu) dbus-daemon refuses containers confined by Docker's default profile, so add `security_opt: ["apparmor=unconfined"]` (Kubernetes: `appArmorProfile: {type: Unconfined}`).

The E810 **DDP package** is part of the image (`/lib/firmware/updates/intel/ice/ddp/ice.pkg`); a host `/lib/firmware` mount may override it.

## Kubernetes

Manifests in [`deploy/k8s`](deploy/k8s) ([README](deploy/k8s/README.md)): SR-IOV Network Device Plugin with one resource per media port (`intel.com/e810_media_p`, `intel.com/e810_media_r`, injected as `PCIDEVICE_INTEL_COM_E810_MEDIA_P` and referenced as `"pci": "env:PCIDEVICE_INTEL_COM_E810_MEDIA_P"`), a `Deployment` with `replicas: 1`, `strategy: Recreate`, host networking, the platform's standard environment variables, Guaranteed QoS (MTL lcores and worker CPUs taken from the pod's cpuset), `hugepages-1Gi`, `IPC_LOCK` + `SYS_NICE` (unprivileged, uid 0 / gid 1000 for VFIO), startup/liveness/readiness probes, `terminationGracePeriodSeconds: 15`, the configuration on a PVC seeded once from a ConfigMap, and the node's MXL root as `hostPath`.

```bash
kubectl apply -k deploy/k8s
```

The two-node mxl-fabrics-agent scenario is in [`deploy/k8s/fabrics`](deploy/k8s/fabrics/README.md).

## Configuration

`/config/gateway.json` (path: `MXLGW_CONFIG`), validated against [`schema/gateway-config.schema.json`](schema/gateway-config.schema.json) and the semantic rules of §9.5. **Every setting is documented in [`docs/configuration.md`](docs/configuration.md)** (generated from the schema). Examples: [`config/examples`](config/examples).

- **Precedence:** environment variable > file > default for every scalar of `node`, `nic`, `ptp`, `mxl`. The platform's standard names are canonical (`NMOS_SEED`, `NMOS_LABEL`, `NMOS_PORT`, `NMOS_REGISTRY_ADDRESS`, `MXL_OUTPUT_DOMAIN_DIR`, `SHUTDOWN_TIMEOUT_S`, …); every other setting is `MXLGW_` + the upper-snake JSON path (`MXLGW_NIC_PRIMARY_PCI`, `MXLGW_PTP_MODE`, …); the older names stay valid as aliases (`MXLGW_HTTP_PORT`, `MXLGW_NODE_LABEL`, `MXLGW_NODE_PUBLIC_ADDRESS`, `MXLGW_MXL_SCAN_PATH`, …). Two variables of one setting with different values, or an unparsable value, stop the gateway with exit 78 naming the variable; unknown variables are ignored. Environment-set keys are read-only in the UI and never written into the file.
- **Secrets:** the configuration holds none (TLS uses file paths), nothing secret is logged, and the export is the complete file.
- **UI changes:** groups and essences apply live (only the edited group is rebuilt); `node`, `nic`, `ptp` and `mxl` changes are saved and flagged *restart required*.
- **Hand edits** are allowed and take effect after a restart (container restart or `POST /api/restart`). While the file differs from what the gateway wrote, the UI shows "configuration changed on disk" and blocks saves until you restart or overwrite.
- **Import / export:** `GET /api/v1/config/export` downloads the file; `POST /api/v1/config/import?keep_ids=true|false&restart=true|false` validates and writes it; with `restart=true` the gateway restarts gracefully (exit 0) so the orchestrator brings it up with the restored file, otherwise it is flagged *restart required*. `keep_ids=false` regenerates all ids for cloning a gateway (with `NMOS_SEED` the NMOS ids come from the seed anyway).
- An invalid file stops the gateway with **exit code 78** and every error printed (JSON pointer + message).

### Settings

Every setting except the per-group fields (groups and essences, see [`docs/configuration.md`](docs/configuration.md)):

<!-- settings-table:begin (generated by tools/gen_config_docs.py) -->

| Environment variable | Setting | Default | Description |
|---|---|---|---|
| `MXLGW_NODE_ID` | `node.id` | `null` | MXL node id when node.seed is not set; generated on first start and written back. Ignored when node.seed is set. |
| `NMOS_SEED`<br>alias `MXLGW_NODE_SEED` | `node.seed` | `null` | Seed for every NMOS id (nodes, devices, sources, flows, senders, receivers) and the default ids of configured MXL domains (UUIDv5, §7.3). Seed-derived ids are never written back. |
| `NMOS_LABEL`<br>alias `MXLGW_NODE_LABEL` | `node.label` | `"mxl-st2110-gateway"` | MXL node label; also the prefix of the device labels (IS-04). |
| `MXLGW_NODE_DESCRIPTION` | `node.description` | `"ST 2110 <-> MXL gateway"` | Node and Device description (IS-04). |
| `NMOS_TAGS`<br>alias `MXLGW_NODE_TAGS` | `node.tags` | `{}` | IS-04 tags added to both nodes and their devices (JSON object of string arrays). Senders and Receivers keep their BCP-002-01 group hints. |
| `NMOS_PORT`<br>alias `MXLGW_NODE_HTTP_PORT`, `MXLGW_HTTP_PORT` | `node.http_port` | `8080` | Port of the MXL node's IS-04 Node API and IS-05 Connection API; also serves the UI, /api, /metrics and health unless node.web_port is set. |
| `WEB_PORT`<br>alias `MXLGW_NODE_WEB_PORT` | `node.web_port` | `null` | Separate port for the admin UI, /api, /metrics and health; null = node.http_port. |
| `NMOS_HOST_ADDRESS`<br>alias `MXLGW_NODE_HOST_ADDRESS`, `MXLGW_NODE_PUBLIC_ADDRESS` | `node.host_address` | `null` | IPv4 literal announced in NMOS hrefs and api.endpoints; null = IPv4 of the default-route interface, else the first non-loopback IPv4. Never a hostname, 0.0.0.0 or 127.x. |
| — (file only) | `node.public_address` | `null` | Deprecated alias of node.host_address. |
| `MXLGW_NODE_PUBLIC_PORT` | `node.public_port` | `null` | Port advertised in NMOS hrefs (reverse proxy / port mapping). |
| `MXLGW_NODE_MANAGEMENT_ADDRESSES` | `node.management_addresses` | `[]` | Deprecated: additional IPv4 addresses announced in api.endpoints; empty = only node.host_address. |
| `NMOS_DNS_SD`<br>alias `MXLGW_NODE_REGISTRY_DNS_SD` | `node.registry.dns_sd` | `null` | Discover the registry with DNS-SD and advertise the node with mDNS (needs Avahi). null = true only if the deprecated mode is "dns-sd". |
| `MXLGW_NODE_REGISTRY_MODE` | `node.registry.mode` |  | Deprecated: "dns-sd" = dns_sd true, "static" = dns_sd false with address and port. |
| `NMOS_REGISTRY_ADDRESS`<br>alias `MXLGW_NODE_REGISTRY_ADDRESS` | `node.registry.address` | `null` | Registration API host (IPv4 or DNS name); null = no static registry. |
| `NMOS_REGISTRY_PORT`<br>alias `MXLGW_NODE_REGISTRY_PORT` | `node.registry.port` | `null` | Registration API port; null = 3210. |
| `NMOS_QUERY_ADDRESS`<br>alias `MXLGW_NODE_REGISTRY_QUERY_ADDRESS` | `node.registry.query_address` | `null` | Query API host (reported in /api/nmos; the gateway does not query); null = registry address. |
| `NMOS_QUERY_PORT`<br>alias `MXLGW_NODE_REGISTRY_QUERY_PORT` | `node.registry.query_port` | `null` | Query API port; null = registry port + 1. |
| `MXLGW_NODE_ST2110_ENABLED` | `node.st2110.enabled` | `true` | Run the ST 2110 node (RTP Receivers of ingest groups, RTP Senders of egress groups). false = RTP connections only from the group defaults. |
| `MXLGW_NODE_ST2110_LABEL` | `node.st2110.label` | `null` | ST 2110 node and device label; null = node.label + " ST 2110". |
| `MXLGW_NODE_ST2110_HTTP_PORT` | `node.st2110.http_port` | `null` | Port of the ST 2110 node's IS-04 Node API and IS-05 Connection API; null = node.http_port + 1. |
| `MXLGW_NODE_ST2110_HOST_ADDRESS` | `node.st2110.host_address` | `null` | IPv4 literal announced by the ST 2110 node; null = the MXL node's host address. |
| `MXLGW_NODE_ST2110_REGISTRY_DNS_SD` | `node.st2110.registry.dns_sd` | `null` | Discover the registry with DNS-SD and advertise the node with mDNS (needs Avahi). null = true only if the deprecated mode is "dns-sd". |
| — (file only) | `node.st2110.registry.mode` |  | Deprecated: "dns-sd" = dns_sd true, "static" = dns_sd false with address and port. |
| `MXLGW_NODE_ST2110_REGISTRY_ADDRESS` | `node.st2110.registry.address` | `null` | Registration API host (IPv4 or DNS name); null = no static registry. |
| `MXLGW_NODE_ST2110_REGISTRY_PORT` | `node.st2110.registry.port` | `null` | Registration API port; null = 3210. |
| — (file only) | `node.st2110.registry.query_address` | `null` | Query API host (reported in /api/nmos; the gateway does not query); null = registry address. |
| — (file only) | `node.st2110.registry.query_port` | `null` | Query API port; null = registry port + 1. |
| `MXLGW_NODE_TLS_ENABLED` | `node.tls.enabled` | `false` | Serve HTTPS instead of HTTP on node.http_port. |
| `MXLGW_NODE_TLS_CERTIFICATE` | `node.tls.certificate` | `null` | PEM certificate chain file. |
| `MXLGW_NODE_TLS_PRIVATE_KEY` | `node.tls.private_key` | `null` | PEM private key file. |
| `MXLGW_NODE_RESUME_CONNECTIONS` | `node.resume_connections` | `true` | Restore the last IS-05 /active of every Sender and Receiver after a restart (state/connections.json, §7.6). |
| `MXLGW_NODE_LOG_LEVEL`<br>alias `MXLGW_LOG_LEVEL` | `node.log_level` | `"info"` | Gateway log level (MTL/DPDK messages are mapped onto it). |
| `SHUTDOWN_TIMEOUT_S`<br>alias `MXLGW_NODE_SHUTDOWN_TIMEOUT_S` | `node.shutdown_timeout_s` | `10` | SIGTERM: upper bound for the graceful shutdown (media stop, MXL release, NMOS deregistration, domain cleanup); the process exits 143 at the latest after this time. |
| `MXLGW_NIC_BACKEND` | `nic.backend` | `"dpdk"` | dpdk (production), kernel and mock are test-only. |
| `MXLGW_NIC_LCORES` | `nic.lcores` | `null` | MTL lcores as a CPU list (e.g. "4-9"); disjoint from app_cpus. null = dpdk backend: the first nic.lcore_count CPUs of the process's CPU affinity (Kubernetes cpuset); kernel/mock: MTL's choice. |
| `MXLGW_NIC_LCORE_COUNT` | `nic.lcore_count` | `4` | Number of MTL lcores taken from the CPU affinity when nic.lcores is null (dpdk backend). |
| `MXLGW_NIC_TX_PACING` | `nic.tx_pacing` | `"auto"` | MTL TX pacing (dpdk backend): auto = NIC rate limiter where the driver has one, else TSC; rl = rate limiter only; tsc = software (TSC) pacing, never touches the NIC rate limiter. Use tsc when MTL fails to start with ice "Failed to add lan txq". |
| `MXLGW_NIC_APP_CPUS` | `nic.app_cpus` | `null` | CPU list for the gateway worker threads; null = dpdk backend: the CPUs of the affinity not used as lcores. |
| `MXLGW_NIC_HUGEPAGE_SOCKET` | `nic.hugepage_socket` | `"auto"` | NUMA socket for hugepage memory; auto = the NIC's socket. |
| — (file only) | `nic.port_pairs` | `[]` | The media port pair (exactly one in v1). |
| — (file only) | `nic.port_pairs[].name` | `"media"` | Port pair name. |
| `MXLGW_NIC_PRIMARY_NAME` | `nic.port_pairs[].primary.name` |  | Port name, also the NMOS interface name. |
| `MXLGW_NIC_PRIMARY_PCI` | `nic.port_pairs[].primary.pci` |  | PCI address (dddd:bb:dd.f) or env:VARIABLE (Kubernetes device plugin). |
| `MXLGW_NIC_PRIMARY_IFNAME` | `nic.port_pairs[].primary.ifname` |  | Kernel interface (nic.backend = kernel only). |
| `MXLGW_NIC_PRIMARY_IP` | `nic.port_pairs[].primary.ip` |  | Port IPv4 address: ST 2110 source address and IS-05 interface_ip. |
| `MXLGW_NIC_PRIMARY_NETMASK` | `nic.port_pairs[].primary.netmask` |  | Subnet mask of the port. |
| `MXLGW_NIC_PRIMARY_GATEWAY` | `nic.port_pairs[].primary.gateway` | `null` | Default gateway for unicast destinations outside the subnet (optional). |
| — (file only) | `nic.port_pairs[].redundant` | `null` | Redundant port (2022-7 leg r); required when any group has redundancy. |
| `MXLGW_NIC_REDUNDANT_NAME` | `nic.port_pairs[].redundant.name` |  | Port name, also the NMOS interface name. |
| `MXLGW_NIC_REDUNDANT_PCI` | `nic.port_pairs[].redundant.pci` |  | PCI address (dddd:bb:dd.f) or env:VARIABLE (Kubernetes device plugin). |
| `MXLGW_NIC_REDUNDANT_IFNAME` | `nic.port_pairs[].redundant.ifname` |  | Kernel interface (nic.backend = kernel only). |
| `MXLGW_NIC_REDUNDANT_IP` | `nic.port_pairs[].redundant.ip` |  | Port IPv4 address: ST 2110 source address and IS-05 interface_ip. |
| `MXLGW_NIC_REDUNDANT_NETMASK` | `nic.port_pairs[].redundant.netmask` |  | Subnet mask of the port. |
| `MXLGW_NIC_REDUNDANT_GATEWAY` | `nic.port_pairs[].redundant.gateway` | `null` | Default gateway for unicast destinations outside the subnet (optional). |
| `MXLGW_PTP_MODE` | `ptp.mode` | `"builtin"` | builtin: MTL PTP on the media ports (host CLOCK_TAI disciplined separately); builtin_phc2sys: MTL also steers the host clock (needs CAP_SYS_TIME); external: no MTL PTP, host CLOCK_TAI is used (§5.2). |
| `MXLGW_PTP_DOMAIN` | `ptp.domain` | `127` | PTP domain number (MTL patch 0002 filters on it). |
| `MXLGW_PTP_REQUIRE_LOCK` | `ptp.require_lock` | `true` | Not ready (/readyz) until PTP is locked. |
| `MXLGW_PTP_WARN_OFFSET_NS` | `ptp.warn_offset_ns` | `10000` | Warning threshold for \|MTL PTP time − host CLOCK_TAI\|. |
| `MXLGW_PTP_MAX_OFFSET_NS` | `ptp.max_offset_ns` | `1000000` | Not ready beyond this \|MTL PTP time − host CLOCK_TAI\|. |
| `MXL_DOMAIN_SCAN_PATH`<br>alias `MXLGW_MXL_SCAN_PATH` | `mxl.scan_path` | `"/Volumes/mxl"` | MXL root scanned for discovered and mirror domains (§8.5); null = off. |
| `MXLGW_MXL_DEFAULT_READ_OFFSET_GRAINS`<br>alias `MXL_READ_OFFSET_GRAINS` | `mxl.default_read_offset_grains` | `null` | Default read offset in grains for MXL Receivers without their own (§5.7); exclusive with default_read_offset_ns. |
| `MXLGW_MXL_DEFAULT_READ_OFFSET_NS`<br>alias `MXL_READ_OFFSET_MS` (milliseconds) | `mxl.default_read_offset_ns` | `null` | Default read offset in ns for MXL Receivers without their own (§5.7). |
| — (file only) | `mxl.domains` | `[]` | Configured domains: the only domains the gateway writes to. The first one is the output domain of MXL_OUTPUT_DOMAIN_*. |
| — (file only) | `mxl.domains[].name` |  | Name referenced by groups[].domain. |
| `MXL_OUTPUT_DOMAIN_DIR` (first domain)<br>alias `MXLGW_MXL_DOMAIN_<NAME>_PATH` | `mxl.domains[].path` |  | Domain directory on a tmpfs (never mirror-*); created if missing. |
| `MXL_OUTPUT_DOMAIN_ID` (first domain)<br>alias `MXLGW_MXL_DOMAIN_<NAME>_ID` | `mxl.domains[].id` | `null` | Domain id; null = taken from domain_def.json or generated, then written back (§8.3). |
| `MXLGW_MXL_DOMAIN_<NAME>_LABEL` | `mxl.domains[].label` | `null` | Label written into a new domain_def.json. |
| `MXLGW_MXL_DOMAIN_<NAME>_DESCRIPTION` | `mxl.domains[].description` | `null` | Description written into a new domain_def.json. |
| `MXL_OUTPUT_DOMAIN_HISTORY_DURATION_NS` (first domain)<br>alias `MXLGW_MXL_DOMAIN_<NAME>_HISTORY_DURATION_NS` | `mxl.domains[].history_duration_ns` | `null` | Ring history written into options.json if the file is missing; null = MXL default. |
| `MXLGW_MXL_DOMAIN_<NAME>_GC_ON_START` | `mxl.domains[].gc_on_start` | `false` | Garbage-collect every stale flow of the domain at start (default: only the gateway's own). |
| `MXL_CLEANUP_ON_EXIT`<br>alias `MXLGW_MXL_CLEANUP_ON_EXIT` | `mxl.cleanup_on_exit` | `false` | On SIGTERM/SIGINT remove the directories of the configured domains after the MXL writers are released (only if their domain_def.json carries the gateway's id and no other process writes a flow there). |
| `MXLGW_CONFIG` | — | | configuration file path (default `/config/gateway.json`); its directory holds all state the gateway writes (`gateway.json`, `gateway.json.bak`, `state/connections.json`) |
| `MXLGW_LOG_FORMAT` | — | | `json` (default) or `text` |

<!-- settings-table:end -->

## Admin UI and REST API

`http://<host>:<NMOS_PORT>/admin/` (or `WEB_PORT`) — a single embedded page, works offline, follows the browser's light/dark theme.

| Tab | Content |
|---|---|
| Dashboard | versions, readiness and reasons, PTP lock and grandmaster, MTL − host TAI offset, links, per-group tiles with essence state, bitrate, counters, 2022-7 leg health, resolved MXL domain per egress essence (mirror marked) |
| Groups | create by counts (video × n, audio × m, ANC × k), edit essences (format, payload type, legs, read offsets), duplicate, delete |
| NMOS | both nodes (ids, address, registry, registration), all Senders/Receivers with their node, active parameters and SDPs |
| Network | port pair form (PCI or interface, IP, netmask, gateway), bind mode, MAC, link, DDP package, counters |
| PTP | mode, domain, lock per port, BMCA selection, grandmaster data, offset and path delay sparklines |
| MXL | configured, discovered and mirror domains, flow browser, MXL Receiver resolution and read lag |
| Configuration | export / import with validation report, raw file, changed-on-disk resolution, restart, preflight, logs |

Every field shows its provenance (default / file / environment); server-side validation errors appear on the field.

**REST API** (SPECIFICATION.md §11.3). Every route answers under `/api/v1/…` (the v1 contract) and, unchanged, under `/api/…`:

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/v1/status` | GET | everything the dashboard shows (nodes, readiness, media, PTP, groups, domains, NMOS) |
| `/api/v1/config` | GET / PUT | file, effective configuration, provenance, `ETag`; full replace with `If-Match` |
| `/api/v1/config/export` | GET | the configuration file byte for byte (no secrets in it) |
| `/api/v1/config/import` | POST | restore a file (`?keep_ids=true|false`, `?restart=true` applies it by a graceful restart) |
| `/api/v1/config/validate` | POST | validate a file without writing it |
| `/api/v1/schema` | GET | the configuration JSON Schema |
| `/api/v1/groups`, `/api/v1/groups/{uid}` | POST / PUT / DELETE | create (by counts), edit, delete groups — applied live |
| `/api/v1/nic`, `/api/v1/ptp`, `/api/v1/domains`, `/api/v1/flows?domain=<id>`, `/api/v1/nmos` | GET | tab data; `/api/v1/nmos` lists both NMOS nodes and all Senders/Receivers |
| `/api/v1/preflight`, `/api/v1/logs` | GET | preflight results, last log lines |
| `/api/v1/restart` | POST | graceful exit 0 for the supervisor to restart |

NMOS: IS-04 Node API and IS-05 Connection API at `/x-nmos/node/v1.3/`, `/x-nmos/connection/v1.1|v1.2/` on the MXL node (`NMOS_PORT`) and on the ST 2110 node (`NMOS_PORT + 1`). Mutating `/api` requests need `Content-Type: application/json` and are rejected cross-origin. There is no authentication: restrict the management network.

Health: `/livez` (process alive), `/readyz` (200 only when serving: the config is valid, MTL is up, PTP is locked unless `require_lock=false`, the clock offset is within `ptp.max_offset_ns`, all configured domains are usable and — for each node with a configured registry — the node is registered; essences waiting for flows do not count; `shutting_down` during the shutdown), `/statusz` (text), `/metrics` (Prometheus, prefix `mxl_st2110_gateway_`).

## Exit codes and shutdown

| Code | Meaning |
|---|---|
| `0` | normal exit, or a restart requested through `/api/restart` or an import with `restart=true` |
| `1` | runtime failure (e.g. the media backend cannot start) |
| `75` | a listening port (`NMOS_PORT`, `NMOS_PORT + 1`, `WEB_PORT`) cannot be bound |
| `78` | configuration or environment error (invalid file or variable, conflicting aliases, preflight failure, unusable MXL domain); the reason is logged every time |
| `130` / `143` | stopped by `SIGINT` / `SIGTERM` after the graceful shutdown |

On `SIGTERM` (`docker stop`, pod deletion) the gateway, within `SHUTDOWN_TIMEOUT_S` (default 10 s): stops the MTL sessions and releases every MXL writer and reader; removes all its NMOS resources so the registries receive DELETEs (node last; at most 3 s); closes its listeners; releases MXL and MTL; with `MXL_CLEANUP_ON_EXIT=true` removes its own output domain(s) — only directories whose `domain_def.json` carries the gateway's id and in which no other process writes; then exits 143. After the timeout it exits 143 regardless. Give the orchestrator a longer grace period (`stop_grace_period: 15s`, `terminationGracePeriodSeconds: 15`).

## Users and permissions

The image runs as **uid/gid 1000** (`USER 1000:1000`), the platform's media-function user; `/config` belongs to it. Setup mode and the test backends need nothing else.

With the **dpdk** backend the container runs as **root with group 1000** (`user: "0:1000"`, Kubernetes `runAsUser: 0`, `runAsGroup: 1000`, `supplementalGroups: [1000]`): DPDK opens the VFIO group device nodes (`/dev/vfio/<group>`), which are root-owned on most hosts, maps hugepages from the host's hugetlbfs and pins DMA memory (`IPC_LOCK`). The gateway sets `umask 002`, so the domains, flows and files it creates are group 1000 and group-writable for the other media functions. Running DPDK as uid 1000 needs the VFIO device nodes owned by that user (containerd `device_ownership_from_security_context = true`, or a udev rule such as `SUBSYSTEM=="vfio", GROUP="1000", MODE="0660"`), a writable hugetlbfs and an unlimited memlock limit; this is not tested on hardware yet (`docs/decisions.md`, O-10).

## PTP modes

| `ptp.mode` | What happens | Use when |
|---|---|---|
| `builtin` (default) | MTL runs PTP on **both** media ports (patched MTL: domain filter, BMCA across ports, per-Announce grandmaster tracking). The host clock is **not touched**: discipline `CLOCK_TAI` separately (step 5 of host preparation). The gateway measures MTL time − host `CLOCK_TAI` every second (`mxl_st2110_gateway_clock_mtl_minus_host_tai_ns`); beyond `ptp.max_offset_ns` it is not ready. | normal and shared hosts, Kubernetes |
| `builtin_phc2sys` | Additionally MTL steers `CLOCK_REALTIME` to the PHC **without subtracting the UTC offset**: the wall clock shows TAI (37 s ahead of UTC). The kernel TAI offset must be 0 (checked at start), NTP/chrony must be off, `CAP_SYS_TIME` is needed. | dedicated single-purpose appliances only |
| `external` | No MTL PTP; the host `CLOCK_TAI` is the only time source. PTP metrics are absent. | VF deployments, hosts already PTP-locked through the kernel; always with the test-only kernel backend |

The UTC offset is only displayed; RTP, MTL and MXL all work on TAI.

## Port usage and co-location

Under host networking all containers of a host share one port space.

| Container | Default port(s) | Protocol | Purpose |
|---|---|---|---|
| mxl-st2110-gateway | `8080` (`NMOS_PORT` = `node.http_port`, aliases `MXLGW_HTTP_PORT`, `MXLGW_NODE_HTTP_PORT`) | TCP (HTTP/HTTPS) | MXL node: IS-04 Node API, IS-05 Connection API; plus admin UI, `/api`, `/metrics`, health unless `WEB_PORT` is set |
| mxl-st2110-gateway | `8081` (`NMOS_PORT + 1`, `MXLGW_NODE_ST2110_HTTP_PORT`) | TCP (HTTP/HTTPS) | ST 2110 node: IS-04 Node API, IS-05 Connection API (not opened with `MXLGW_NODE_ST2110_ENABLED=false`) |
| mxl-st2110-gateway | none (`WEB_PORT` = `node.web_port`, optional) | TCP | admin UI, `/api`, `/metrics`, health on a separate port |
| mxl-st2110-gateway | none | — | no WebSocket port: IS-07 Events WebSocket, nmos-cpp Settings/Logging APIs and the other optional APIs are disabled |
| mxl-st2110-gateway | none on the host stack | — | ST 2110 media and PTP run on DPDK-owned ports; only the test-only kernel backend uses UDP ports of the configured legs on its test interfaces |
| mxl-st2110-gateway | `5353/udp` via the host's Avahi, only with `NMOS_DNS_SD=true` | mDNS | DNS-SD (registry discovery), shared host daemon |
| mxl-decklink | `8080` | TCP | web UI, REST, health, metrics |
| mxl-decklink | `3212`, `3213` | TCP | NMOS Node/Connection API, IS-07 WebSocket |
| mxl-fabrics-agent | `8095` | TCP | UI, REST/control API, health, metrics |
| mxl-fabrics-agent | `3232`, `3233` | TCP | NMOS Node API, WebSocket |
| mxl-fabrics-agent | `23500`–`23599` | TCP / RDMA CM | fabric data ports (target pool) |

**With mxl-decklink on the same host** (both default to 8080), either (a) set `NMOS_PORT=8090` (8090 and 8091 are free in the table) under host networking, (b) use a bridge network with a port mapping and set `node.public_port` / `NMOS_HOST_ADDRESS` so NMOS hrefs advertise the mapped port, or (c) put a reverse proxy in front and set `NMOS_HOST_ADDRESS` / `node.public_port` to the proxy. Two gateways on one host need different `NMOS_PORT`s two apart (each uses `NMOS_PORT` and `NMOS_PORT + 1`). DNS-SD discovery (optional) needs host networking. The preflight warns when the port is in use or equals a sibling default.

## Multi-host operation with mxl-fabrics-agent

[mxl-fabrics-agent](https://github.com/LeeO86/mxl-fabrics-agent) runs one container per host and replicates the MXL flows that enabled MXL Receivers need from the host that writes them, using the MXL Fabrics API (RDMA). The gateway has no Fabrics code and needs no configuration for it:

- **Host A, gateway as MXL Sender (ingest):** flows are written into a configured domain directly under the host's MXL root, with a stable domain id (written back into the config) and stable flow ids.
- **Host B, gateway as MXL Receiver (egress):** a controller stages host A's `mxl_domain_id` and `mxl_flow_id`. The agent on host B creates `mirror-<domain-id>` next to the local domains; the receiver finds it by id, waits (`waiting_for_flow`, then `no_signal`) until grains arrive and starts without further action. Read a few grains behind the writer on host B (`read_offset_grains` or `mxl.default_read_offset_grains`; the example uses 2) and watch `mxl_st2110_gateway_mxl_read_lag_grains`.

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

5. Check `mxl_st2110_gateway_mxl_reader_info{domain_kind="mirror"}` and `mxl_st2110_gateway_mxl_read_lag_grains` on host B (Grafana row *Egress*).

## Metrics and Grafana

`/metrics` (Prometheus text format) on the web port (`NMOS_PORT` unless `WEB_PORT` is set); every metric starts with `mxl_st2110_gateway_` and is listed in [`docs/metrics.md`](docs/metrics.md). Metric names and labels are a stable public interface. Dashboard: [`monitoring/grafana/mxl-st2110-gateway.json`](monitoring/grafana/mxl-st2110-gateway.json) (Grafana ≥ 11; variables `datasource`, `instance`, `group`, `essence`; rows Overview, PTP, NIC, Ingest, Egress, NMOS, MXL domains). Scrape example: [`monitoring/prometheus/scrape-example.yaml`](monitoring/prometheus/scrape-example.yaml); Prometheus Operator: [`deploy/k8s/servicemonitor.yaml`](deploy/k8s/servicemonitor.yaml).

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

<a id="preflight-http-port"></a>**http-port** — `NMOS_PORT`, `NMOS_PORT + 1` (ST 2110 node) or `WEB_PORT` is already in use (warning; the listener then exits 75), or `NMOS_PORT` equals a default port of mxl-decklink / mxl-fabrics-agent (warning); see [Port usage](#port-usage-and-co-location).

<a id="preflight-lcores"></a>**lcores** — `nic.lcores` names a CPU that does not exist on this host.

Other frequent issues:

- **Essence `waiting_for_flow`** (egress): the MXL flow or domain does not exist (yet). It starts automatically once it appears; check the flow id, `mxl.scan_path` and, for remote flows, the agent. **`no_signal`**: the flow exists but no grains arrive in time.
- **Ingest essence `no_signal` / `no_packets` while the source sends**: before 1.0.7 the RTP receiver listened for the essence's configured `payload_type`, not the SDP's (MTL drops other payload types; VideoIPath audio often uses 98). 1.0.7 takes it from the staged SDP; `ingest_receiver_started` logs the payload type in use.
- **CrashLoopBackOff with `EAL: Cannot set affinity` and `media_backend_failed`** in a Guaranteed pod (exclusive CPUs, CPU 0 reserved): before 1.0.9 DPDK's main lcore was CPU 0, outside the pod's CPUs. 1.0.9 puts it on the first app CPU; `mtl_init` logs `main_lcore`.
- **TX audio below 1000 packets/s (ptime 1 ms), gaps at the receiver, `late_frames` growing**: before 1.0.8 the audio blocks of a TX group were handed to MTL once per grain period behind the video, and with a read offset close to the output delay MTL dropped the first blocks of every period as late. 1.0.8 hands each block over when its data is due; `mxl_st2110_gateway_egress_lead_ns` shows the remaining lead (about output delay − read offset − 1 ms for an on-time source).
- **Essence `error` / `format_mismatch`**: the flow or SDP format differs from the essence configuration (size, rate, scan, channels, packet time).
- **Essence `error` / `egress_sender_failed`**: the ST 2110 sender could not be created (MTL error in the log just before `egress_sender_failed`); the MXL side may still read. Retried with a growing delay.
- **`/readyz` `clock_mismatch`**: MTL PTP time and host `CLOCK_TAI` differ by more than `ptp.max_offset_ns` — fix host time sync.
- **`/readyz` `nmos_not_registered`** / **`st2110_nmos_not_registered`**: the MXL node / ST 2110 node has a registry configured but is not registered; check `NMOS_REGISTRY_ADDRESS`/`_PORT` (or `MXLGW_NODE_ST2110_REGISTRY_*`), the registry's reachability, or Avahi when DNS-SD is on. Without a configured registry registration is not required.
- **Exit 75 `http_listen_failed`**: `NMOS_PORT`, `NMOS_PORT + 1` or `WEB_PORT` is taken (another gateway, mxl-decklink, …); see [Port usage](#port-usage-and-co-location).
- **`domain_id_mismatch` (error)**: the output domain's `domain_def.json` carries another id than `MXL_OUTPUT_DOMAIN_ID` / the seed-derived id; the file is kept and its id used. Remove the directory (or fix the id) if it belongs to another function.
- **`mtl_init failed` after `ice_tx_queue_start(): Failed to add lan txq`** and `fallback to tsc as rl init fail` (E810 PF): the NIC rate limiter cannot be set up; MTL's fallback to TSC pacing does not always recover the port. Set `nic.tx_pacing` to `tsc` (`MXLGW_NIC_TX_PACING=tsc`).
- **`PTP(0): t3 tx timestamp timeout`**, PTP never locks: MTL's PTP gets no TX timestamp from the NIC for its Delay_Req. Use `ptp.mode = external` (`MXLGW_PTP_MODE=external`) with the host's `CLOCK_TAI` disciplined to the same grandmaster as the media network and a kernel TAI offset of 37 s ([tai-offset](#preflight-tai-offset)).

## Development

```bash
docker build --target build -f docker/Dockerfile -t mxlgw-build .   # compile + unit tests
docker build -f docker/Dockerfile -t mxlgw:dev .                    # runtime image
tests/integration/smoke.sh mxlgw:dev                                # container smoke test
tests/integration/lifecycle.sh mxlgw:dev                            # platform lifecycle: registry, SIGTERM, cleanup
tests/integration/loopback.sh mxlgw:dev                             # kernel-backend media loopback (sudo, hugepages)
tests/integration/late-flow.sh mxlgw:dev                            # receiver before its flow, mirror domain
tests/integration/nmos-testing.sh mxlgw:dev                         # AMWA suites (sudo, avahi)
python3 monitoring/tools/gen_dashboard.py --check                   # dashboard up to date
python3 tools/gen_config_docs.py --check                            # docs/configuration.md up to date
cd web && npm ci && npm test && npm run dev                         # UI against a gateway on :8080
```

Build the slow `deps` stage once (`docker build --target deps -f docker/Dockerfile -t mxlgw-deps .`). A local CMake build needs MXL, MTL/DPDK and nmos-cpp under `CMAKE_PREFIX_PATH`; without them only the pure core and its unit tests are built. Contribution rules: [`AGENTS.md`](AGENTS.md).

## Conformance

AMWA nmos-testing IS-04-01, IS-05-01 and IS-05-02 (both NMOS nodes) and BCP-007-03-01 (MXL node) run in CI ([`tests/integration/nmos-testing.sh`](tests/integration/nmos-testing.sh)); results and documented warnings are in [`docs/conformance.md`](docs/conformance.md). Hardware acceptance: [`docs/acceptance.md`](docs/acceptance.md); capacity: [`docs/performance.md`](docs/performance.md).

## License

MIT — see [`LICENSE`](LICENSE). Third-party components and their licenses: [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
