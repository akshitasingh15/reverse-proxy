#!/usr/bin/env bash
# End-to-end tests: starts backends and proxies, checks each feature with curl.
# Run from the project root:  make test
set -u
cd "$(dirname "$0")/.."

PASS=0; FAIL=0
TMP=$(mktemp -d)
PIDS=()

cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

check() {  # check "<name>" "<expected>" "<actual>"
    if [ "$2" == "$3" ]; then echo "  PASS  $1"; PASS=$((PASS+1));
    else echo "  FAIL  $1 (expected '$2', got '$3')"; FAIL=$((FAIL+1)); fi
}

start_proxy() {  # start_proxy <port> <extra config lines...>
    local port=$1; shift
    local conf="$TMP/proxy_$port.conf"
    printf "listen_port = %s\nthreads = 4\n" "$port" > "$conf"
    printf "%s\n" "$@" >> "$conf"
    ./proxy "$conf" > "$TMP/proxy_$port.log" 2>&1 & PIDS+=($!)
}

for p in 9101 9102 9103; do python3 backend/server.py $p > /dev/null 2>&1 & PIDS+=($!); done

BACKENDS=("backend = 127.0.0.1:9101" "backend = 127.0.0.1:9102" "backend = 127.0.0.1:9103")
start_proxy 18080 "${BACKENDS[@]}" "rate_limit_capacity = 0"
start_proxy 18081 "${BACKENDS[@]}" "rate_limit_capacity = 5" "rate_limit_refill_per_sec = 1"
start_proxy 18082 "${BACKENDS[@]}" "rate_limit_capacity = 0" "deny = 127.0.0.0/8"
start_proxy 18083 "backend = 127.0.0.1:9104" "backend = 127.0.0.1:9105" "rate_limit_capacity = 0"
start_proxy 18084 "backend = 127.0.0.1:9104" "backend = 127.0.0.1:9101" "rate_limit_capacity = 0"
sleep 1

echo "Forwarding"
check "GET is forwarded" "200" "$(curl -s -o /dev/null -w '%{http_code}' localhost:18080/hello)"
check "POST body is forwarded" "backend-9101 received: zscaler" \
      "$(curl -s -X POST -d zscaler localhost:18080/ | sed 's/backend-910[0-9]/backend-9101/')"
check "X-Forwarded-For is added" "1" "$(curl -s localhost:18080/ | grep -c 'client=127.0.0.1')"

echo "Load balancing"
for i in $(seq 30); do curl -s localhost:18080/; done > "$TMP/rr.txt"
for p in 9101 9102 9103; do
    check "backend $p got 10 of 30 requests" "10" "$(grep -c "backend-$p" "$TMP/rr.txt")"
done

echo "Failover"
codes=$(for i in $(seq 6); do curl -s -o /dev/null -w '%{http_code} ' localhost:18084/; done)
check "one dead backend: all requests still succeed" "200 200 200 200 200 200 " "$codes"
check "all backends dead returns 502" "502" "$(curl -s -o /dev/null -w '%{http_code}' localhost:18083/)"

echo "Rate limiting"
codes=$(for i in $(seq 8); do curl -s -o /dev/null -w '%{http_code} ' localhost:18081/; done)
check "burst of 8: first 5 allowed, rest 429" "200 200 200 200 200 429 429 429 " "$codes"
sleep 2.1
check "tokens refill over time" "200" "$(curl -s -o /dev/null -w '%{http_code}' localhost:18081/)"

echo "Access control"
check "denied CIDR gets 403" "403" "$(curl -s -o /dev/null -w '%{http_code}' localhost:18082/)"

echo "Concurrency"
CURLS=()
for i in $(seq 50); do curl -s -o /dev/null -w '%{http_code}\n' localhost:18080/ >> "$TMP/conc.txt" & CURLS+=($!); done
wait "${CURLS[@]}"
check "50 parallel requests all succeed" "50" "$(grep -c 200 "$TMP/conc.txt")"

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
