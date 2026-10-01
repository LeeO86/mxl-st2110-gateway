# Kubernetes deployment

Manifests for running `mxl-st2110-gateway` on a Kubernetes node with an Intel E810 (SPECIFICATION.md §15.2). All files pass `kubeconform` in CI.

| File | Purpose |
|---|---|
| `namespace.yaml` | namespace `mxl` |
| `sriov-dp-configmap.yaml` | SR-IOV Network Device Plugin config: one resource per media port (`intel.com/e810_media_p`, `intel.com/e810_media_r`) |
| `pvc.yaml` | PersistentVolumeClaim for `/config` (the gateway writes its configuration and `state/`) |
| `configmap-seed.yaml` | first-start configuration, copied to `/config/gateway.json` only if absent |
| `deployment.yaml` | the gateway: `replicas: 1`, `Recreate`, host network, Guaranteed QoS, hugepages, probes |
| `service.yaml` | in-cluster access and scrape target (`http`, port 8080) |
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

The startup probe allows 150 s for DPDK initialisation; readiness follows `/readyz` (PTP lock, clock offset, domains, NMOS registration).

## Notes

- **Host network** is the default so DNS-SD finds the registry and NMOS advertises the node's management address. Alternative: pod network + `Service` + `node.registry.mode = "static"` + `node.public_address` / `public_port`.
- **Capabilities:** `IPC_LOCK` and `SYS_NICE` (+ `SYS_TIME` only for `ptp.mode = builtin_phc2sys`), `privileged: false`, `allowPrivilegeEscalation: false`. `IPC_LOCK` lifts `RLIMIT_MEMLOCK` for vfio DMA pinning; verify on your containerd version (open item in `docs/decisions.md`).
- **MXL domain:** the default is a `hostPath` of the node's MXL root so media functions in other pods (and the mxl-fabrics-agent DaemonSet) share the domains. When every MXL consumer is a container of the same pod, an `emptyDir: {medium: Memory}` works instead — it is not shared across pods.
- **Configuration from the pod spec:** any scalar setting can be set with an environment variable (§9.1), e.g. `MXLGW_NODE_LABEL` from `spec.nodeName`. Such settings are read-only in the UI.
- **Metrics:** `kubectl apply -f deploy/k8s/servicemonitor.yaml` with the Prometheus Operator; the dashboard is `monitoring/grafana/mxl-st2110-gateway.json`.
