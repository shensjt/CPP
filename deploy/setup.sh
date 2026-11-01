#!/usr/bin/env bash
set -euo pipefail

# 一键：创建 webserver 用户、写 systemd unit、开机自启并启动。
# 用法（在仓库根目录，root）：
#   sudo bash deploy/setup.sh
#
# 可选环境变量：
#   APP_NAME=sjtwebserver SERVICE_USER=webserver
#   HOST=0.0.0.0 PORT=8080 THREADS=4 TIMEOUT=60

APP_NAME=${APP_NAME:-sjtwebserver}
SERVICE_USER=${SERVICE_USER:-webserver}
HOST=${HOST:-0.0.0.0}
PORT=${PORT:-8080}
THREADS=${THREADS:-4}
TIMEOUT=${TIMEOUT:-60}

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/.." && pwd)
BIN="${ROOT_DIR}/build/server/web_server"
WWW="${ROOT_DIR}/projects/web_server/www"

if [ "$(id -u)" -ne 0 ]; then
  echo "请用 root 运行：sudo bash deploy/setup.sh" >&2
  exit 1
fi

if [ ! -x "${BIN}" ]; then
  echo "找不到二进制：${BIN}" >&2
  echo "先编译：cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release && cmake --build build/server -j" >&2
  exit 1
fi

if [ ! -f "${WWW}/index.html" ]; then
  echo "找不到静态站：${WWW}/index.html" >&2
  echo "先构建博客：python3 blog/scripts/build.py" >&2
  exit 1
fi

if ss -lntn 2>/dev/null | grep -qE ":${PORT}([[:space:]]|$)"; then
  echo "端口 ${PORT} 被占用，先停掉手动运行的 web_server" >&2
  exit 1
fi

id "${SERVICE_USER}" >/dev/null 2>&1 || useradd -r -s /usr/sbin/nologin "${SERVICE_USER}"
mkdir -p "${ROOT_DIR}/logs"
chown -R "${SERVICE_USER}:${SERVICE_USER}" "${ROOT_DIR}/logs"
chmod -R a+rX "${WWW}" "${ROOT_DIR}/build/server"
chmod +x "${BIN}"

cat >"/etc/systemd/system/${APP_NAME}.service" <<EOF
[Unit]
Description=SJTWebServer
After=network.target

[Service]
Type=simple
User=${SERVICE_USER}
Group=${SERVICE_USER}
WorkingDirectory=${ROOT_DIR}
ExecStart=${BIN} --host ${HOST} --port ${PORT} --root ${WWW} --threads ${THREADS} --timeout ${TIMEOUT}
Restart=always
RestartSec=3
LimitNOFILE=65535
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now "${APP_NAME}"
systemctl --no-pager status "${APP_NAME}" || true

echo
echo "完成。验证："
echo "  ss -lntp | grep ':${PORT}'"
echo "  curl -I http://127.0.0.1:${PORT}/"
