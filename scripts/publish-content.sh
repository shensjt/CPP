#!/usr/bin/env bash
set -euo pipefail

HOST="${HOST:-deploy@39.106.115.254}"
REMOTE="${REMOTE:-/opt/sjtweb-runtime}"
SITE="build/www"
BASE="${BASE:-http://127.0.0.1:8080}"
MKDOCS="${MKDOCS:-mkdocs}"

echo "==> build"
"$MKDOCS" build -f blog/mkdocs.yml --strict

echo "==> guard"
test -s build/www/index.html

echo "==> snapshot"
ssh "$HOST" "set -e; cd '$REMOTE'; mkdir -p www; rm -rf www.prev; cp -al www www.prev"

echo "==> rsync"
rsync -az --delete-after --delay-updates --chmod=D755,F644 \
  "$SITE/" "$HOST:$REMOTE/www/"

echo "==> smoke"
if ! ssh "$HOST" "set -e; curl -fsS '$BASE/' >/dev/null; curl -fsS '$BASE/sitemap.xml' >/dev/null"; then
  if ssh "$HOST" "test -n \"\$(ls -A '$REMOTE/www.prev' 2>/dev/null)\""; then
    echo "==> rollback"
    ssh "$HOST" "set -e; cd '$REMOTE'; rsync -a --delete www.prev/ www/"
    ssh "$HOST" "curl -fsS '$BASE/' >/dev/null" \
      || echo "rollback smoke failed, manual intervention needed" >&2
  else
    echo "no snapshot to roll back to" >&2
  fi
  echo "publish failed" >&2
  exit 1
fi

echo "==> done"
