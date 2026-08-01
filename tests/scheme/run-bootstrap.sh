#!/usr/bin/env bash
# Run Chaaya bootstrap Scheme suites (Kaappi-compatible checks without import/SRFI-64).
# Usage: bash tests/scheme/run-bootstrap.sh [path-to-chaaya]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHAAYA="${1:-$ROOT/build/chaaya}"
if [[ ! -x "$CHAAYA" ]]; then
  echo "chaaya binary not found: $CHAAYA (build first)" >&2
  exit 1
fi

HARNESS="$ROOT/tests/scheme/bootstrap/harness.scm"
failed=0
ran=0

for f in "$ROOT"/tests/scheme/bootstrap/*.scm; do
  base="$(basename "$f")"
  [[ "$base" == "harness.scm" ]] && continue
  ran=$((ran + 1))
  tmp="$(mktemp -t chaaya-bootstrap.XXXXXX.scm)"
  cat "$HARNESS" "$f" >"$tmp"
  echo "==> $base"
  if ! "$CHAAYA" "$tmp"; then
    echo "FAIL: $base" >&2
    failed=$((failed + 1))
  fi
  rm -f "$tmp"
done

echo "$ran suites, $failed failed"
exit "$failed"
