#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_dir/scripts/toolchain.sh"
check_toolchain homekit
# CI runs host tests in a separate job before compiling.
if [[ "${TION_SKIP_HOST_TESTS:-0}" != 1 ]]; then
  bash "$repo_dir/scripts/test-host.sh"
fi
"$ARDUINO_CLI" --config-file "$ARDUINO_CLI_CONFIG" compile \
  --fqbn "$tion_fqbn" \
  --build-property 'compiler.cpp.extra_flags=-DTION_COMPILE_SPIKE_ONLY' \
  --libraries "$HOMESPAN_LIBRARY_DIR" \
  --build-path "$repo_dir/.build/compile-only" \
  "$repo_dir/spike/CompileOnly"

# The portable library must also build for a chip without the S3 adapter.
"$ARDUINO_CLI" --config-file "$ARDUINO_CLI_CONFIG" compile \
  --fqbn esp32:esp32:esp32 \
  --libraries "$repo_dir/libraries" \
  --build-path "$repo_dir/.build/portable-only" \
  "$repo_dir/spike/PortableOnly"

bash "$repo_dir/scripts/build-read-only.sh"
bash "$repo_dir/scripts/build-homekit.sh"
TION_DIAGNOSTICS=1 bash "$repo_dir/scripts/build-homekit.sh"
