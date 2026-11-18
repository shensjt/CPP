#!/usr/bin/env bash
set -euo pipefail

HOST="${HOST:-deploy@39.106.115.254}"
REMOTE="${REMOTE:-/opt/sjtweb-runtime}"
SITE="build/www"
BASE="${BASE:-http://127.0.0.1:8080}"
MKDOCS="${MKDOCS:-mkdocs}"
SSH_KEY="${SSH_KEY:-}"

SSH_CMD=(ssh)
RSYNC_SSH=()
if [ -n "$SSH_KEY" ]; then
  SSH_CMD=(ssh -i "$SSH_KEY")
  RSYNC_SSH=(-e "ssh -i $SSH_KEY")
fi

echo "==> build"
"$MKDOCS" build -f blog/mkdocs.yml --strict

echo "==> guard"
test -s build/www/index.html

echo "==> snapshot"
"${SSH_CMD[@]}" "$HOST" "set -e; cd '$REMOTE'; mkdir -p www; rm -rf www.prev; cp -al www www.prev"

echo "==> rsync"
rsync -az "${RSYNC_SSH[@]}" --delete-after --delay-updates --chmod=D755,F644 \
  "$SITE/" "$HOST:$REMOTE/www/"

echo "==> smoke"
if ! "${SSH_CMD[@]}" "$HOST" "set -e; curl -fsS '$BASE/' >/dev/null; curl -fsS '$BASE/sitemap.xml' >/dev/null"; then
  if "${SSH_CMD[@]}" "$HOST" "test -n \"\$(ls -A '$REMOTE/www.prev' 2>/dev/null)\""; then
    echo "==> rollback"
    "${SSH_CMD[@]}" "$HOST" "set -e; cd '$REMOTE'; rsync -a --delete www.prev/ www/"
    "${SSH_CMD[@]}" "$HOST" "curl -fsS '$BASE/' >/dev/null" \
      || echo "rollback smoke failed, manual intervention needed" >&2
  else
    echo "no snapshot to roll back to" >&2
  fi
  echo "publish failed" >&2
  exit 1
fi

echo "==> done"
