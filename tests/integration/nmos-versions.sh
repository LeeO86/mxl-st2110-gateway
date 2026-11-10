#!/usr/bin/env bash
# Configurable NMOS API versions of the ST 2110 node (node.is04_versions / node.is05_versions): no hardware.
#
#   tests/integration/nmos-versions.sh mxlgw:dev
#
# A gateway on the mock backend with MXLGW_NODE_IS04_VERSIONS=v1.2 and MXLGW_NODE_IS05_VERSIONS=v1.0,v1.1
# (a controller that speaks only IS-05 v1.0): the ST 2110 node offers exactly those versions and the
# matching sr-ctrl device controls, registers with a static registry at IS-04 v1.2, and an IS-05 v1.0 PATCH
# with an SDP (audio, payload type 98 via a=rtpmap) activates and is restored after a restart. The MXL node
# keeps IS-04 v1.3 / IS-05 v1.1, v1.2. An unknown or empty version list exits 78 at start.
source "$(dirname "$0")/lib.sh"

IMAGE="${1:?usage: nmos-versions.sh <image>}"
need docker curl python3
WORK="$(mktemp -d /tmp/mxlgw-nmos-versions.XXXXXX)"
on_exit "as_root rm -rf '$WORK'"
NMOS_PORT=18300
ST2110_PORT=$((NMOS_PORT + 1))
REG_PORT=18310
BASE="http://127.0.0.1:$NMOS_PORT"
ST2110="http://127.0.0.1:$ST2110_PORT"
REG="http://127.0.0.1:$REG_PORT"
SEED="versions-gw"
GW="$IT_PREFIX-nmos-versions"

python3 "$(dirname "$0")/mock_registry.py" --port "$REG_PORT" &
REG_PID=$!
on_exit "kill $REG_PID"
wait_until 10 "mock registry" http_ok "$REG/x-nmos/registration/v1.2"

make_mxl_root "$WORK/mxl" 256m
mkdir -p "$WORK/config"
cat >"$WORK/config/gateway.json" <<'EOF'
{
  "schema_version": 1,
  "nic": {"backend": "mock", "port_pairs": [{"name": "media", "primary": {"name": "media-p", "ip": "10.1.1.21", "netmask": "255.255.255.0"}}]},
  "ptp": {"mode": "external", "require_lock": false},
  "groups": [
    {"label": "CAM 1", "direction": "ingest", "domain": "main",
     "video": [{"label": "CAM 1 V", "width": 1920, "height": 1080, "rate": "50/1", "defaults": {"legs": [{"multicast": "239.1.1.1", "port": 20000}]}}],
     "audio": [{"label": "CAM 1 A", "channels": 2, "defaults": {"legs": [{"multicast": "239.1.1.2", "port": 20000}]}}]},
    {"label": "PGM", "direction": "egress", "domain": "main",
     "video": [{"label": "PGM V", "width": 1920, "height": 1080, "rate": "50/1", "defaults": {"legs": [{"multicast": "239.10.0.1", "port": 20000}]}}]}
  ]
}
EOF
chmod 0777 "$WORK/config" # the image user (uid 1000) writes gateway.json and state/

gateway_args=(--network host -v "$WORK/config":/config -v "$WORK/mxl":/Volumes/mxl
    -e NMOS_PORT="$NMOS_PORT" -e NMOS_SEED="$SEED" -e NMOS_DNS_SD=false
    -e MXL_DOMAIN_SCAN_PATH=/Volumes/mxl -e MXL_OUTPUT_DOMAIN_DIR="/Volumes/mxl/$SEED" -e SHUTDOWN_TIMEOUT_S=10
    -e MXLGW_NODE_ST2110_REGISTRY_DNS_SD=false -e MXLGW_NODE_ST2110_REGISTRY_ADDRESS=127.0.0.1 -e MXLGW_NODE_ST2110_REGISTRY_PORT="$REG_PORT")

# ---- an unknown or empty version list is a configuration error: exit 78 before any port opens
for bad in "MXLGW_NODE_IS05_VERSIONS=v1.3" "MXLGW_NODE_IS05_VERSIONS=" "MXLGW_NODE_IS04_VERSIONS=v1.3,1.2"; do
    IT_CONTAINERS+=("$GW-bad")
    set +e
    timeout 60 docker run --rm --name "$GW-bad" "${gateway_args[@]}" -e "$bad" "$IMAGE" >"$WORK/bad.log" 2>&1
    rc=$?
    set -e
    [[ $rc -eq 78 ]] || { cat "$WORK/bad.log" >&2; fail "$bad exited with $rc, expected 78"; }
    grep -q "is0[45]_versions" "$WORK/bad.log" || { cat "$WORK/bad.log" >&2; fail "$bad: the error does not name the setting"; }
done
pass "unknown or empty version lists exit 78 and name the setting"

docker rm -f "$GW" >/dev/null 2>&1 || true
docker run -d --name "$GW" "${gateway_args[@]}" \
    -e MXLGW_NODE_IS04_VERSIONS=v1.2 -e MXLGW_NODE_IS05_VERSIONS="v1.0,v1.1" "$IMAGE" >/dev/null
IT_CONTAINERS+=("$GW")
IT_METRICS+=("nmos-versions $BASE")
wait_until 30 "/readyz" http_ok "$BASE/readyz"

# ---- offered versions and device controls
[[ "$(json "$ST2110/x-nmos/node/" 'j')" == '["v1.2/"]' ]] || fail "ST 2110 Node API versions: $(json "$ST2110/x-nmos/node/" 'j')"
[[ "$(json "$ST2110/x-nmos/connection/" 'j')" == '["v1.0/", "v1.1/"]' ]] || fail "ST 2110 Connection API versions: $(json "$ST2110/x-nmos/connection/" 'j')"
[[ "$(json "$BASE/x-nmos/node/" 'j')" == '["v1.3/"]' ]] || fail "MXL Node API versions changed"
[[ "$(json "$BASE/x-nmos/connection/" 'j')" == '["v1.1/", "v1.2/"]' ]] || fail "MXL Connection API versions changed"
controls=$(json "$ST2110/x-nmos/node/v1.2/devices" 'sorted({c["type"] for d in j for c in d["controls"] if "sr-ctrl" in c["type"]})')
[[ "$controls" == '["urn:x-nmos:control:sr-ctrl/v1.0", "urn:x-nmos:control:sr-ctrl/v1.1"]' ]] || fail "ST 2110 device controls: $controls"
href=$(json "$ST2110/x-nmos/node/v1.2/devices" '[c["href"] for d in j for c in d["controls"] if c["type"].endswith("/v1.0")][0]')
[[ "$href" == */x-nmos/connection/v1.0 ]] || [[ "$href" == */x-nmos/connection/v1.0/ ]] || fail "sr-ctrl/v1.0 href: $href"
mxl_controls=$(json "$BASE/x-nmos/node/v1.3/devices" 'sorted({c["type"] for d in j for c in d["controls"] if "sr-ctrl" in c["type"]})')
[[ "$mxl_controls" == '["urn:x-nmos:control:sr-ctrl/v1.1", "urn:x-nmos:control:sr-ctrl/v1.2"]' ]] || fail "MXL device controls: $mxl_controls"
[[ "$(json "$BASE/api/nmos" '[n["is05_versions"] for n in j["nodes"] if n["node"] == "st2110"][0]')" == '["v1.0", "v1.1"]' ]] ||
    fail "/api/nmos does not report the ST 2110 node's IS-05 versions"
pass "ST 2110 node: IS-04 v1.2, IS-05 v1.0/v1.1, controls sr-ctrl/v1.0 + v1.1; MXL node unchanged"

# ---- registration at the highest offered IS-04 version (v1.2), v1.3 resources downgraded
ST2110_ID=$(json "$ST2110/x-nmos/node/v1.2/self" 'j["id"]')
registered_v12() {
    python3 - "$REG" "$ST2110_ID" <<'PY'
import json, sys, urllib.request
reg, node_id = sys.argv[1], sys.argv[2]
log = json.load(urllib.request.urlopen(f"{reg}/_log"))
posts = [r for r in log if r["method"] == "POST" and r.get("type") in ("node", "device", "source", "flow", "sender", "receiver")]
assert posts and all(r["version"] == "v1.2" for r in posts), {r["version"] for r in posts}
types = {r["type"] for r in posts}
assert {"node", "device", "source", "flow", "sender", "receiver"} <= types, types
q = lambda t: json.load(urllib.request.urlopen(f"{reg}/x-nmos/query/v1.2/{t}"))
assert [n["id"] for n in q("nodes")] == [node_id]
v12 = {"id", "version", "label", "description", "flow_id", "transport", "tags", "device_id", "manifest_href", "caps", "interface_bindings", "subscription"}
senders = q("senders")
assert senders, "no Senders registered"
for s in senders:
    assert s["transport"] == "urn:x-nmos:transport:rtp", s["transport"]
    assert set(s) <= v12, f"Sender attributes beyond IS-04 v1.2: {set(s) - v12}"
PY
}
wait_until 20 "ST 2110 node registered at IS-04 v1.2" registered_v12
[[ "$(json "$BASE/api/nmos" '[n["registration_uri"] for n in j["nodes"] if n["node"] == "st2110"][0]')" == *"/x-nmos/registration/v1.2"* ]] ||
    fail "ST 2110 registration URI is not v1.2"
pass "ST 2110 node registered at IS-04 v1.2 (every resource type, Senders with v1.2 attributes only)"

# ---- IS-05 v1.0: transporttype is a v1.1 endpoint; a PATCH with an SDP (audio payload type 98) activates
rid=$(json "$ST2110/x-nmos/node/v1.2/receivers" 'next(r["id"] for r in j if r["label"] == "CAM 1 A")')
[[ "$(json "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/" 'sorted(j)')" == '["active/", "constraints/", "staged/"]' ]] ||
    fail "v1.0 receiver sub-resources: $(json "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/" 'sorted(j)')"
[[ "$(json "$ST2110/x-nmos/connection/v1.1/single/receivers/$rid/" '"transporttype/" in j')" == "True" ]] || fail "v1.1 has no transporttype/"
code=$(curl -sS -o /dev/null -w '%{http_code}' "$ST2110/x-nmos/connection/v1.2/single/receivers/")
[[ "$code" == "404" ]] || fail "IS-05 v1.2 is not offered but answered $code"
body=$(python3 - <<'PY'
import json
sdp = "\r\n".join(["v=0", "o=- 1443716955 1443716955 IN IP4 10.1.1.50", "s=CAM 1 A", "t=0 0",
    "m=audio 20000 RTP/AVP 98", "c=IN IP4 239.1.1.2/64", "a=source-filter: incl IN IP4 239.1.1.2 10.1.1.50",
    "a=rtpmap:98 L24/48000/2", "a=ptime:1", "a=ts-refclk:ptp=IEEE1588-2008:08-00-11-FF-FE-21-E1-B0:127", "a=mediaclk:direct=0", ""])
print(json.dumps({"master_enable": True, "activation": {"mode": "activate_immediate"},
                  "transport_file": {"type": "application/sdp", "data": sdp}}))
PY
)
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
code=$(curl -sS --max-time 10 -o "$WORK/patch.json" -w '%{http_code}' -X PATCH -H 'Content-Type: application/json' \
    "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/staged" -d "$body") || true
[[ "$code" == "200" ]] || { cat "$WORK/patch.json" >&2; fail "IS-05 v1.0 PATCH with an SDP returned $code"; }
rx_started_98() { # <since>: the audio receiver started with payload type 98
    docker logs --since "$1" "$GW" 2>&1 | python3 -c '
import json, sys
for line in sys.stdin:
    try:
        e = json.loads(line)
    except ValueError:
        continue
    if e.get("event") == "ingest_receiver_started" and e.get("essence") == "CAM 1 A" and e.get("payload_type") == 98:
        sys.exit(0)
sys.exit(1)'
}
wait_until 10 "CAM 1 A receiver started with payload type 98" rx_started_98 "$since"
active='[j["master_enable"], j["transport_params"][0]["multicast_ip"], "a=rtpmap:98 " in j["transport_file"]["data"]]'
[[ "$(json "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/active" "$active")" == '[true, "239.1.1.2", true]' ]] ||
    fail "v1.0 /active: $(json "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/active" "$active")"
pass "IS-05 v1.0 PATCH with an SDP activates (multicast from the SDP, payload type 98); no transporttype in v1.0"

# ---- the v1.0 activation is saved and restored after a restart, still offered over v1.0
saved_98() {
    python3 -c '
import json, sys
a = json.load(open(sys.argv[1]))["resources"][sys.argv[2]]["active"]
sys.exit(0 if a.get("master_enable") and "a=rtpmap:98 " in ((a.get("transport_file") or {}).get("data") or "") else 1)' \
        "$WORK/config/state/connections.json" "$rid"
}
wait_until 10 "CAM 1 A activation saved" saved_98
since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
docker restart -t 15 "$GW" >/dev/null
wait_until 30 "/readyz after the restart" http_ok "$BASE/readyz"
wait_until 15 "restored CAM 1 A receiver started with payload type 98" rx_started_98 "$since"
[[ "$(json "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/active" "$active")" == '[true, "239.1.1.2", true]' ]] ||
    fail "v1.0 /active after the restart: $(json "$ST2110/x-nmos/connection/v1.0/single/receivers/$rid/active" "$active")"
pass "the v1.0 activation is restored after a restart with payload type 98"
pass "nmos-versions"
