#!/usr/bin/env bash
# AMWA nmos-testing against the gateway (SPECIFICATION.md §17.3, §7.7, acceptance criterion 8).
#
#   tests/integration/nmos-testing.sh mxlgw:dev
#
# Runs IS-04-01, IS-05-01, IS-05-02 and BCP-007-03-01 non-interactively against both NMOS nodes of
# the gateway (§7.1: MXL node on $PORT, ST 2110 node on $PORT2) with the testing tool's mock registry
# announced over multicast DNS-SD (owner decision Q13: no registry container). The gateway uses the
# MTL kernel-socket backend on a local veth pair (§7.7) and node.registry.dns_sd /
# node.st2110.registry.dns_sd = true through the host's avahi-daemon (D-Bus socket mounted into the
# container); DNS-SD stays supported although the platform default is off. The suites run in two
# phases, each with only the node under test registering (the tool's mock registry must see one node).
# Any "Fail" fails the run; JSON results go to $IT_ARTIFACTS.
source "$(dirname "$0")/lib.sh"

IMAGE="${1:?usage: nmos-testing.sh <image>}"
# The pin lives in .github/workflows/ci.yaml (AGENTS.md: pins in exactly two places).
NMOS_TESTING_REF="${NMOS_TESTING_REF:-$(sed -n 's/^ *NMOS_TESTING_REF: *\([0-9a-f]\{40\}\).*/\1/p' "$(dirname "$0")/../../.github/workflows/ci.yaml" | head -1)}"
[[ -n "$NMOS_TESTING_REF" ]] || fail "NMOS_TESTING_REF not set and not found in .github/workflows/ci.yaml"
PORT=18185  # MXL node (MXL Senders/Receivers)
PORT2=18186 # ST 2110 node (RTP Senders/Receivers), node.http_port + 1
need docker curl python3 ip

WORK="$(mktemp -d /tmp/mxlgw-nmostest.XXXXXX)"
on_exit "as_root rm -rf '$WORK'"
ensure_hugepages 1024
make_mxl_root "$WORK/mxl" 1g
# Media ports for the kernel backend: both ends of one veth pair (activations send into it).
as_root ip link del mxlit6 2>/dev/null || true
as_root ip link add mxlit6 type veth peer name mxlit7
on_exit "as_root ip link del mxlit6"
as_root ip addr add 192.168.82.1/24 dev mxlit6
as_root ip addr add 192.168.83.1/24 dev mxlit7
as_root ip link set mxlit6 up
as_root ip link set mxlit7 up
mkdir -p "$WORK/config" "$IT_ARTIFACTS"

# ---- avahi on the host (the gateway's dns_sd client talks to it over D-Bus)
if ! pgrep -x avahi-daemon >/dev/null; then
    command -v avahi-daemon >/dev/null || as_root apt-get install -y -qq avahi-daemon >/dev/null
fi
# the package starts the daemon itself where systemd runs (GitHub runners)
if ! pgrep -x avahi-daemon >/dev/null; then
    if [[ -d /run/systemd/system ]]; then
        as_root systemctl start avahi-daemon
    else
        pgrep -f "dbus-daemon --system" >/dev/null || { as_root mkdir -p /run/dbus; as_root rm -f /run/dbus/pid; as_root dbus-daemon --system --fork; }
        as_root avahi-daemon -D --no-chroot
    fi
fi
wait_until 10 "avahi-daemon" pgrep -x avahi-daemon

# ---- nmos-testing at the pinned commit
TOOL="$WORK/nmos-testing"
mkdir -p "$TOOL"
curl -fsSL "https://codeload.github.com/AMWA-TV/nmos-testing/tar.gz/$NMOS_TESTING_REF" | tar xz -C "$TOOL" --strip-components=1
python3 -c "import ensurepip" 2>/dev/null || as_root apt-get install -y -qq python3-venv >/dev/null
# netifaces builds from source
[[ -e "$(python3 -c 'import sysconfig; print(sysconfig.get_paths()["include"])')/Python.h" ]] || as_root apt-get install -y -qq python3-dev >/dev/null
python3 -m venv "$WORK/venv"
"$WORK/venv/bin/pip" install -q --disable-pip-version-check -r "$TOOL/requirements.txt"
cat >"$TOOL/nmostesting/UserConfig.py" <<'EOF'
from . import Config as CONFIG
CONFIG.ENABLE_DNS_SD = True
CONFIG.DNS_SD_MODE = 'multicast'
CONFIG.DNS_SD_ADVERT_TIMEOUT = 60
CONFIG.API_PROCESSING_TIMEOUT = 2
CONFIG.HTTP_TIMEOUT = 5
CONFIG.MAX_TEST_ITERATIONS = 0
# The SDPs the tool stages must match the receivers' fixed formats (§6.4): 1080p50, 8 ch L24 1 ms.
CONFIG.SDP_PREFERENCES.update({"channels": 8, "sample_rate": 48000, "packet_time": 1, "max_packet_time": 1,
                               "width": 1920, "height": 1080, "interlace": False, "exactframerate": "50",
                               "depth": 10, "sampling": "YCbCr-4:2:2", "colorimetry": "BT709", "TCS": "SDR", "TP": "2110TPN"})
EOF

# ---- the gateway: kernel backend, real MXL domain, one ingest and one egress group
HOST_IP="${NMOS_TEST_HOST_IP:-$(ip -4 route get 1.1.1.1 | sed -n 's/.* src \([0-9.]*\).*/\1/p')}"
# Only the node under test registers (DNS-SD): nmos-testing's mock registry must not see the other node.
gateway_config() { # <MXL node dns_sd> <ST 2110 node dns_sd>
    cat <<EOF
{
  "schema_version": 1,
  "node": {"label": "IT-NMOS", "http_port": $PORT, "host_address": "$HOST_IP", "registry": {"dns_sd": $1},
           "st2110": {"registry": {"dns_sd": $2}}},
  "nic": {"backend": "kernel", "lcores": "1", "port_pairs": [{"name": "media",
          "primary":   {"name": "media-p", "ifname": "mxlit6", "ip": "192.168.82.1", "netmask": "255.255.255.0"},
          "redundant": {"name": "media-r", "ifname": "mxlit7", "ip": "192.168.83.1", "netmask": "255.255.255.0"}}]},
  "ptp": {"mode": "external", "require_lock": false},
  "mxl": {"scan_path": "/Volumes/mxl", "domains": [{"name": "main", "path": "/Volumes/mxl/main", "history_duration_ns": 100000000}]},
  "groups": [
    {"label": "CAM 1", "direction": "ingest", "domain": "main", "redundancy": true,
     "video": [{"label": "CAM 1 V", "width": 1920, "height": 1080, "rate": "50/1",
                "defaults": {"legs": [{"multicast": "239.1.1.1", "port": 20000}, {"multicast": "239.2.1.1", "port": 20000}]}}],
     "audio": [{"label": "CAM 1 A1-8", "channels": 8, "defaults": {"legs": [{"multicast": "239.1.1.2", "port": 20000}, {"multicast": "239.2.1.2", "port": 20000}]}}],
     "anc":   [{"label": "CAM 1 ANC", "defaults": {"legs": [{"multicast": "239.1.1.3", "port": 20000}, {"multicast": "239.2.1.3", "port": 20000}]}}]},
    {"label": "PGM", "direction": "egress", "domain": "main", "redundancy": true,
     "video": [{"label": "PGM V", "width": 1920, "height": 1080, "rate": "50/1",
                "defaults": {"legs": [{"multicast": "239.10.0.1", "port": 20000}, {"multicast": "239.20.0.1", "port": 20000}]}}],
     "audio": [{"label": "PGM A1-8", "channels": 8, "defaults": {"legs": [{"multicast": "239.10.0.2", "port": 20000}, {"multicast": "239.20.0.2", "port": 20000}]}}],
     "anc":   [{"label": "PGM ANC", "defaults": {"legs": [{"multicast": "239.10.0.3", "port": 20000}, {"multicast": "239.20.0.3", "port": 20000}]}}]}
  ]
}
EOF
}
# dbus-daemon refuses AppArmor-confined clients whose profile has no D-Bus rules (docker-default),
# which nmos-cpp reports as DNSServiceBrowse error -65553 (kDNSServiceErr_Refused).
aa_opts=()
if [[ "$(cat /sys/module/apparmor/parameters/enabled 2>/dev/null)" == "Y" ]]; then
    aa_opts=(--security-opt apparmor=unconfined)
fi
run_gateway() { # <MXL node dns_sd> <ST 2110 node dns_sd>
    if docker inspect "$IT_PREFIX-nmos" >/dev/null 2>&1; then
        docker stop -t 15 "$IT_PREFIX-nmos" >/dev/null
        docker logs "$IT_PREFIX-nmos" >"$IT_ARTIFACTS/$IT_PREFIX-nmos-$3.log" 2>&1 || true
    fi
    # the gateway runs as root and rewrites the file (generated uids)
    gateway_config "$1" "$2" | as_root tee "$WORK/config/gateway.json" >/dev/null
    start_gateway "$IT_PREFIX-nmos" "$IMAGE" "$WORK/config" "$WORK/mxl" host -v /run/dbus:/run/dbus -v /run/avahi-daemon:/run/avahi-daemon "${aa_opts[@]}"
    wait_until 90 "gateway /livez" http_ok "http://127.0.0.1:$PORT/livez"
    wait_until 30 "ST 2110 node" http_ok "http://127.0.0.1:$PORT2/x-nmos/node/v1.3/self"
}

run_suite() { # <tag> <suite> <args...>
    local tag="$1" suite="$2"
    shift 2
    log "nmos-testing $suite $*"
    set +e
    (cd "$TOOL" && "$WORK/venv/bin/python" nmos-test.py suite "$suite" --selection all "$@" --output "$IT_ARTIFACTS/nmos-testing-$tag.json") \
        >"$IT_ARTIFACTS/nmos-testing-$tag.log" 2>&1
    local rc=$?
    set -e
    python3 - "$IT_ARTIFACTS/nmos-testing-$tag.json" <<'EOF' || true
import json, sys
try:
    data = json.load(open(sys.argv[1]))
except Exception as e:
    print(f"  (no results: {e})")
    sys.exit(0)
counts = {}
for r in data.get("results", []):
    counts[r["state"]] = counts.get(r["state"], 0) + 1
    if r["state"] in ("Fail", "Test Error"):
        print(f"  {r['state']}: {r['name']}: {r.get('detail', '')}")
print("  " + ", ".join(f"{k}: {v}" for k, v in sorted(counts.items())))
EOF
    case $rc in
        0 | 1) pass "$tag (exit $rc)" ;;
        *) FAILED_SUITES+=("$tag"); log "FAIL: $tag (exit $rc, see $IT_ARTIFACTS/nmos-testing-$tag.log)" ;;
    esac
}

FAILED_SUITES=()
# MXL node: IS-04, IS-05 v1.2 (MXL transport only exists in v1.2) and BCP-007-03.
run_gateway true false mxl
run_suite IS-04-01-mxl IS-04-01 --host "$HOST_IP" --port "$PORT" --version v1.3
run_suite IS-05-01-v1.2-mxl IS-05-01 --host "$HOST_IP" --port "$PORT" --version v1.2
run_suite IS-05-02-mxl IS-05-02 --host "$HOST_IP" "$HOST_IP" --port "$PORT" "$PORT" --version v1.3 v1.2
run_suite BCP-007-03-01 BCP-007-03-01 --host "$HOST_IP" "$HOST_IP" --port "$PORT" "$PORT" --version v1.3 v1.2
# ST 2110 node: IS-04, IS-05 v1.1 and v1.2.
run_gateway false true st2110
run_suite IS-04-01-st2110 IS-04-01 --host "$HOST_IP" --port "$PORT2" --version v1.3
run_suite IS-05-01-v1.1-st2110 IS-05-01 --host "$HOST_IP" --port "$PORT2" --version v1.1
run_suite IS-05-01-v1.2-st2110 IS-05-01 --host "$HOST_IP" --port "$PORT2" --version v1.2
run_suite IS-05-02-st2110 IS-05-02 --host "$HOST_IP" "$HOST_IP" --port "$PORT2" "$PORT2" --version v1.3 v1.2

if ((${#FAILED_SUITES[@]})); then
    fail "suites with failures: ${FAILED_SUITES[*]}"
fi
pass "nmos-testing"
