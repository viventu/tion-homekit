#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_dir/scripts/toolchain.sh"
check_toolchain
"$ARDUINO_CLI" --config-file "$ARDUINO_CLI_CONFIG" compile \
  --fqbn "$tion_fqbn" \
  --libraries "$repo_dir/libraries" \
  --build-path "$repo_dir/.build/read-only" \
  "$repo_dir/firmware/TionReadOnly"
