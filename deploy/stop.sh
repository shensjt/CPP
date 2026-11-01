#!/usr/bin/env bash
set -euo pipefail

# 停止 systemd 服务（不删除 unit，也不删文件）。
# 用法：sudo bash deploy/stop.sh

APP_NAME=${APP_NAME:-sjtwebserver}
PORT=${PORT:-8080}

if [ "$(id -u)" -ne 0 ]; then
  echo "请用 root 运行：sudo bash deploy/stop.sh" >&2
  exit 1
fi

systemctl stop "${APP_NAME}" 2>/dev/null || true
systemctl --no-pager status "${APP_NAME}" || true

if ss -lntn 2>/dev/null | grep -qE ":${PORT}([[:space:]]|$)"; then
  echo "警告：端口 ${PORT} 仍被其他进程占用：" >&2
  ss -lntp | grep ":${PORT}" || true
  echo "如果是手动运行的 web_server，请回原终端 Ctrl+C 或 kill 掉。" >&2
  exit 1
fi

echo "服务已停止，端口 ${PORT} 已释放。"
