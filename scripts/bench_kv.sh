#!/usr/bin/env bash
set -euo pipefail

# 一次 KV 基线：启动 kv_store、pidstat 采样、跑 RUNS 次。
# 用法示例：
#   MODE=set   CLIENTS=128 REQUESTS=200000 AOF=off      ./scripts/bench_kv.sh
#   MODE=get   CLIENTS=128 REQUESTS=200000 AOF=everysec ./scripts/bench_kv.sh
#   MODE=mixed CLIENTS=128 REQUESTS=200000 AOF=off      ./scripts/bench_kv.sh

KV_BIN="${KV_BIN:-build/server/kv_store}"
HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-16380}"
THREADS="${THREADS:-4}"
MODE="${MODE:-set}"
AOF="${AOF:-off}"
CLIENTS="${CLIENTS:-128}"
REQUESTS="${REQUESTS:-200000}"
KEYS="${KEYS:-10000}"
VALUE_SIZE="${VALUE_SIZE:-64}"
RUNS="${RUNS:-3}"
MEMTIER_THREADS="${MEMTIER_THREADS:-4}"
OUT_DIR="${OUT_DIR:-doc/benchmarks/raw/kv-${MODE}-aof-${AOF}-c${CLIENTS}-t${THREADS}-$(date +%Y%m%d-%H%M%S)}"

if [ "$MODE" != "set" ] && [ "$MODE" != "get" ] && [ "$MODE" != "mixed" ]; then
    echo "MODE must be set|get|mixed" >&2
    exit 1
fi
if [ "$AOF" != "off" ] && [ "$AOF" != "everysec" ]; then
    echo "AOF must be off|everysec" >&2
    exit 1
fi
if [ ! -x "$KV_BIN" ]; then
    echo "kv binary not found: $KV_BIN" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
AOF_PATH="$OUT_DIR/kv.aof"
rm -f "$AOF_PATH"

{
    date -u
    uname -a
    nproc
    (lscpu | grep -E 'Model name|CPU\(s\)|Thread|Core|Socket' || true)
    (g++ --version | head -1 || true)
    echo "commit=$(git rev-parse HEAD 2>/dev/null || true)"
    echo "kv=$KV_BIN host=$HOST port=$PORT threads=$THREADS mode=$MODE aof=$AOF"
    echo "clients=$CLIENTS requests=$REQUESTS keys=$KEYS value_size=$VALUE_SIZE runs=$RUNS"
    (redis-benchmark --version 2>/dev/null || true)
    (memtier_benchmark --version 2>/dev/null | head -1 || true)
} > "$OUT_DIR/environment.txt"

aof_args=(--no-aof)
if [ "$AOF" = "everysec" ]; then
    aof_args=(--aof "$AOF_PATH")
fi

server_pid=
sampler_pid=

start_server() {
    "$KV_BIN" --host "$HOST" --port "$PORT" --threads "$THREADS" \
        --timeout 60 "${aof_args[@]}" >> "$OUT_DIR/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 100); do
        if redis-cli -h "$HOST" -p "$PORT" ping 2>/dev/null | grep -q PONG; then
            return 0
        fi
        sleep 0.1
    done
    echo "kv_store failed to start" >&2
    cat "$OUT_DIR/server.log" >&2
    return 1
}

stop_server() {
    if [ -n "${server_pid:-}" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
        server_pid=
    fi
}

cleanup() {
    if [ -n "${sampler_pid:-}" ]; then
        kill "$sampler_pid" 2>/dev/null || true
        wait "$sampler_pid" 2>/dev/null || true
    fi
    stop_server
}
trap cleanup EXIT

start_server
pidstat -u -t -r -w -p "$server_pid" 1 > "$OUT_DIR/pidstat.log" 2>&1 &
sampler_pid=$!

# GET/MIXED 先预热 key 空间，避免 100% miss。
case "$MODE" in
    get)
        echo "preload: redis-benchmark SET" | tee -a "$OUT_DIR/preload.log"
        redis-benchmark -h "$HOST" -p "$PORT" \
            -n "$((KEYS * 10))" -c 20 -d "$VALUE_SIZE" -r "$KEYS" \
            -t set -q >> "$OUT_DIR/preload.log" 2>&1
        ;;
    mixed)
        echo "preload: memtier SET sequential" | tee -a "$OUT_DIR/preload.log"
        memtier_benchmark -s "$HOST" -p "$PORT" --protocol=redis \
            --threads=1 --clients=1 --ratio=1:0 --key-pattern=S:S \
            --key-minimum=1 --key-maximum="$KEYS" \
            --requests="$KEYS" --hide-histogram >> "$OUT_DIR/preload.log" 2>&1
        ;;
esac

memtier_clients=$(( (CLIENTS + MEMTIER_THREADS - 1) / MEMTIER_THREADS ))
memtier_requests=$(( (REQUESTS + CLIENTS - 1) / CLIENTS ))
if [ "$memtier_requests" -lt 1 ]; then memtier_requests=1; fi

aof_before=0
if [ "$AOF" = "everysec" ] && [ -f "$AOF_PATH" ]; then
    aof_before=$(stat -c '%s' "$AOF_PATH")
fi

: > "$OUT_DIR/redis-benchmark.log"
: > "$OUT_DIR/memtier.log"
for run in $(seq 1 "$RUNS"); do
    echo "=== run $run/$RUNS ===" | tee -a "$OUT_DIR/redis-benchmark.log" "$OUT_DIR/memtier.log"
    if [ "$MODE" = "mixed" ]; then
        memtier_benchmark -s "$HOST" -p "$PORT" --protocol=redis \
            --threads="$MEMTIER_THREADS" --clients="$memtier_clients" \
            --ratio=1:1 --key-pattern=R:R \
            --key-minimum=1 --key-maximum="$KEYS" \
            --requests="$memtier_requests" \
            --hide-histogram --print-percentiles=50,90,99 \
            --json-out-file="$OUT_DIR/memtier-run${run}.json" \
            | tee -a "$OUT_DIR/memtier.log"
    else
        redis-benchmark -h "$HOST" -p "$PORT" \
            -n "$REQUESTS" -c "$CLIENTS" -d "$VALUE_SIZE" -r "$KEYS" \
            -t "$MODE" | tee -a "$OUT_DIR/redis-benchmark.log"
    fi
done

if [ -n "${sampler_pid:-}" ]; then
    kill "$sampler_pid" 2>/dev/null || true
    wait "$sampler_pid" 2>/dev/null || true
    sampler_pid=
fi

stop_server

aof_after=0
if [ "$AOF" = "everysec" ] && [ -f "$AOF_PATH" ]; then
    aof_after=$(stat -c '%s' "$AOF_PATH")
    echo "aof_before_bytes=$aof_before" > "$OUT_DIR/aof.txt"
    echo "aof_after_bytes=$aof_after" >> "$OUT_DIR/aof.txt"
fi

if [ "$AOF" = "everysec" ]; then
    start_ns=$(date +%s%N)
    start_server
    end_ns=$(date +%s%N)
    echo "replay_ms=$(( (end_ns - start_ns) / 1000000 ))" > "$OUT_DIR/replay.txt"
    stop_server
fi

echo "done: $OUT_DIR"
