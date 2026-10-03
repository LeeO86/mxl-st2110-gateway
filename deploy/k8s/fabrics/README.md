# Two nodes with mxl-fabrics-agent (optional)

An ingest flow of the gateway on **node A** is received on **node B** through a mirror domain that [mxl-fabrics-agent](https://github.com/LeeO86/mxl-fabrics-agent) creates and fills (SPECIFICATION.md §8.6, §15.2). This scenario does not gate v1.0.

| Object | Node | Role |
|---|---|---|
| `Deployment/mxl-st2110-gateway-a` | `node-a` | ingest group "CAM 1" (ST 2110 → MXL Senders) in `/Volumes/mxl/main`, domain id `a1a1a1a1-0000-4000-8000-00000000a001` |
| `Deployment/mxl-st2110-gateway-b` | `node-b` | egress group "CAM 1 FROM A" (MXL Receivers → ST 2110), reads 2 grains behind the writer |
| mxl-fabrics-agent DaemonSet | both | from the agent's repository (`deploy/`), `hostNetwork`, `hostPath` of the same `/Volumes/mxl` |

The kustomization is self-contained: kustomize does not allow an overlay inside its base directory, so both gateways are defined here (`docs/decisions.md`). Both listen on 8090 so mxl-decklink can keep 8080 on the same node (§15.4).

## Prerequisites

- Each node is prepared as in [../README.md](../README.md) (vfio, hugepages, MXL root tmpfs, SR-IOV device plugin with `../sriov-dp-configmap.yaml`), and **both** have a TAI-disciplined `CLOCK_TAI` — grain indices are TAI-based and replicated 1:1 (§5.1).
- RDMA-capable links between the nodes for the agent (rdma-core, irdma with RoCEv2; see the agent's README).
- One NMOS registry reachable from both nodes.
- Replace `node-a` / `node-b` with the real node names and adjust the IP addresses in the seed configurations.

## Steps

```bash
kubectl apply -f deploy/k8s/sriov-dp-configmap.yaml      # once per cluster, adjusted per node
kubectl apply -k deploy/k8s/fabrics
kubectl apply -k <mxl-fabrics-agent>/deploy               # agent DaemonSet
```

1. On node A, connect a 2110 source to the ingest receivers of "CAM 1" and enable its MXL Senders (controller or IS-05 PATCH).
2. PATCH node B's MXL Receiver with node A's ids:

   ```bash
   curl -X PATCH -H 'Content-Type: application/json' \
     http://<node-b>:8090/x-nmos/connection/v1.2/single/receivers/<receiver-id>/staged -d '{
       "master_enable": true, "activation": {"mode": "activate_immediate"},
       "transport_params": [{"mxl_domain_id": "a1a1a1a1-0000-4000-8000-00000000a001",
                             "mxl_flow_id": "<flow id of node A MXL Sender>"}]}'
   ```

3. The agent on node B creates `/Volumes/mxl/mirror-a1a1a1a1-0000-4000-8000-00000000a001` (eager mode: already has). The receiver resolves it by id (§8.5), shows `waiting_for_flow` / `no_signal` until grains arrive (§5.8) and then starts on its own.
4. Check `mxl_st2110_gateway_mxl_reader_info{domain_kind="mirror"}` and `mxl_st2110_gateway_mxl_read_lag_grains` on node B (Grafana row *Egress*).
