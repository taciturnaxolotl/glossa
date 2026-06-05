#!/usr/bin/env bash
set -euo pipefail

if [ $# -eq 0 ]; then
  echo "usage: $(basename "$0") <text>" >&2
  exit 1
fi

TEXT="$*"
TMP="/tmp/grimoire_test.json"

.venv/bin/python3 grimoire.py --json "$TMP" "$TEXT"
scp "$TMP" remarkable:/tmp/grimoire_strokes.json
ssh -o ConnectTimeout=10 remarkable '/home/root/uinject /tmp/grimoire_strokes.json'
