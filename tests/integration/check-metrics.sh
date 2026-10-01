#!/usr/bin/env bash
# Lints a /metrics exposition with promtool (Phase 6 acceptance).
#
#   tests/integration/check-metrics.sh http://127.0.0.1:8080/metrics
#   curl -s …/metrics | tests/integration/check-metrics.sh -
#
# The `_ns` metric names of SPECIFICATION.md §12.1 are a public interface and trip promtool's
# "abbreviated units" lint; that one finding is tolerated (docs/decisions.md, open question O-3).
# Everything else — parse errors, missing HELP/TYPE, counter naming, duplicates — fails.
set -euo pipefail

source_="${1:?usage: check-metrics.sh <url>|-}"
promtool_image="${PROMTOOL_IMAGE:-prom/prometheus:v3.15.0}"

if [[ "$source_" == "-" ]]; then
    body="$(cat)"
else
    body="$(curl -fsS "$source_")"
fi
[[ -n "$body" ]] || { echo "check-metrics: empty exposition" >&2; exit 1; }

if command -v promtool >/dev/null 2>&1; then
    run() { promtool check metrics; }
else
    # pull first: the pull progress would otherwise end up in the captured findings
    docker image inspect "$promtool_image" >/dev/null 2>&1 || docker pull -q "$promtool_image" >/dev/null
    run() { docker run --rm -i --entrypoint /bin/promtool "$promtool_image" check metrics; }
fi

set +e
out="$(printf '%s\n' "$body" | run 2>&1)"
rc=$?
set -e

remaining="$(printf '%s\n' "$out" | grep -vE '^mxlgw_[a-z0-9_]+_ns metric names should not contain abbreviated units$' | grep -v '^$' || true)"
if [[ -n "$remaining" ]]; then
    echo "check-metrics: promtool findings:" >&2
    printf '%s\n' "$remaining" >&2
    exit 1
fi
if [[ $rc -ne 0 && $rc -ne 3 ]]; then
    echo "check-metrics: promtool failed with exit code $rc" >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
echo "check-metrics: ok ($(printf '%s\n' "$body" | grep -c '^mxlgw_') samples)"
