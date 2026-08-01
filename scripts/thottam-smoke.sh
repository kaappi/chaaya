#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kaappi_root="${KAAPPI_ROOT:-"${repo_root}/../kaappi"}"

chaaya_bin="${repo_root}/build/chaaya"
if [[ ! -x "${chaaya_bin}" ]]; then
  echo "SKIP: chaaya binary not found at ${chaaya_bin}; run 'make' first."
  exit 0
fi

if [[ -x "${kaappi_root}/zig-out/bin/thottam" ]]; then
  thottam_cmd=("${kaappi_root}/zig-out/bin/thottam")
elif [[ -x "${kaappi_root}/zig-out/bin/kaappi" ]]; then
  thottam_cmd=("${kaappi_root}/zig-out/bin/kaappi" thottam)
elif command -v thottam >/dev/null 2>&1; then
  thottam_cmd=("$(command -v thottam)")
elif command -v kaappi >/dev/null 2>&1; then
  thottam_cmd=("$(command -v kaappi)" thottam)
else
  echo "SKIP: no thottam command found. Build ../kaappi first (zig build)."
  exit 0
fi

if ! "${thottam_cmd[@]}" --help >/dev/null 2>&1; then
  echo "SKIP: found thottam command but '--help' failed: ${thottam_cmd[*]}"
  exit 0
fi

echo "OK: thottam command available: ${thottam_cmd[*]}"
echo "OK: chaaya binary available: ${chaaya_bin}"
echo "Next: install package libs with thottam, then run chaaya with --lib-path."
