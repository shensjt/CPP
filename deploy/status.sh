#!/usr/bin/env bash
set -euo pipefail

# 查看服务状态、端口、本地请求和最近日志。
# 用法：sudo bash deploy/status.sh

APP_NAME=${APP_NAME:-sjtwebserver}
PORT=${PORT:-8080}

echo "=== active / enabled ==="
systemctl is-active "${APP_NAME}" 2>/dev/null || true
systemctl is-enabled "${APP_NAME}" 2>/dev/null || true

echo
echo "=== status ==="
systemctl --no-pager status "${APP_NAME}" || true

echo
echo "=== listen :${PORT} ==="
ss -lntp | grep ":${PORT}" || echo "没有进程监听 ${PORT}"

echo
echo "=== local health ==="
curl -fsS -o /dev/null -w "http://127.0.0.1:${PORT}/ -> %{http_code}\n" \
  --max-time 3 "http://127.0.0.1:${PORT}/" || echo "请求失败"

echo
echo "=== recent logs ==="
journalctl -u "${APP_NAME}" -n 20 --no-pager 2>/dev/null || true
