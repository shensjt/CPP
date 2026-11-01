#!/usr/bin/env bash
set -euo pipefail

# 用法：
#   SSH_TARGET=webserver@1.2.3.4 ./deploy/deploy.sh
# 可选：
#   REMOTE_ROOT=/opt/sjtwebserver
#   APP_NAME=sjtwebserver

SSH_TARGET=${SSH_TARGET:?set SSH_TARGET=user@host}
REMOTE_ROOT=${REMOTE_ROOT:-/opt/sjtwebserver}
APP_NAME=${APP_NAME:-sjtwebserver}

python3 blog/scripts/build.py

cmake -S . -B build/server -DCMAKE_BUILD_TYPE=Release
cmake --build build/server -j

rsync -avz --delete projects/web_server/www/ "${SSH_TARGET}:${REMOTE_ROOT}/www/"
rsync -avz build/server/web_server "${SSH_TARGET}:${REMOTE_ROOT}/bin/web_server.new"

ssh "${SSH_TARGET}" "
  set -e
  cd '${REMOTE_ROOT}'
  if [ -f bin/web_server ]; then
    mv bin/web_server bin/web_server.prev
  fi
  mv bin/web_server.new bin/web_server
  chmod +x bin/web_server
  sudo systemctl restart '${APP_NAME}'
"

echo "deployed to ${SSH_TARGET}:${REMOTE_ROOT}"
