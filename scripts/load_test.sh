#!/usr/bin/env bash

set -euo pipefail

if ! command -v wrk >/dev/null 2>&1; then
  echo "wrk is required for this load test. Install wrk and rerun scripts/load_test.sh." >&2
  exit 1
fi

TARGET_URL="${TARGET_URL:-http://localhost:8000}"
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-100}"
REQUESTS="${REQUESTS:-1000}"
CLIENT_POOL_SIZE="${CLIENT_POOL_SIZE:-250}"

if [[ "$REQUESTS" -lt "$CONNECTIONS" ]]; then
  DURATION=10
else
  DURATION=$(( (REQUESTS / CONNECTIONS) + 5 ))
fi

echo "Running load test"
echo "target_url=$TARGET_URL"
echo "threads=$THREADS"
echo "connections=$CONNECTIONS"
echo "requests_target=$REQUESTS"
echo "client_pool_size=$CLIENT_POOL_SIZE"
echo "duration_seconds=$DURATION"

CLIENT_POOL_SIZE="$CLIENT_POOL_SIZE" \
wrk \
  -t"$THREADS" \
  -c"$CONNECTIONS" \
  -d"${DURATION}s" \
  -s scripts/wrk_rate_limiter.lua \
  "$TARGET_URL"

echo
echo "Post-run metrics"
curl -s "$TARGET_URL/metrics"
