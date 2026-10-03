#!/usr/bin/env bash
# Container smoke test (SPECIFICATION.md §9.1, §10, §14): no hardware, no hugepages.
#
#   tests/integration/smoke.sh mxlgw:dev
#
# --version, setup mode without a config file, exit 78 paths, a configured gateway on the mock
# backend with a real MXL tmpfs domain (both NMOS nodes, admin UI, /metrics lint), graceful stop.
# Runs as the image user (uid 1000); the configuration directories are world-writable for it.
source "$(dirname "$0")/lib.sh"

IMAGE="${1:?usage: smoke.sh <image>}"
need docker curl python3
WORK="$(mktemp -d /tmp/mxlgw-smoke.XXXXXX)"
on_exit "as_root rm -rf '$WORK'"
BASE=http://127.0.0.1:18180
ST2110=http://127.0.0.1:18181

# ---- --version
version=$(docker run --rm "$IMAGE" --version)
log "$version"
grep -q "mxl-st2110-gateway" <<<"$version" || fail "--version output"
if grep -qE '^(MTL|DPDK|MXL|nmos-cpp) unknown' <<<"$version"; then
    fail "--version reports unknown pins: $version"
fi
pass "--version"

# ---- setup mode: no configuration file
mkdir -p "$WORK/setup"
chmod 0777 "$WORK/setup"
docker run -d --name "$IT_PREFIX-setup" --network host -e MXLGW_HTTP_PORT=18180 -v "$WORK/setup":/config "$IMAGE" >/dev/null
IT_CONTAINERS+=("$IT_PREFIX-setup")
wait_until 30 "setup-mode /livez" http_ok "$BASE/livez"
[[ "$(json "$BASE/api/status" 'j["setup_mode"]')" == "True" ]] || fail "setup_mode not reported"
code=$(curl -s -o "$WORK/readyz.json" -w '%{http_code}' "$BASE/readyz")
if [[ "$code" != "503" ]] || ! grep -q unconfigured "$WORK/readyz.json"; then
    fail "/readyz in setup mode: $code $(cat "$WORK/readyz.json")"
fi
[[ "$(curl -fsS "$BASE/admin/")" == "<!DOCTYPE html>"* ]] || fail "admin UI"
[[ "$(curl -s -o /dev/null -w '%{http_code}' "$BASE/admin")" == "301" ]] || fail "/admin redirect"
[[ -s "$WORK/setup/gateway.json" ]] || fail "minimal configuration not written"
docker rm -f "$IT_PREFIX-setup" >/dev/null
pass "setup mode"

# ---- exit 78: unwritable/missing config directory, invalid file, non-tmpfs domain
set +e
docker run --rm -e MXLGW_CONFIG=/nonexistent/gateway.json "$IMAGE" >/dev/null 2>&1
rc=$?
set -e
[[ $rc -eq 78 ]] || fail "missing config directory exited with $rc, expected 78"
mkdir -p "$WORK/invalid"
chmod 0777 "$WORK/invalid"
echo '{"schema_version": 1, "nic": {"backend": "warp"}}' >"$WORK/invalid/gateway.json"
set +e
docker run --rm -v "$WORK/invalid":/config "$IMAGE" >"$WORK/invalid.log" 2>&1
rc=$?
set -e
[[ $rc -eq 78 ]] || fail "invalid configuration exited with $rc, expected 78"
grep -q "/nic/backend" "$WORK/invalid.log" || fail "invalid configuration error does not name /nic/backend"
set +e
docker run --rm -v "$WORK/setup":/config -e NMOS_PORT=18180 -e MXLGW_HTTP_PORT=18190 "$IMAGE" >"$WORK/conflict.log" 2>&1
rc=$?
set -e
[[ $rc -eq 78 ]] || fail "conflicting NMOS_PORT/MXLGW_HTTP_PORT exited with $rc, expected 78"
grep -q "NMOS_PORT" "$WORK/conflict.log" || fail "the alias conflict error does not name NMOS_PORT"

config() { # <domain root inside the container>
    cat <<EOF
{
  "schema_version": 1,
  "node": {"label": "IT-SMOKE", "http_port": 18180},
  "nic": {"backend": "mock", "port_pairs": [{"name": "media", "primary": {"name": "media-p", "ip": "10.1.1.21", "netmask": "255.255.255.0"}}]},
  "ptp": {"mode": "external", "require_lock": false},
  "mxl": {"scan_path": "/Volumes/mxl", "domains": [{"name": "main", "path": "$1/main", "history_duration_ns": 100000000}]},
  "groups": [
    {"label": "CAM 1", "direction": "ingest", "domain": "main",
     "video": [{"label": "CAM 1 V", "width": 1920, "height": 1080, "rate": "50/1", "defaults": {"legs": [{"multicast": "239.1.1.1", "port": 20000}]}}],
     "audio": [{"label": "CAM 1 A", "channels": 8, "defaults": {"legs": [{"multicast": "239.1.1.2", "port": 20000}]}}]},
    {"label": "PGM", "direction": "egress", "domain": "main",
     "video": [{"label": "PGM V", "width": 1920, "height": 1080, "rate": "50/1", "defaults": {"legs": [{"multicast": "239.10.0.1", "port": 20000}]}}]}
  ]
}
EOF
}
mkdir -p "$WORK/plain/mxl" "$WORK/plain/config"
chmod 0777 "$WORK/plain/mxl" "$WORK/plain/config"
config /Volumes/mxl >"$WORK/plain/config/gateway.json"
set +e
docker run --rm -v "$WORK/plain/config":/config -v "$WORK/plain/mxl":/Volumes/mxl "$IMAGE" >"$WORK/plain.log" 2>&1
rc=$?
set -e
if [[ "$(stat -f -c %T "$WORK/plain/mxl")" == "tmpfs" ]]; then
    log "skipping the non-tmpfs check: $WORK is on tmpfs"
else
    [[ $rc -eq 78 ]] || fail "non-tmpfs domain exited with $rc, expected 78"
    # refused by the preflight (check domain-<NAME>) before the bootstrap would log mxl_domain_not_tmpfs
    grep -qE '"check":"domain-MAIN"|"event":"mxl_domain_not_tmpfs"' "$WORK/plain.log" || fail "non-tmpfs domain: no domain check failure logged"
fi
pass "exit code 78 paths"

# ---- configured node on the mock backend
make_mxl_root "$WORK/mxl" 512m
mkdir -p "$WORK/config"
chmod 0777 "$WORK/config"
config /Volumes/mxl >"$WORK/config/gateway.json"
docker run -d --name "$IT_PREFIX-smoke" --network host -v "$WORK/config":/config -v "$WORK/mxl":/Volumes/mxl "$IMAGE" >/dev/null
IT_CONTAINERS+=("$IT_PREFIX-smoke")
wait_until 30 "/livez" http_ok "$BASE/livez"
[[ "$(json "$BASE/api/status" 'j["setup_mode"]')" == "False" ]] || fail "still in setup mode"
[[ -s "$WORK/mxl/main/domain_def.json" ]] || fail "domain_def.json not created"
domain_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["id"])' "$WORK/mxl/main/domain_def.json")
[[ "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["mxl"]["domains"][0]["id"])' "$WORK/config/gateway.json")" == "$domain_id" ]] ||
    fail "domain id not written back to the configuration"
# MXL node: MXL Senders (ingest) and MXL Receivers (egress); ST 2110 node: RTP Receivers (ingest) and RTP Senders (egress).
[[ "$(json "$BASE/x-nmos/node/v1.3/self" 'j["label"]')" == "IT-SMOKE" ]] || fail "IS-04 Node API"
[[ "$(json "$BASE/x-nmos/node/v1.3/senders" 'len(j)')" == "2" ]] || fail "expected 2 MXL senders"
[[ "$(json "$BASE/x-nmos/node/v1.3/receivers" 'len(j)')" == "1" ]] || fail "expected 1 MXL receiver"
[[ "$(json "$ST2110/x-nmos/node/v1.3/self" 'j["label"]')" == "IT-SMOKE ST 2110" ]] || fail "ST 2110 node API"
[[ "$(json "$ST2110/x-nmos/node/v1.3/receivers" 'len(j)')" == "2" ]] || fail "expected 2 RTP receivers"
[[ "$(json "$ST2110/x-nmos/node/v1.3/senders" 'len(j)')" == "1" ]] || fail "expected 1 RTP sender"
json "$BASE/x-nmos/connection/v1.2/single/receivers/" 'len(j)' >/dev/null || fail "IS-05 Connection API v1.2"
json "$ST2110/x-nmos/connection/v1.1/single/senders/" 'len(j)' >/dev/null || fail "IS-05 Connection API v1.1"
[[ "$(json "$BASE/x-nmos/node/v1.3/self" 'j["href"].split("/")[2].split(":")[0]')" =~ ^[0-9]+(\.[0-9]+){3}$ ]] || fail "href is not an IPv4 literal"
[[ "$(curl -fsS "$BASE/admin/")" == "<!DOCTYPE html>"* ]] || fail "admin UI"
[[ "$(json "$BASE/api/domains" 'j["configured"][0]["id"]')" == "$domain_id" ]] || fail "/api/domains"
[[ "$(json "$BASE/api/v1/domains" 'j["configured"][0]["id"]')" == "$domain_id" ]] || fail "/api/v1/domains"
wait_until 15 "/readyz 200 (no registry configured)" http_ok "$BASE/readyz"
"$(dirname "$0")/check-metrics.sh" "$BASE/metrics"
pass "configured gateway: MXL node :18180, ST 2110 node :18181"

# ---- graceful stop
start=$SECONDS
docker stop -t 15 "$IT_PREFIX-smoke" >/dev/null
rc=$(docker inspect -f '{{.State.ExitCode}}' "$IT_PREFIX-smoke")
((SECONDS - start <= 11)) || fail "shutdown took $((SECONDS - start)) s"
[[ "$rc" == "143" ]] || fail "exit code $rc after SIGTERM, expected 143"
[[ -s "$WORK/mxl/main/domain_def.json" ]] || fail "the domain was removed without MXL_CLEANUP_ON_EXIT"
pass "graceful stop (exit 143 in $((SECONDS - start)) s)"
pass "smoke"
