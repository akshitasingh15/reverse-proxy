#!/usr/bin/env bash
# Load test with wrk: 3 Python backends behind the proxy, rate limiting off.
# Usage (from project root):  ./bench/benchmark.sh [connections] [seconds]
set -u
cd "$(dirname "$0")/.."
CONNS=${1:-100}
SECS=${2:-15}
THREADS=${THREADS:-4}

command -v wrk >/dev/null || { echo "install wrk first (sudo apt install wrk / brew install wrk)"; exit 1; }
make -s proxy

TMP=$(mktemp -d); PIDS=()
trap 'kill "${PIDS[@]}" 2>/dev/null; rm -rf "$TMP"' EXIT

for p in 9301 9302 9303; do python3 backend/server.py $p > /dev/null 2>&1 & PIDS+=($!); done
cat > "$TMP/bench.conf" <<EOF
listen_port = 18888
threads = $THREADS
rate_limit_capacity = 0
backend = 127.0.0.1:9301
backend = 127.0.0.1:9302
backend = 127.0.0.1:9303
EOF
./proxy "$TMP/bench.conf" > /dev/null 2>&1 & PIDS+=($!)
sleep 1

echo "== wrk: $CONNS connections, ${SECS}s, proxy worker threads=$THREADS =="
wrk -t2 -c"$CONNS" -d"${SECS}s" --latency http://127.0.0.1:18888/
