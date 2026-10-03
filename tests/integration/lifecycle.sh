#!/usr/bin/env bash
# Platform lifecycle (platform guideline G1-G12, SPECIFICATION.md §7.1, §10, §14.4): no hardware, no hugepages.
#
#   tests/integration/lifecycle.sh mxlgw:dev
#
# A gateway on the mock backend, configured only through the platform's standard environment variables,
# as uid 1000 without D-Bus/Avahi: start -> registered with a (mock) static registry -> ready; ids from
# NMOS_SEED; IP literals and NMOS_TAGS in the registry; only MXL resources in the MXL registry, the ST 2110
# node on its own port; a second instance on other ports; a taken port exits 75; an active MXL Receiver
# survives a restart; SIGTERM -> exit 143 within SHUTDOWN_TIMEOUT_S, every resource deleted from the
# registry and the own output domain removed (MXL_CLEANUP_ON_EXIT=true), a sibling domain untouched.
source "$(dirname "$0")/lib.sh"

IMAGE="${1:?usage: lifecycle.sh <image>}"
need docker curl python3 ip
WORK="$(mktemp -d /tmp/mxlgw-lifecycle.XXXXXX)"
on_exit "as_root rm -rf '$WORK'"
NMOS_PORT=18200
ST2110_PORT=$((NMOS_PORT + 1))
REG_PORT=18210
BASE="http://127.0.0.1:$NMOS_PORT"
REG="http://127.0.0.1:$REG_PORT"
SEED="prod1-gw"
GW="$IT_PREFIX-lifecycle"

# ---- image: uid 1000 and OCI labels (G11)
user=$(docker image inspect -f '{{.Config.User}}' "$IMAGE")
[[ "$user" == "1000:1000" ]] || fail "image user is '$user', expected 1000:1000"
labels=$(docker image inspect -f '{{json .Config.Labels}}' "$IMAGE")
python3 - "$labels" <<'PY' || fail "image labels: $labels"
import json, re, sys
l = json.loads(sys.argv[1])
assert re.fullmatch(r"[0-9a-f]{40}", l.get("io.dmf.mxl.revision", "")), "io.dmf.mxl.revision"
assert l.get("org.opencontainers.image.source", "").startswith("https://github.com/"), "source"
assert l.get("org.opencontainers.image.licenses") == "MIT", "licenses"
assert "org.opencontainers.image.revision" in l, "revision"
PY
pass "image runs as 1000:1000 with the OCI and io.dmf.mxl.revision labels"

# ---- mock registry
python3 "$(dirname "$0")/mock_registry.py" --port "$REG_PORT" &
REG_PID=$!
on_exit "kill $REG_PID"
wait_until 10 "mock registry" http_ok "$REG/x-nmos/registration/v1.3"

# ---- MXL root with another function's domain next to ours
make_mxl_root "$WORK/mxl" 256m
mkdir -p "$WORK/mxl/other-function"
echo '{"id": "b2b2b2b2-0000-4000-8000-00000000b002", "label": "other", "description": "", "tags": {}}' >"$WORK/mxl/other-function/domain_def.json"

config() { # groups only: identity, ports, registry and domain come from the environment
    cat <<'EOF'
{
  "schema_version": 1,
  "nic": {"backend": "mock", "port_pairs": [{"name": "media", "primary": {"name": "media-p", "ip": "10.1.1.21", "netmask": "255.255.255.0"}}]},
  "ptp": {"mode": "external", "require_lock": false},
  "groups": [
    {"label": "CAM 1", "direction": "ingest", "domain": "main",
     "video": [{"label": "CAM 1 V", "width": 1920, "height": 1080, "rate": "50/1", "defaults": {"legs": [{"multicast": "239.1.1.1", "port": 20000}]}}]},
    {"label": "PGM", "direction": "egress", "domain": "main",
     "video": [{"label": "PGM V", "width": 1920, "height": 1080, "rate": "50/1", "defaults": {"legs": [{"multicast": "239.10.0.1", "port": 20000}]}}]}
  ]
}
EOF
}
mkdir -p "$WORK/config" "$WORK/config2"
config >"$WORK/config/gateway.json"
config >"$WORK/config2/gateway.json"
chmod 0777 "$WORK/config" "$WORK/config2" # the image user (uid 1000) writes gateway.json and state/

TAGS='{"urn:x-platform:production":["prod1"],"urn:x-platform:function":["gw"]}'
run_gateway() { # <name> <config dir> <nmos port> <output domain> [extra docker args...]
    local name="$1" cfg="$2" port="$3" domain="$4"
    shift 4
    docker rm -f "$name" >/dev/null 2>&1 || true
    docker run -d --name "$name" --network host \
        -v "$cfg":/config -v "$WORK/mxl":/Volumes/mxl \
        -e NMOS_PORT="$port" \
        -e MXL_DOMAIN_SCAN_PATH=/Volumes/mxl \
        -e MXL_OUTPUT_DOMAIN_DIR="/Volumes/mxl/$domain" \
        -e SHUTDOWN_TIMEOUT_S=10 \
        "$@" "$IMAGE" >/dev/null
    IT_CONTAINERS+=("$name")
}
run_gateway "$GW" "$WORK/config" "$NMOS_PORT" "$SEED" \
    -e NMOS_SEED="$SEED" -e NMOS_LABEL="PROD1 GW" -e NMOS_TAGS="$TAGS" \
    -e NMOS_REGISTRY_ADDRESS=127.0.0.1 -e NMOS_REGISTRY_PORT="$REG_PORT" -e NMOS_DNS_SD=false \
    -e MXL_CLEANUP_ON_EXIT=true
IT_METRICS+=("lifecycle $BASE")

# ---- start -> registered -> ready (G4, G7)
wait_until 30 "/livez" http_ok "$BASE/livez"
wait_until 30 "/readyz 200 (registered with the static registry)" http_ok "$BASE/readyz"
pass "ready: registered with the static registry, no DNS-SD"

ids=$(python3 - "$SEED" <<'PY'
import sys, uuid
ns = uuid.uuid5(uuid.NAMESPACE_URL, "urn:x-mxl-st2110-gateway:seed:" + sys.argv[1])
print(uuid.uuid5(ns, "node"), uuid.uuid5(ns, "st2110-node"), uuid.uuid5(ns, "mxl-domain:main"))
PY
)
read -r NODE_ID ST2110_ID DOMAIN_ID <<<"$ids"
HOST_IP=$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')
log "expected node $NODE_ID, ST 2110 node $ST2110_ID, domain $DOMAIN_ID, host address $HOST_IP"

# ---- the registry sees the MXL node only, with seed ids, IP literals and the tags (G3, G4, G5)
python3 - "$REG" "$NODE_ID" "$HOST_IP" "$NMOS_PORT" <<'PY' || fail "registry content"
import json, sys, urllib.request
reg, node_id, host_ip, port = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
q = lambda t: json.load(urllib.request.urlopen(f"{reg}/x-nmos/query/v1.3/{t}"))
nodes = q("nodes")
assert [n["id"] for n in nodes] == [node_id], f"nodes {[n['id'] for n in nodes]}"
n = nodes[0]
assert n["label"] == "PROD1 GW", n["label"]
assert n["href"] == f"http://{host_ip}:{port}/", n["href"]
hosts = {e["host"] for e in n["api"]["endpoints"]}
assert hosts == {host_ip}, hosts
assert n["tags"]["urn:x-platform:production"] == ["prod1"], n["tags"]
devices = q("devices")
assert len(devices) == 1 and devices[0]["node_id"] == node_id and devices[0]["tags"]["urn:x-platform:function"] == ["gw"], devices
assert devices[0]["label"].startswith("PROD1 GW"), devices[0]["label"]
for kind in ("senders", "receivers"):
    items = q(kind)
    assert items and all(i["transport"] == "urn:x-nmos:transport:mxl" for i in items), f"{kind}: {[i['transport'] for i in items]}"
    for i in items:
        assert i["tags"]["urn:x-nmos:tag:grouphint/v1.0"][0].split(":")[1] == "Video 1", i["tags"]
PY
pass "MXL registry: node $NODE_ID (seed), IP-literal href and endpoints, NMOS_TAGS, MXL Senders/Receivers only"

# ---- the ST 2110 node on its own port, not registered (§7.1)
[[ "$(json "http://127.0.0.1:$ST2110_PORT/x-nmos/node/v1.3/self" 'j["id"]')" == "$ST2110_ID" ]] || fail "ST 2110 node id"
[[ "$(json "http://127.0.0.1:$ST2110_PORT/x-nmos/node/v1.3/self" 'j["href"]')" == "http://$HOST_IP:$ST2110_PORT/" ]] || fail "ST 2110 node href"
transports=$(json "http://127.0.0.1:$ST2110_PORT/x-nmos/node/v1.3/senders" 'sorted({s["transport"] for s in j})')
[[ "$transports" == '["urn:x-nmos:transport:rtp"]' ]] || fail "ST 2110 node senders: $transports"
[[ "$(json "$BASE/api/nmos" 'j["st2110"]["registered"]')" == "False" ]] || fail "ST 2110 node reports a registration"
pass "ST 2110 node $ST2110_ID on :$ST2110_PORT with the RTP Senders/Receivers, not in the MXL registry"

# ---- seed-derived output domain, metrics prefix, /api/v1, no D-Bus (G2, G3, G7, G10)
[[ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$WORK/mxl/$SEED/domain_def.json")" == "$DOMAIN_ID" ]] ||
    fail "output domain id is not the seed-derived one"
[[ "$(metric "$BASE" 'mxl_st2110_gateway_nmos_registered{node="mxl"}')" == "1" ]] || fail "mxl_st2110_gateway_nmos_registered{node=\"mxl\"}"
[[ "$(metric "$BASE" 'mxl_st2110_gateway_nmos_registered{node="st2110"}')" == "0" ]] || fail "mxl_st2110_gateway_nmos_registered{node=\"st2110\"}"
[[ "$(json "$BASE/api/v1/status" 'j["node"]["id"]')" == "$NODE_ID" ]] || fail "/api/v1/status"
curl -fsS "$BASE/api/v1/config/export" | python3 -c 'import json,sys; j=json.load(sys.stdin); assert "seed" not in j["node"] if "node" in j else True' ||
    fail "environment values leaked into the exported file"
if docker logs "$GW" 2>&1 | grep -qi "DNSService\|avahi\|dbus"; then
    fail "DNS-SD/D-Bus activity although NMOS_DNS_SD=false"
fi
pass "seed-derived output domain $DOMAIN_ID, metric prefix, /api/v1, no D-Bus/Avahi"

# ---- two instances on one host; a taken port exits 75 (G6)
run_gateway "$GW-2" "$WORK/config2" 18202 "$SEED-2" -e NMOS_SEED="$SEED-2"
wait_until 30 "second instance /readyz" http_ok "http://127.0.0.1:18202/readyz"
docker rm -f "$GW-2" >/dev/null
IT_CONTAINERS+=("$GW-3")
set +e
timeout 60 docker run --rm --name "$GW-3" --network host -v "$WORK/config2":/config -v "$WORK/mxl":/Volumes/mxl -e NMOS_PORT="$NMOS_PORT" \
    -e MXL_OUTPUT_DOMAIN_DIR="/Volumes/mxl/$SEED-3" "$IMAGE" >"$WORK/port-taken.log" 2>&1
rc=$?
set -e
[[ $rc -eq 75 ]] || { cat "$WORK/port-taken.log" >&2; fail "a taken port exited with $rc, expected 75"; }
pass "second instance on :18202 ready; a taken port exits 75"

# ---- an active MXL Receiver survives a restart (G9 SHOULD)
FLOW=00000000-0000-4000-8000-0000000000f1
rid=$(nmos_id "$BASE" receivers "PGM V")
patch_staged "$BASE" receivers "$rid" "{\"master_enable\": true, \"activation\": {\"mode\": \"activate_immediate\"},
  \"transport_params\": [{\"mxl_domain_id\": \"$DOMAIN_ID\", \"mxl_flow_id\": \"$FLOW\"}]}"
docker restart -t 15 "$GW" >/dev/null
wait_until 30 "/readyz after the restart" http_ok "$BASE/readyz"
active=$(json "$BASE/x-nmos/connection/v1.2/single/receivers/$rid/active" '[j["master_enable"], j["transport_params"][0]["mxl_flow_id"]]')
[[ "$active" == "[true, \"$FLOW\"]" ]] || fail "MXL Receiver /active after the restart: $active"
sid=$(nmos_id "$BASE" senders "CAM 1 V")
[[ "$(json "$BASE/x-nmos/connection/v1.2/single/senders/$sid/active" 'j["transport_params"][0]["mxl_domain_id"]')" == "$DOMAIN_ID" ]] ||
    fail "MXL Sender does not report the active mxl_domain_id"
pass "MXL Receiver connection restored after a restart; MXL Sender reports mxl_domain_id"

# ---- SIGTERM: exit 143 within SHUTDOWN_TIMEOUT_S, deregistered, own domain removed (G8)
[[ -d "$WORK/mxl/$SEED" ]] || fail "output domain missing before the stop"
mark=$(python3 -c 'import time; print(time.time())')
start=$SECONDS
docker stop -t 15 "$GW" >/dev/null
elapsed=$((SECONDS - start))
rc=$(docker inspect -f '{{.State.ExitCode}}' "$GW")
[[ "$rc" == "143" ]] || fail "exit code $rc after SIGTERM, expected 143"
((elapsed <= 11)) || fail "shutdown took ${elapsed}s (SHUTDOWN_TIMEOUT_S=10)"
python3 - "$REG" "$mark" "$NODE_ID" <<'PY' || fail "registry after SIGTERM"
import json, sys, urllib.request
reg, mark, node_id = sys.argv[1], float(sys.argv[2]), sys.argv[3]
for t in ("nodes", "devices", "sources", "flows", "senders", "receivers"):
    left = json.load(urllib.request.urlopen(f"{reg}/x-nmos/query/v1.3/{t}"))
    assert not left, f"{t} still registered: {[r['id'] for r in left]}"
log = [r for r in json.load(urllib.request.urlopen(f"{reg}/_log")) if r["t"] >= mark and r["method"] == "DELETE"]
assert any(r.get("type") == "node" and r.get("id") == node_id for r in log), "no DELETE of the node"
assert log[-1].get("type") == "node", f"the node is not deleted last: {log[-1]}"
print(f"{len(log)} DELETEs", file=sys.stderr)
PY
[[ ! -e "$WORK/mxl/$SEED" ]] || fail "own output domain not removed (MXL_CLEANUP_ON_EXIT=true)"
[[ -s "$WORK/mxl/other-function/domain_def.json" ]] || fail "another function's domain was touched"
docker logs "$GW" 2>&1 | grep -q '"event":"nmos_deregistered"' || fail "no nmos_deregistered log event"
pass "SIGTERM: exit 143 in ${elapsed}s, every resource DELETEd (node last), own domain removed, sibling domain kept"
pass "lifecycle"
