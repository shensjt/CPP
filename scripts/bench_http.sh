#!/usr/bin/env bash
set -euo pipefail

# 一次 HTTP 基线：启动 server、pidstat 采样、wrk 跑 RUNS 次。
# 用法示例：
#   THREADS=4 CACHE_BYTES=16777216 RUNS=3 ./scripts/bench_http.sh
#   THREADS=4 CACHE_BYTES=0 RUNS=3 ./scripts/bench_http.sh
#   THREADS=4 CACHE_BYTES=16777216 PATH_URL=/1mb.bin CONNECTIONS=32 ./scripts/bench_http.sh
# 旧版本（不支持 --cache-bytes）可设 CACHE_BYTES= 。

SERVER_BIN="${SERVER_BIN:-build/server/web_server}"
ROOT="${ROOT:-build/bench-www}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-18080}"
THREADS="${THREADS:-4}"
CACHE_BYTES="${CACHE_BYTES-16777216}"
PATH_URL="${PATH_URL:-/}"
WRK_THREADS="${WRK_THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-100}"
DURATION="${DURATION:-30s}"
RUNS="${RUNS:-3}"
KEEP_ALIVE="${KEEP_ALIVE:-1}"
OUT_DIR="${OUT_DIR:-doc/benchmarks/raw/http-t${THREADS}-cache${CACHE_BYTES:-none}-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$OUT_DIR"

if [ ! -x "$SERVER_BIN" ]; then
    echo "server binary not found: $SERVER_BIN" >&2
    exit 1
fi

if [ ! -f "$ROOT/index.html" ]; then
    if [ ! -f build/www/index.html ]; then
        echo "build/www/index.html not found; run make blog-local first" >&2
        exit 1
    fi
    mkdir -p "$ROOT"
    cp -a build/www/. "$ROOT/"
fi
if [ ! -f "$ROOT/1mb.bin" ]; then
    dd if=/dev/zero of="$ROOT/1mb.bin" bs=1M count=1 status=none
fi

{
    date -u
    uname -a
    nproc
    (lscpu | grep -E 'Model name|CPU\(s\)|Thread|Core|Socket' || true)
    (g++ --version | head -1 || true)
    echo "commit=$(git rev-parse HEAD 2>/dev/null || true)"
    echo "server=$SERVER_BIN"
    echo "root=$ROOT host=$HOST port=$PORT threads=$THREADS cache_bytes=${CACHE_BYTES:-default}"
    echo "path=$PATH_URL wrk_threads=$WRK_THREADS connections=$CONNECTIONS duration=$DURATION runs=$RUNS keep_alive=$KEEP_ALIVE"
} > "$OUT_DIR/environment.txt"

server_args=(
    --host "$HOST" --port "$PORT" --root "$ROOT"
    --threads "$THREADS" --timeout 60
)
if [ -n "$CACHE_BYTES" ]; then
    server_args+=(--cache-bytes "$CACHE_BYTES")
fi

"$SERVER_BIN" "${server_args[@]}" > "$OUT_DIR/server.log" 2>&1 &
server_pid=$!
sampler_pid=

cleanup() {
    if [ -n "${sampler_pid:-}" ]; then
        kill "$sampler_pid" 2>/dev/null || true
        wait "$sampler_pid" 2>/dev/null || true
    fi
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 50); do
    if curl -fsS "http://$HOST:$PORT/" >/dev/null 2>&1; then
        break
    fi
    sleep 0.2
done
if ! curl -fsS "http://$HOST:$PORT/" >/dev/null 2>&1; then
    echo "server failed to start" >&2
    cat "$OUT_DIR/server.log" >&2
    exit 1
fi

pidstat -u -t -r -w -p "$server_pid" 1 > "$OUT_DIR/pidstat.log" 2>&1 &
sampler_pid=$!

wrk_args=(-t "$WRK_THREADS" -c "$CONNECTIONS" -d "$DURATION" --latency)
if [ "$KEEP_ALIVE" = "0" ]; then
    wrk_args+=(-H "Connection: close")
fi

{
    echo "server: $SERVER_BIN ${server_args[*]}"
    echo "wrk: wrk ${wrk_args[*]} http://$HOST:$PORT$PATH_URL"
    echo "runs: $RUNS"
} > "$OUT_DIR/command.txt"

: > "$OUT_DIR/wrk.log"
for run in $(seq 1 "$RUNS"); do
    echo "=== run $run/$RUNS ===" | tee -a "$OUT_DIR/wrk.log"
    wrk "${wrk_args[@]}" "http://$HOST:$PORT$PATH_URL" | tee -a "$OUT_DIR/wrk.log"
done

echo "done: $OUT_DIR"
