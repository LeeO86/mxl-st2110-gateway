#!/usr/bin/env bash
# Kernel-backend media loopback (SPECIFICATION.md §17.3, Phase 3/4 acceptance).
#
#   tests/integration/loopback.sh mxlgw:dev
#
# pattern writer -> MXL "main" -> egress gateway (group PGM, ST 2022-7 on two veth legs)
#   -> ST 2110 -> ingest gateway in its own network namespace (group LOOP) -> new MXL flows -> mxl-verify
#
# Checks: v210 colour bars with a continuous frame counter, tone frequency/level per channel,
# timecode ANC continuity, A/V alignment within one audio block, RTP timestamp = grain time + the
# 2-grain output delay, a second 8-channel audio essence, the IS-05 path (SDP from the egress Sender
# into the ingest Receiver), and leg-R loss that leaves the output intact while
# mxl_st2110_gateway_rx_leg_seq_lost_total{leg="r"} grows. Needs root (sudo), docker and hugepages.
source "$(dirname "$0")/lib.sh"

IMAGE="${1:?usage: loopback.sh <image>}"
DURATION_MS="${LOOPBACK_DURATION_MS:-60000}"
need docker curl python3 ip nsenter nft

WORK="$(mktemp -d /tmp/mxlgw-loopback.XXXXXX)"
on_exit "as_root rm -rf '$WORK'"
DOMAIN_ID=7e3a8c52-1d2f-4f0a-9b8e-5c6d7e8f9a79
SRC_V=55555555-5555-4555-8555-000000000001
SRC_A1=55555555-5555-4555-8555-000000000002
SRC_ANC=55555555-5555-4555-8555-000000000003
SRC_A2=55555555-5555-4555-8555-000000000004
# MXL node / ST 2110 node of each gateway (§7.1)
GWE=http://127.0.0.1:18181
GWE2110=http://127.0.0.1:18191
GWI=http://192.168.79.2:18182
GWI2110=http://192.168.79.2:18192

ensure_hugepages 1024
make_mxl_root "$WORK/mxl" 3g
mkdir -p "$WORK/egress" "$WORK/ingest"

make_netns "$IT_PREFIX-ns" "$IMAGE"
make_veth mxlit0 mxlit1 192.168.79.1/24 192.168.79.2/24 "$IT_PREFIX-ns"
make_veth mxlit2 mxlit3 192.168.80.1/24 192.168.80.2/24 "$IT_PREFIX-ns"

essences() { # <prefix> <multicast third octet base>
    cat <<EOF
      "video": [{"label": "$1 V", "width": 1920, "height": 1080, "rate": "25/1",
                 "defaults": {"legs": [{"multicast": "239.79.0.1", "port": 20000}, {"multicast": "239.80.0.1", "port": 20000}]}}],
      "audio": [{"label": "$1 A1", "channels": 8, "bit_depth": 24, "ptime_us": 1000, "block_us": 1000,
                 "defaults": {"legs": [{"multicast": "239.79.0.2", "port": 20002}, {"multicast": "239.80.0.2", "port": 20002}]}},
                {"label": "$1 A2", "channels": 8, "bit_depth": 24, "ptime_us": 1000, "block_us": 1000,
                 "defaults": {"legs": [{"multicast": "239.79.0.3", "port": 20004}, {"multicast": "239.80.0.3", "port": 20004}]}}],
      "anc":   [{"label": "$1 ANC",
                 "defaults": {"legs": [{"multicast": "239.79.0.4", "port": 20006}, {"multicast": "239.80.0.4", "port": 20006}]}}]
EOF
}

gateway_config() { # <label> <port> <if-p> <ip-p> <if-r> <ip-r> <lcores> <group> <direction>
    cat <<EOF
{
  "schema_version": 1,
  "node": {"label": "$1", "http_port": $2, "st2110": {"http_port": $(($2 + 10))}},
  "nic": {"backend": "kernel", "lcores": "$7", "port_pairs": [{"name": "media",
          "primary":   {"name": "media-p", "ifname": "$3", "ip": "$4", "netmask": "255.255.255.0"},
          "redundant": {"name": "media-r", "ifname": "$5", "ip": "$6", "netmask": "255.255.255.0"}}]},
  "ptp": {"mode": "external", "require_lock": false},
  "mxl": {"scan_path": "/Volumes/mxl",
          "domains": [{"name": "main", "path": "/Volumes/mxl/main", "id": "$DOMAIN_ID", "history_duration_ns": 200000000}]},
  "groups": [{"label": "$8", "direction": "$9", "domain": "main", "redundancy": true,
$(essences "$8")
  }]
}
EOF
}
gateway_config IT-EGRESS 18181 mxlit0 192.168.79.1 mxlit2 192.168.80.1 1 PGM egress >"$WORK/egress/gateway.json"
gateway_config IT-INGEST 18182 mxlit1 192.168.79.2 mxlit3 192.168.80.2 2 LOOP ingest >"$WORK/ingest/gateway.json"

log "starting the gateways (they bootstrap the domain) and the pattern writers"
start_gateway "$IT_PREFIX-egress" "$IMAGE" "$WORK/egress" "$WORK/mxl" host
start_gateway "$IT_PREFIX-ingest" "$IMAGE" "$WORK/ingest" "$WORK/mxl" "container:$IT_PREFIX-ns"
IT_METRICS+=("egress $GWE" "ingest $GWI")
wait_until 90 "egress /livez" http_ok "$GWE/livez"
wait_until 90 "ingest /livez" http_ok "$GWI/livez"
start_tool "$IT_PREFIX-pattern" "$IMAGE" "$WORK/mxl" mxl-pattern-writer --domain /Volumes/mxl/main \
    --video-flow "$SRC_V" --audio-flow "$SRC_A1" --anc-flow "$SRC_ANC" --width 1920 --height 1080 --rate 25/1 --channels 8 --label PATTERN
start_tool "$IT_PREFIX-pattern2" "$IMAGE" "$WORK/mxl" mxl-pattern-writer --domain /Volumes/mxl/main \
    --audio-flow "$SRC_A2" --rate 25/1 --channels 8 --tone-hz 440 --label PATTERN2

log "connecting the egress MXL Receivers and enabling the RTP Senders"
declare -A SRC=(["V"]=$SRC_V ["A1"]=$SRC_A1 ["A2"]=$SRC_A2 ["ANC"]=$SRC_ANC)
for e in V A1 A2 ANC; do
    rid=$(nmos_id "$GWE" receivers "PGM $e")
    patch_staged "$GWE" receivers "$rid" "{\"master_enable\": true, \"activation\": {\"mode\": \"activate_immediate\"},
        \"transport_params\": [{\"mxl_domain_id\": \"$DOMAIN_ID\", \"mxl_flow_id\": \"${SRC[$e]}\"}]}"
    sid=$(nmos_id "$GWE2110" senders "PGM $e")
    patch_staged "$GWE2110" senders "$sid" '{"master_enable": true, "activation": {"mode": "activate_immediate"}}'
done
for e in V A1 A2 ANC; do
    wait_until 30 "egress PGM $e running" essence_state_is "$GWE" "PGM $e" running
done

log "connecting the ingest Receivers with the egress Senders' SDP files"
for e in V A1 A2 ANC; do
    sid=$(nmos_id "$GWE2110" senders "PGM $e")
    sdp=$(curl -fsS "$GWE2110/x-nmos/connection/v1.2/single/senders/$sid/transportfile")
    grep -q "a=group:DUP" <<<"$sdp" || fail "egress SDP of PGM $e has no DUP group"
    rid=$(nmos_id "$GWI2110" receivers "LOOP $e")
    body=$(python3 -c 'import json,sys; print(json.dumps({"sender_id": sys.argv[1], "master_enable": True,
        "activation": {"mode": "activate_immediate"}, "transport_file": {"data": sys.argv[2], "type": "application/sdp"}}))' "$sid" "$sdp")
    patch_staged "$GWI2110" receivers "$rid" "$body"
    msid=$(nmos_id "$GWI" senders "LOOP $e")
    patch_staged "$GWI" senders "$msid" '{"master_enable": true, "activation": {"mode": "activate_immediate"}}'
done
for e in V A1 A2 ANC; do
    wait_until 30 "ingest LOOP $e running" essence_state_is "$GWI" "LOOP $e" running
done

LOOP_V=$(essence_field "$GWI" "LOOP V" flow_id)
LOOP_A1=$(essence_field "$GWI" "LOOP A1" flow_id)
LOOP_A2=$(essence_field "$GWI" "LOOP A2" flow_id)
LOOP_ANC=$(essence_field "$GWI" "LOOP ANC" flow_id)
mkdir -p "$IT_ARTIFACTS"

run_tool "$IMAGE" "$WORK/mxl" mxl-info -d /Volumes/mxl/main -l >"$IT_ARTIFACTS/mxl-info.txt" || fail "mxl-info failed"
for f in "$LOOP_V" "$LOOP_A1" "$LOOP_ANC"; do
    grep -q "$f" "$IT_ARTIFACTS/mxl-info.txt" || fail "mxl-info does not list ingest flow $f"
done

# Losses the gateways count themselves on the test-only kernel backend (lib.sh verify_media).
ACCT_EGRESS="$GWE"
ACCT_INGEST="$GWI"
ACCT_INGEST_CONTAINER="$IT_PREFIX-ingest"
ACCT_AUDIO_TX=('mxl_st2110_gateway_tx_late_frames_total{essence="PGM A1"}' 'mxl_st2110_gateway_tx_late_frames_total{essence="PGM A2"}')
ACCT_VIDEO_TX=('mxl_st2110_gateway_tx_late_frames_total{essence="PGM V"}')
ACCT_VIDEO_RX=('mxl_st2110_gateway_rx_frames_total{essence="LOOP V",result="incomplete"}' 'mxl_st2110_gateway_rx_frames_total{essence="LOOP V",result="dropped"}')
verify() { # <report.json> <mxl-verify arguments...>
    local report="$1"
    shift
    verify_media "$IMAGE" "$WORK/mxl" "$report" --domain /Volumes/mxl/main "$@"
}

log "verifying the ingest flows for ${DURATION_MS} ms"
verify "$IT_ARTIFACTS/loopback-verify.json" --video-flow "$LOOP_V" --audio-flow "$LOOP_A1" --anc-flow "$LOOP_ANC" \
    --width 1920 --height 1080 --rate 25/1 --channels 8 --duration-ms "$DURATION_MS" --expect-offset-grains 2 ||
    { cat "$IT_ARTIFACTS/loopback-verify.json" >&2; fail "mxl-verify (video, audio 1, ANC)"; }
verify "$IT_ARTIFACTS/loopback-verify-a2.json" --audio-flow "$LOOP_A2" --rate 25/1 --channels 8 --tone-hz 440 --duration-ms 5000 ||
    { cat "$IT_ARTIFACTS/loopback-verify-a2.json" >&2; fail "mxl-verify (audio 2)"; }
pass "loopback media verified"

log "dropping 5 % of leg R"
lost_before=$(metric "$GWI" 'mxl_st2110_gateway_rx_leg_seq_lost_total{essence="LOOP V",leg="r"}')
# MTL's kernel backend receives with UDP sockets, so a netfilter rule in the ingest namespace drops
# packets before they reach it (nft numgen: xt_statistic is not available on every kernel).
in_netns "$IT_PREFIX-ns" nft add table inet mxlit
in_netns "$IT_PREFIX-ns" nft add chain inet mxlit input '{ type filter hook input priority 0 ; }'
in_netns "$IT_PREFIX-ns" nft add rule inet mxlit input iifname mxlit3 meta l4proto udp numgen random mod 20 0 drop
sleep 3
verify "$IT_ARTIFACTS/loopback-verify-leg-loss.json" --video-flow "$LOOP_V" --audio-flow "$LOOP_A1" --anc-flow "$LOOP_ANC" \
    --width 1920 --height 1080 --rate 25/1 --channels 8 --duration-ms 10000 --expect-offset-grains 2 ||
    { cat "$IT_ARTIFACTS/loopback-verify-leg-loss.json" >&2; fail "output not intact with leg R loss"; }
lost_after=$(metric "$GWI" 'mxl_st2110_gateway_rx_leg_seq_lost_total{essence="LOOP V",leg="r"}')
python3 -c "import sys; sys.exit(0 if float(sys.argv[2]) > float(sys.argv[1]) + 100 else 1)" "${lost_before:-0}" "${lost_after:-0}" ||
    fail "mxl_st2110_gateway_rx_leg_seq_lost_total{leg=\"r\"} did not grow ($lost_before -> $lost_after)"
in_netns "$IT_PREFIX-ns" nft delete table inet mxlit
pass "leg R loss: output intact, leg r lost $lost_before -> $lost_after"

"$(dirname "$0")/check-metrics.sh" "$GWE/metrics"
"$(dirname "$0")/check-metrics.sh" "$GWI/metrics"
pass "loopback"
