#!/usr/bin/env bash
# Receiver activated before its flow exists (SPECIFICATION.md §17.3, §5.8, acceptance criterion 13).
#
#   tests/integration/late-flow.sh mxlgw:dev
#
# An egress gateway's MXL Receivers are PATCHed with the id of a simulated mxl-fabrics-agent mirror
# domain and flow ids that do not exist yet. They must wait (waiting_for_flow, flow-not-found
# counter growing), start on their own when a writer appears in the mirror domain, go to no_signal
# (not error) when the writer stops, resume when it is restarted, and never write into the mirror.
source "$(dirname "$0")/lib.sh"

IMAGE="${1:?usage: late-flow.sh <image>}"
need docker curl python3 ip nsenter sha256sum

WORK="$(mktemp -d /tmp/mxlgw-lateflow.XXXXXX)"
on_exit "as_root rm -rf '$WORK'"
LOCAL_ID=7e3a8c52-1d2f-4f0a-9b8e-5c6d7e8f9a80
MIRROR_ID=a1a1a1a1-0000-4000-8000-00000000a001
FLOW_V=66666666-6666-4666-8666-000000000001
FLOW_A=66666666-6666-4666-8666-000000000002
FLOW_ANC=66666666-6666-4666-8666-000000000003
GWE=http://127.0.0.1:18183
GWI=http://192.168.81.2:18184

ensure_hugepages 1024
make_mxl_root "$WORK/mxl" 2g
mkdir -p "$WORK/egress" "$WORK/ingest"
make_netns "$IT_PREFIX-lf-ns" "$IMAGE"
make_veth mxlit4 mxlit5 192.168.81.1/24 192.168.81.2/24 "$IT_PREFIX-lf-ns"

# The mirror domain as mxl-fabrics-agent would create it (§8.6): sibling of the local domains,
# identity and marker in domain_def.json, options.json copied from the origin.
MIRROR="$WORK/mxl/mirror-$MIRROR_ID"
mkdir -p "$MIRROR"
cat >"$MIRROR/domain_def.json" <<EOF
{"id": "$MIRROR_ID", "label": "Host A main (mirror)", "description": "simulated mirror",
 "x-mxl-fabrics-agent": {"mirror": true, "source_host_id": "host-a", "owner_host_id": "host-b"}}
EOF
echo '{"urn:x-mxl:option:history_duration/v1.0": 200000000}' >"$MIRROR/options.json"
chmod -R a+rwX "$MIRROR"
DEF_SUM=$(sha256sum "$MIRROR/domain_def.json" | cut -d' ' -f1)

config() { # <label> <port> <ifname> <ip> <lcores> <group> <direction> <multicast-base>
    cat <<EOF
{
  "schema_version": 1,
  "node": {"label": "$1", "http_port": $2, "registry": {"mode": "static", "address": "127.0.0.1", "port": 9}},
  "nic": {"backend": "kernel", "lcores": "$5", "port_pairs": [{"name": "media",
          "primary": {"name": "media-p", "ifname": "$3", "ip": "$4", "netmask": "255.255.255.0"}}]},
  "ptp": {"mode": "external", "require_lock": false},
  "mxl": {"scan_path": "/Volumes/mxl",
          "domains": [{"name": "local", "path": "/Volumes/mxl/local-$1", "id": $( [[ $7 == egress ]] && echo "\"$LOCAL_ID\"" || echo null ), "history_duration_ns": 200000000}]},
  "groups": [{"label": "$6", "direction": "$7", "domain": "local",
      "video": [{"label": "$6 V", "width": 1920, "height": 1080, "rate": "25/1", "defaults": {"legs": [{"multicast": "$8.1", "port": 20000}]}}],
      "audio": [{"label": "$6 A", "channels": 2, "defaults": {"legs": [{"multicast": "$8.2", "port": 20002}]}}],
      "anc":   [{"label": "$6 ANC", "defaults": {"legs": [{"multicast": "$8.3", "port": 20004}]}}]}]
}
EOF
}
config IT-LF-EGRESS 18183 mxlit4 192.168.81.1 1 PGM egress 239.81.0 >"$WORK/egress/gateway.json"
config IT-LF-INGEST 18184 mxlit5 192.168.81.2 2 LOOP ingest 239.81.0 >"$WORK/ingest/gateway.json"

start_gateway "$IT_PREFIX-lf-egress" "$IMAGE" "$WORK/egress" "$WORK/mxl" host
start_gateway "$IT_PREFIX-lf-ingest" "$IMAGE" "$WORK/ingest" "$WORK/mxl" "container:$IT_PREFIX-lf-ns"
wait_until 90 "egress /livez" http_ok "$GWE/livez"
wait_until 90 "ingest /livez" http_ok "$GWI/livez"

log "activating the MXL Receivers on the mirror domain before any flow exists"
declare -A FLOW=(["V"]=$FLOW_V ["A"]=$FLOW_A ["ANC"]=$FLOW_ANC)
for e in V A ANC; do
    rid=$(nmos_id "$GWE" receivers "PGM $e")
    patch_staged "$GWE" receivers "$rid" "{\"master_enable\": true, \"activation\": {\"mode\": \"activate_immediate\"},
        \"transport_params\": [{\"mxl_domain_id\": \"$MIRROR_ID\", \"mxl_flow_id\": \"${FLOW[$e]}\"}]}"
    sid=$(nmos_id "$GWE" senders "PGM $e")
    patch_staged "$GWE" senders "$sid" '{"master_enable": true, "activation": {"mode": "activate_immediate"}}'
    sdp=$(curl -fsS "$GWE/x-nmos/connection/v1.2/single/senders/$sid/transportfile")
    body=$(python3 -c 'import json,sys; print(json.dumps({"sender_id": sys.argv[1], "master_enable": True,
        "activation": {"mode": "activate_immediate"}, "transport_file": {"data": sys.argv[2], "type": "application/sdp"}}))' "$sid" "$sdp")
    patch_staged "$GWI" receivers "$(nmos_id "$GWI" receivers "LOOP $e")" "$body"
    patch_staged "$GWI" senders "$(nmos_id "$GWI" senders "LOOP $e")" '{"master_enable": true, "activation": {"mode": "activate_immediate"}}'
done
for e in V A ANC; do
    wait_until 10 "PGM $e waiting_for_flow" essence_state_is "$GWE" "PGM $e" waiting_for_flow
done
nf1=$(metric "$GWE" 'mxlgw_mxl_flow_not_found_total{essence="PGM V"}')
sleep 10
nf2=$(metric "$GWE" 'mxlgw_mxl_flow_not_found_total{essence="PGM V"}')
python3 -c "import sys; sys.exit(0 if float(sys.argv[2]) > float(sys.argv[1]) else 1)" "${nf1:-0}" "${nf2:-0}" ||
    fail "mxlgw_mxl_flow_not_found_total did not grow ($nf1 -> $nf2)"
essence_state_is "$GWE" "PGM V" waiting_for_flow || fail "PGM V left waiting_for_flow without a flow"
pass "waiting for the flow (flow-not-found $nf1 -> $nf2)"
ready_before=$(json "$GWE/readyz" 'sorted(j["reasons"])')

writer() {
    start_tool "$IT_PREFIX-lf-pattern" "$IMAGE" "$WORK/mxl" mxl-pattern-writer --domain "/Volumes/mxl/mirror-$MIRROR_ID" \
        --video-flow "$FLOW_V" --audio-flow "$FLOW_A" --anc-flow "$FLOW_ANC" --width 1920 --height 1080 --rate 25/1 --channels 2 --label MIRROR
}

log "starting the writer in the mirror domain"
writer
for e in V A ANC; do
    wait_until 20 "PGM $e running" essence_state_is "$GWE" "PGM $e" running
done
[[ "$(metric "$GWE" 'mxlgw_mxl_reader_info{essence="PGM V",domain_kind="mirror"}')" == "1" ]] || fail "mxlgw_mxl_reader_info does not show the mirror domain"
for e in V A ANC; do
    wait_until 30 "LOOP $e running" essence_state_is "$GWI" "LOOP $e" running
done
mkdir -p "$IT_ARTIFACTS"
run_tool "$IMAGE" "$WORK/mxl" mxl-verify --domain "/Volumes/mxl/local-IT-LF-INGEST" \
    --video-flow "$(essence_field "$GWI" "LOOP V" flow_id)" --audio-flow "$(essence_field "$GWI" "LOOP A" flow_id)" \
    --anc-flow "$(essence_field "$GWI" "LOOP ANC" flow_id)" --width 1920 --height 1080 --rate 25/1 --channels 2 --duration-ms 5000 \
    >"$IT_ARTIFACTS/late-flow-verify.json" || { cat "$IT_ARTIFACTS/late-flow-verify.json" >&2; fail "ST 2110 output of the late flow"; }
pass "receivers started on their own; ST 2110 output verified by the ingest side"

log "stopping the writer"
docker stop -t 5 "$IT_PREFIX-lf-pattern" >/dev/null
docker rm -f "$IT_PREFIX-lf-pattern" >/dev/null
for e in V A ANC; do
    wait_until 10 "PGM $e no_signal" essence_state_is "$GWE" "PGM $e" no_signal
done
sleep 3
for e in V A ANC; do
    st=$(essence_field "$GWE" "PGM $e" state)
    [[ "$st" == "no_signal" || "$st" == "waiting_for_flow" ]] || fail "PGM $e is $st after the writer stopped"
done
[[ "$(json "$GWE/readyz" 'sorted(j["reasons"])')" == "$ready_before" ]] || fail "/readyz changed when the flow went away"
pass "writer stopped: no_signal, readiness unaffected"

log "restarting the writer (flows re-created)"
writer
for e in V A ANC; do
    wait_until 20 "PGM $e running again" essence_state_is "$GWE" "PGM $e" running
done
pass "reading resumed"

docker rm -f "$IT_PREFIX-lf-pattern" >/dev/null
[[ "$(sha256sum "$MIRROR/domain_def.json" | cut -d' ' -f1)" == "$DEF_SUM" ]] || fail "the gateway modified the mirror domain_def.json"
unexpected=$(find "$MIRROR" -mindepth 1 -maxdepth 1 ! -name domain_def.json ! -name options.json \
    ! -name "$FLOW_V.mxl-flow" ! -name "$FLOW_A.mxl-flow" ! -name "$FLOW_ANC.mxl-flow" -print)
[[ -z "$unexpected" ]] || fail "unexpected entries in the mirror domain: $unexpected"
pass "nothing written into the mirror domain"
pass "late-flow"
