#!/usr/bin/env bash
# Container entrypoint (SPECIFICATION.md §14). The binary itself validates the configuration and the
# environment (preflight, §14.3) and exits 78 with an actionable message; this script only adds
# checks that must happen before the process starts and lets other tools in the image be run.
set -u

log() {
    printf '{"ts":"%s","level":"%s","event":"%s","details":"%s"}\n' "$(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)" "$1" "$2" "$3" >&2
}

# `docker run … mxl-info -d /Volumes/mxl/main -l`, `… mxl-verify --help`, `… bash`
if [[ $# -gt 0 && "$1" != -* ]]; then
    exec "$@"
fi
case "${1:-}" in
    --version | --help | -h) exec /usr/local/bin/mxl-st2110-gateway "$@" ;;
esac

memlock="$(ulimit -l)"
if [[ "$memlock" != "unlimited" ]]; then
    log warn entrypoint_memlock "memlock ulimit is ${memlock} KiB; DPDK needs it unlimited (compose: ulimits.memlock -1, Kubernetes: CAP_IPC_LOCK)"
fi

config="${MXLGW_CONFIG:-/config/gateway.json}"
config_dir="$(dirname "$config")"
if [[ ! -d "$config_dir" ]]; then
    log error entrypoint_check_failed "configuration directory ${config_dir} does not exist; mount a writable volume at /config (README#config-volume)"
    exit 78
fi
if [[ ! -w "$config_dir" ]]; then
    log error entrypoint_check_failed "configuration directory ${config_dir} is not writable; the gateway writes gateway.json and state/ there (README#config-volume)"
    exit 78
fi

exec /usr/local/bin/mxl-st2110-gateway "$@"
