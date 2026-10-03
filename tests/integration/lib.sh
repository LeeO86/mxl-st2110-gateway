# shellcheck shell=bash
# Shared helpers for the integration tests (SPECIFICATION.md §17.3). Source from a test script:
#   source "$(dirname "$0")/lib.sh"
# The tests start gateway containers from the image under test with host networking (or inside a
# network-namespace container for the second end of a veth pair) and talk to their HTTP port.

set -euo pipefail

IT_PREFIX="${IT_PREFIX:-mxlgw-it}"
IT_CONTAINERS=()
IT_CLEANUP=()
IT_METRICS=() # "<name> <base-url>" pairs; their /metrics and /api/status are saved on failure
IT_FAILED=0
IT_ARTIFACTS="${IT_ARTIFACTS:-${PWD}/it-artifacts}"

log() { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
fail() {
    log "FAIL: $*"
    IT_FAILED=1
    exit 1
}
pass() { log "PASS: $*"; }

need() {
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 || fail "missing tool: $c"
    done
}

# sudo only when not root (GitHub runners have passwordless sudo)
as_root() {
    if [[ $(id -u) -eq 0 ]]; then "$@"; else sudo "$@"; fi
}

it_cleanup() {
    local rc=$?
    mkdir -p "$IT_ARTIFACTS"
    if [[ $rc -ne 0 || $IT_FAILED -ne 0 ]]; then
        local entry
        for entry in "${IT_METRICS[@]}"; do
            curl -fsS --max-time 3 "${entry#* }/metrics" >"$IT_ARTIFACTS/${entry%% *}.metrics.txt" 2>/dev/null || true
            curl -fsS --max-time 3 "${entry#* }/api/status" >"$IT_ARTIFACTS/${entry%% *}.status.json" 2>/dev/null || true
        done
    fi
    for c in "${IT_CONTAINERS[@]}"; do
        docker logs "$c" >"$IT_ARTIFACTS/$c.log" 2>&1 || true
        docker rm -f "$c" >/dev/null 2>&1 || true
    done
    local i
    for ((i = ${#IT_CLEANUP[@]} - 1; i >= 0; i--)); do
        eval "${IT_CLEANUP[$i]}" >/dev/null 2>&1 || true
    done
    if [[ $rc -ne 0 || $IT_FAILED -ne 0 ]]; then
        log "logs in $IT_ARTIFACTS"
    fi
    exit $rc
}
trap it_cleanup EXIT

on_exit() { IT_CLEANUP+=("$1"); }

# Hugepages for DPDK/MTL (kernel backend still needs them).
ensure_hugepages() {
    local want="${1:-512}"
    local have
    have=$(cat /proc/sys/vm/nr_hugepages)
    if ((have < want)); then
        as_root sysctl -q -w vm.nr_hugepages="$want"
        have=$(cat /proc/sys/vm/nr_hugepages)
    fi
    if ((have < want)); then
        # fragmented memory: free the page cache, compact and try again
        sync
        as_root sysctl -q -w vm.drop_caches=3
        as_root sysctl -q -w vm.compact_memory=1 || true
        as_root sysctl -q -w vm.nr_hugepages="$want"
        have=$(cat /proc/sys/vm/nr_hugepages)
        ((have >= want)) || fail "only $have of $want hugepages available"
    fi
    mountpoint -q /dev/hugepages || fail "/dev/hugepages is not mounted"
}

# A tmpfs MXL root. Usage: make_mxl_root <dir> [size]
make_mxl_root() {
    local dir="$1" size="${2:-2g}"
    mkdir -p "$dir"
    as_root mount -t tmpfs -o "size=$size,mode=1777" tmpfs "$dir"
    on_exit "as_root umount -l '$dir'"
}

# start_gateway <name> <image> <config-dir> <mxl-root> <network: host|container:NAME> [extra docker args...]
# MTL (also its kernel backend) needs root for the host's hugetlbfs; group 1000 as in deploy/k8s, so the
# tools running as the image user (uid 1000) can write into the domains the gateway creates.
start_gateway() {
    local name="$1" image="$2" config="$3" mxl="$4" net="$5"
    shift 5
    docker rm -f "$name" >/dev/null 2>&1 || true
    docker run -d --name "$name" \
        --user 0:1000 \
        --network "$net" \
        --cap-add IPC_LOCK --cap-add SYS_NICE --cap-add NET_RAW --cap-add NET_ADMIN \
        --ulimit memlock=-1:-1 \
        -v /dev/hugepages:/dev/hugepages \
        -v "$config":/config \
        -v "$mxl":/Volumes/mxl \
        -e MXLGW_CONFIG=/config/gateway.json \
        "$@" "$image" >/dev/null
    IT_CONTAINERS+=("$name")
}

# run_tool <image> <mxl-root> <tool> [args...]   (foreground, removes the container)
run_tool() {
    local image="$1" mxl="$2"
    shift 2
    docker run --rm --network none -v "$mxl":/Volumes/mxl "$image" "$@"
}

# start_tool <name> <image> <mxl-root> <tool> [args...]   (background)
start_tool() {
    local name="$1" image="$2" mxl="$3"
    shift 3
    docker rm -f "$name" >/dev/null 2>&1 || true
    docker run -d --name "$name" --network none -v "$mxl":/Volumes/mxl "$image" "$@" >/dev/null
    IT_CONTAINERS+=("$name")
}

# wait_until <timeout-s> <description> <command...>
wait_until() {
    local timeout="$1" what="$2"
    shift 2
    local end=$((SECONDS + timeout))
    until "$@" >/dev/null 2>&1; do
        if ((SECONDS >= end)); then
            fail "timeout after ${timeout}s waiting for: $what"
        fi
        sleep 1
    done
}

http_ok() { curl -fsS -o /dev/null --max-time 3 "$1"; }

# json <url> <python-expression on `j`>   e.g. json "$base/api/status" 'j["setup_mode"]'
json() {
    curl -fsS --max-time 5 "$1" | python3 -c "import json,sys; j=json.load(sys.stdin); v=($2); print(v if not isinstance(v,(dict,list)) else json.dumps(v))"
}

# readiness reasons (sorted) regardless of the 200/503 status
readyz_reasons() {
    curl -sS --max-time 5 "$1/readyz" | python3 -c "import json,sys; print(json.dumps(sorted(json.load(sys.stdin)['reasons'])))"
}

# essence_field <base> <essence-label> <field>
essence_field() {
    json "$1/api/status" "next(e for g in j['groups'] for e in g['essences'] if e['label']=='$2').get('$3')"
}

essence_state_is() { [[ "$(essence_field "$1" "$2" state)" == "$3" ]]; }

# nmos_id <base> receivers|senders <label>
nmos_id() {
    json "$1/x-nmos/node/v1.3/$2" "next(r['id'] for r in j if r['label']=='$3')"
}

# patch_staged <base> receivers|senders <id> <json-body>
patch_staged() {
    local body code
    body=$(mktemp)
    code=$(curl -sS --max-time 10 -o "$body" -w '%{http_code}' -X PATCH -H 'Content-Type: application/json' \
        "$1/x-nmos/connection/v1.2/single/$2/$3/staged" -d "$4") || true
    if [[ "$code" != "200" ]]; then
        cat "$body" >&2 || true
        rm -f "$body"
        fail "PATCH $2/$3 returned $code"
    fi
    rm -f "$body"
}

# metric <base> <name{labels}> → value of the first matching sample (empty if none)
metric() {
    curl -fsS --max-time 5 "$1/metrics" | python3 -c "
import re, sys
want = sys.argv[1]
name, _, labels = want.partition('{')
need = dict(re.findall(r'(\w+)=\"([^\"]*)\"', labels))
for line in sys.stdin:
    if not line.startswith(name):
        continue
    m = re.match(r'([a-zA-Z_:][\w:]*)(\{(.*)\})? (\S+)$', line.strip())
    if not m or m.group(1) != name:
        continue
    have = dict(re.findall(r'(\w+)=\"((?:[^\"\\\\]|\\\\.)*)\"', m.group(3) or ''))
    if all(have.get(k) == v for k, v in need.items()):
        print(m.group(4))
        break
" "$2"
}

# A container that only holds a network namespace; gateways join it with --network container:<name>.
# make_netns <name> <image>
make_netns() {
    local ns="$1" image="$2"
    docker rm -f "$ns" >/dev/null 2>&1 || true
    docker run -d --name "$ns" --network none --entrypoint sleep "$image" infinity >/dev/null
    IT_CONTAINERS+=("$ns")
}

netns_pid() { docker inspect -f '{{.State.Pid}}' "$1"; }

# in_netns <ns-container> <command...>
in_netns() {
    local pid
    pid=$(netns_pid "$1")
    shift
    as_root nsenter -t "$pid" -n "$@"
}

# veth pair with the second end inside the namespace container (the kernel drops packets from its
# own addresses as martians when both ends live in one namespace).
# make_veth <host-if> <ns-if> <host-ip/len> <ns-ip/len> <ns-container>
make_veth() {
    local hif="$1" nif="$2" hip="$3" nip="$4" ns="$5"
    local pid
    pid=$(netns_pid "$ns")
    as_root ip link del "$hif" 2>/dev/null || true
    as_root ip link add "$hif" type veth peer name "$nif"
    on_exit "as_root ip link del '$hif'"
    as_root ip link set "$nif" netns "$pid"
    as_root ip addr add "$hip" dev "$hif"
    as_root ip link set "$hif" up
    as_root sysctl -q -w "net.ipv4.conf.$hif.rp_filter=0"
    as_root nsenter -t "$pid" -n ip addr add "$nip" dev "$nif"
    as_root nsenter -t "$pid" -n ip link set "$nif" up
    as_root nsenter -t "$pid" -n ip link set lo up
    as_root nsenter -t "$pid" -n sysctl -q -w "net.ipv4.conf.$nif.rp_filter=0" || true
    as_root nsenter -t "$pid" -n sysctl -q -w net.ipv4.conf.all.rp_filter=0 || true
    as_root sysctl -q -w net.core.rmem_max=4194304
    as_root sysctl -q -w net.core.wmem_max=4194304
}

# ---- media verification on the test-only kernel backend
# The kernel backend has no pacing guarantees and reads the two legs' sockets one after the other
# (§17.2). Under CPU load (GitHub runners) MTL drops frames whose transmit time passed
# (mxl_st2110_gateway_tx_late_frames_total), loses packets on both legs (audio: its "unrecovered (lost on both)"
# statistic; video: frames counted as incomplete) and the ingest correctly marks such grains invalid.
# Bad audio blocks and invalid video grains are accepted only if those counters explain them for the
# same window (one lost audio packet can touch two verify blocks); bars, frame counters, timecode,
# offsets and A/V alignment must always be exact (docs/decisions.md).
#
# The caller sets ACCT_EGRESS / ACCT_INGEST (base URLs), ACCT_INGEST_CONTAINER and the metric
# selectors ACCT_AUDIO_TX (egress, late audio), ACCT_VIDEO_TX (egress, late video) and ACCT_VIDEO_RX
# (ingest, incomplete/dropped video).
sum_metrics() { # <base> <metric{labels}>...
    local base="$1" sum=0 v
    shift
    for m in "$@"; do
        v=$(metric "$base" "$m")
        sum=$((sum + ${v:-0}))
    done
    echo "$sum"
}
acct_late_audio() { sum_metrics "$ACCT_EGRESS" "${ACCT_AUDIO_TX[@]}"; }
acct_lost_video() {
    echo $(($(sum_metrics "$ACCT_EGRESS" "${ACCT_VIDEO_TX[@]}") + $(sum_metrics "$ACCT_INGEST" "${ACCT_VIDEO_RX[@]}")))
}
acct_unrecovered_audio() { # <since RFC 3339>
    docker logs --since "$1" "$ACCT_INGEST_CONTAINER" 2>&1 |
        grep -o 'RX_AUDIO_SESSION([0-9,]*): per-port loss[^"]*unrecovered (lost on both) [0-9]*' |
        awk '{s += $NF} END {print s + 0}'
}
verify_media() { # <image> <mxl-root> <report.json> <mxl-verify arguments...>
    local image="$1" mxl="$2" report="$3" since late0 video0 audio_allowed video_allowed
    shift 3
    since=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    late0=$(acct_late_audio)
    video0=$(acct_lost_video)
    run_tool "$image" "$mxl" mxl-verify "$@" >"$report" && return 0
    sleep 11 # MTL prints its per-port loss statistics every 10 s
    audio_allowed=$(($(acct_late_audio) - late0 + 2 * $(acct_unrecovered_audio "$since")))
    video_allowed=$(($(acct_lost_video) - video0))
    python3 - "$report" "$audio_allowed" "$video_allowed" <<'PY'
import json, sys
r = json.load(open(sys.argv[1]))
audio_allowed, video_allowed = int(sys.argv[2]), int(sys.argv[3])
explainable = ("audio tone frequency/level/phase mismatch", "video grains missing (marked invalid)")
other = [f for f in r["failures"] if f not in explainable]
bad = r.get("audio", {}).get("bad_blocks", 0)
invalid = r.get("video", {}).get("invalid", 0)
if not other and bad <= audio_allowed and invalid <= video_allowed:
    print(f"accepted: {bad} bad audio block(s) (allowance {audio_allowed}), {invalid} invalid video grain(s) "
          f"(allowance {video_allowed}), all explained by late, incomplete or both-legs-lost packets counted by the gateways", file=sys.stderr)
    sys.exit(0)
print(f"not explained: {bad} bad audio block(s) (allowance {audio_allowed}), {invalid} invalid video grain(s) "
      f"(allowance {video_allowed}), other failures {other}", file=sys.stderr)
sys.exit(1)
PY
}
