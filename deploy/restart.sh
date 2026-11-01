#!/usr/bin/env bash
set -euo pipefail

# 重启 systemd 服务并做一次本地健康检查。
# 用法：sudo bash deploy/restart.sh

APP_NAME=${APP_NAME:-sjtwebserver}
PORT=${PORT:-8080}

if [ "$(id -u)" -ne 0 ]; then
  echo "请用 root 运行：sudo bash deploy/restart.sh" >&2
  exit 1
fi

systemctl restart "${APP_NAME}"
systemctl --no-pager status "${APP_NAME}" || true

echo
echo -n "本地健康检查："
curl -fsS -o /dev/null -w "http://127.0.0.1:${PORT}/ -> %{http_code}\n" \
  --max-time 3 "http://127.0.0.1:${PORT}/" || echo "失败"
