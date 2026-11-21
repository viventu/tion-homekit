#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$repo_dir/scripts/toolchain.sh"
check_toolchain homekit
build_path="$repo_dir/.build/homekit"
# Bash before 4.4 (macOS /bin/bash 3.2) treats an empty array as unset under
# `set -u`; the ${array[@]+...} form below expands to nothing instead.
extra_args=()
if [[ "${TION_DIAGNOSTICS:-0}" != 0 && "${TION_DIAGNOSTICS:-0}" != 1 ]]; then
  echo 'TION_DIAGNOSTICS must be 0 or 1.' >&2
  exit 1
fi
if [[ "${TION_DIAGNOSTICS:-0}" == "1" ]]; then
  build_path="$repo_dir/.build/homekit-diagnostics"
  # The web log serves HTTP from the Arduino loop task, whose default stack is
  # 8192 bytes. The diagnostics build doubles it as headroom; the actual need
  # has not been measured. The normal build keeps the default.
  extra_args=(--build-property 'compiler.cpp.extra_flags=-DTION_ENABLE_WEB_DIAGNOSTICS -DARDUINO_LOOP_STACK_SIZE=16384')
fi
"$ARDUINO_CLI" --config-file "$ARDUINO_CLI_CONFIG" compile \
  --fqbn "$tion_fqbn" \
  --libraries "$HOMESPAN_LIBRARY_DIR" \
  --libraries "$repo_dir/libraries" \
  ${extra_args[@]+"${extra_args[@]}"} \
  --build-path "$build_path" \
  "$repo_dir/firmware/TionHomeKit"
