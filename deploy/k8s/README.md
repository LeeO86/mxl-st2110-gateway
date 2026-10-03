# Kubernetes deployment

Manifests for running `mxl-st2110-gateway` on a Kubernetes node with an Intel E810 (SPECIFICATION.md §15.2). All files pass `kubeconform` in CI.

| File | Purpose |
|---|---|
| `namespace.yaml` | namespace `mxl` |
| `sriov-dp-configmap.yaml` | SR-IOV Network Device Plugin config: one resource per media port (`intel.com/e810_media_p`, `intel.com/e810_media_r`) |
| `pvc.yaml` | PersistentVolumeClaim for `/config` (the gateway writes its configuration and `state/`) |
| `configmap-seed.yaml` | first-start configuration, copied to `/config/gateway.json` only if absent |
| `deployment.yaml` | the gateway: `replicas: 1`, `Recreate`, host network, Guaranteed QoS, hugepages, probes, the platform's standard environment variables |
| `service.yaml` | in-cluster access and scrape target (`http` 8080: MXL node, UI, API, metrics; `nmos-st2110` 8081: ST 2110 node) |
| `servicemonitor.yaml` | optional Prometheus Operator `ServiceMonitor` (not in `kustomization.yaml`) |
| `kustomization.yaml` | `kubectl apply -k deploy/k8s` |
| `fabrics/` | optional two-node scenario with mxl-fabrics-agent ([README](fabrics/README.md)) |

## Node preparation

Follow the README section "Host preparation" on the node, then:

1. Bind both media PFs to `vfio-pci` persistently, e.g. `driverctl set-override 0000:31:00.0 vfio-pci` and `driverctl set-override 0000:31:00.1 vfio-pci`.
2. Reserve 1 GiB hugepages on the kernel command line (`default_hugepagesz=1G hugepagesz=1G hugepages=8`) so the kubelet reports `hugepages-1Gi`.
3. Create the node's MXL root as a tmpfs (`/etc/fstab`: `tmpfs /Volumes/mxl tmpfs size=8g,mode=1777 0 0`). The `hostPath` volume has `type: Directory`, so the pod does not start until it exists.
4. Discipline `CLOCK_TAI` (linuxptp DaemonSet / PTP operator, or `ptp4l` + `phc2sys -a -r` on the node). With mxl-fabrics-agent this is required on **every** node that writes, replicates or reads flows (§5.1).
5. Enable the static CPU manager policy (`--cpu-manager-policy=static`) so the integer CPU request gets exclusive cores for the MTL lcores.
6. Label the node: `kubectl label node <node> mxl.example.net/st2110-gateway=true`.
7. Install the [SR-IOV Network Device Plugin](https://github.com/k8snetworkplumbingwg/sriov-network-device-plugin) with `sriov-dp-configmap.yaml` (adjust `pciAddresses`). It injects `PCIDEVICE_INTEL_COM_E810_MEDIA_P` / `_R`, which the seed configuration uses as `"pci": "env:PCIDEVICE_INTEL_COM_E810_MEDIA_P"`.

## Deploy

```bash
kubectl apply -k deploy/k8s
kubectl -n mxl rollout status deploy/mxl-st2110-gateway
kubectl -n mxl port-forward deploy/mxl-st2110-gateway 8080:8080   # or http://<node-ip>:8080/admin/
```

The startup probe allows 150 s for DPDK initialisation; readiness follows `/readyz` (PTP lock, clock offset, domains, registration with the configured registry). On `SIGTERM` the gateway stops the media, releases MXL, deletes its resources from the registry and exits 143 within `SHUTDOWN_TIMEOUT_S` (10 s), so `terminationGracePeriodSeconds: 15` is enough.

## Notes

- **Environment:** `deployment.yaml` sets the platform's standard variables (`NMOS_SEED`, `NMOS_LABEL`, `NMOS_TAGS`, `NMOS_HOST_ADDRESS` from `status.hostIP`, `NMOS_REGISTRY_ADDRESS`/`_PORT`, `NMOS_DNS_SD=false`, `NMOS_PORT`, `MXL_DOMAIN_SCAN_PATH`, `MXL_OUTPUT_DOMAIN_DIR`, `MXL_CLEANUP_ON_EXIT`, `SHUTDOWN_TIMEOUT_S`). Every other scalar setting can be set the same way (README "Settings"); such settings are read-only in the UI.
- **Two NMOS nodes:** the MXL node (`NMOS_PORT`) registers with the MXL registry; the ST 2110 node (`NMOS_PORT + 1`) runs without a registry unless `MXLGW_NODE_ST2110_REGISTRY_ADDRESS` (or DNS-SD) points it at the facility's ST 2110 registry.
- **Host network** is used because the media NIC is owned by DPDK and NMOS announces the node's address. A pod network works too: set `NMOS_HOST_ADDRESS` to an address the registry and controllers reach and expose `NMOS_PORT` and `NMOS_PORT + 1`. No DNS-SD, so no Avahi or D-Bus mounts.
- **Users and capabilities:** the image runs as uid/gid 1000; this deployment runs the gateway as uid 0 with gid 1000 because DPDK opens the root-owned VFIO group device nodes (README "Users and permissions"). `IPC_LOCK` and `SYS_NICE` (+ `SYS_TIME` only for `ptp.mode = builtin_phc2sys`), `privileged: false`, `allowPrivilegeEscalation: false`, no `hostIPC`. `IPC_LOCK` lifts `RLIMIT_MEMLOCK` for vfio DMA pinning; verify on your containerd version (open item in `docs/decisions.md`).
- **CPUs:** with `nic.lcores` unset the gateway takes `nic.lcore_count` (default 4) MTL lcores from the pod's cpuset and runs its worker threads on the rest (`cpu_placement` log line).
- **MXL domain:** the default is a `hostPath` of the node's MXL root so media functions in other pods (and the mxl-fabrics-agent DaemonSet) share the domains. When every MXL consumer is a container of the same pod, an `emptyDir: {medium: Memory}` works instead — it is not shared across pods.
- **Metrics:** `kubectl apply -f deploy/k8s/servicemonitor.yaml` with the Prometheus Operator; the dashboard is `monitoring/grafana/mxl-st2110-gateway.json`.
