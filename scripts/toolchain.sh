#!/usr/bin/env bash
# Shared build inputs. Source this file; it performs no installation or upload.

readonly tion_fqbn='esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=disabled,CDCOnBoot=default,USBMode=hwcdc'

check_toolchain() {
  : "${ARDUINO_CLI:?Set ARDUINO_CLI to Arduino CLI 1.5.1}"
  : "${ARDUINO_CLI_CONFIG:?Set ARDUINO_CLI_CONFIG to an isolated config file}"
  "$ARDUINO_CLI" --config-file "$ARDUINO_CLI_CONFIG" version |
    grep -E 'Version: 1\.5\.1[[:space:]]+Commit: 01f3d4f2b[[:space:]]' || return 1
  "$ARDUINO_CLI" --config-file "$ARDUINO_CLI_CONFIG" core list |
    grep -E '^esp32:esp32[[:space:]]+3\.3\.8[[:space:]]' || return 1

  if [[ "${1:-}" == homekit ]]; then
    : "${HOMESPAN_LIBRARY_DIR:?Set HOMESPAN_LIBRARY_DIR to a directory containing HomeSpan}"
    local library="$HOMESPAN_LIBRARY_DIR/HomeSpan"
    grep -Fx 'version=2.1.8' "$library/library.properties" || return 1
    git -C "$library" rev-parse HEAD |
      grep -Fx '107ffc07f4455754ea89068d8cf2e992de3583e6' || return 1
    if ! git -C "$library" diff --quiet HEAD -- ||
       [[ -n "$(git -C "$library" ls-files --others)" ]]; then
      echo 'HomeSpan checkout must be clean, with no extra files (including ignored files).' >&2
      return 1
    fi
  fi
}
