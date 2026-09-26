#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) platform=macOS_ARM64 ;;
  Darwin-x86_64) platform=macOS_64bit ;;
  Linux-aarch64|Linux-arm64) platform=Linux_ARM64 ;;
  Linux-x86_64) platform=Linux_64bit ;;
  *) echo 'Supported bootstrap hosts: macOS/Linux, arm64/x86_64.' >&2; exit 1 ;;
esac

toolchain_dir="${TION_TOOLCHAIN_DIR:-$repo_dir/.arduino}"
if [[ -e "$toolchain_dir" ]]; then
  echo 'Toolchain destination already exists; choose a new TION_TOOLCHAIN_DIR.' >&2
  exit 1
fi
mkdir -p "$toolchain_dir"
toolchain_dir="$(cd "$toolchain_dir" && pwd)"
mkdir -p "$toolchain_dir/bin" "$toolchain_dir/data" "$toolchain_dir/downloads" \
  "$toolchain_dir/user" "$toolchain_dir/libraries"
archive="arduino-cli_1.5.1_${platform}.tar.gz"
curl --fail --location --retry 3 --output "$toolchain_dir/$archive" \
  "https://github.com/arduino/arduino-cli/releases/download/v1.5.1/$archive"
python3 - "$toolchain_dir" "$archive" "$repo_dir/third_party/arduino-cli-1.5.1.sha256" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root, archive, manifest = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
hashes = dict(line.split()[::-1] for line in manifest.read_text().splitlines())
if hashlib.sha256((root / archive).read_bytes()).hexdigest() != hashes[archive]:
    raise SystemExit("Arduino CLI archive checksum mismatch")
directories = {key: str(root / key) for key in ("data", "user", "downloads")}
config = "directories:\n" + "".join(f"  {key}: {json.dumps(value)}\n" for key, value in directories.items())
config += "board_manager:\n  additional_urls:\n    - https://espressif.github.io/arduino-esp32/package_esp32_index.json\n"
(root / "arduino-cli.yaml").write_text(config)
PY
tar -xzf "$toolchain_dir/$archive" -C "$toolchain_dir/bin" arduino-cli
cli="$toolchain_dir/bin/arduino-cli"
config="$toolchain_dir/arduino-cli.yaml"
"$cli" --config-file "$config" core update-index
"$cli" --config-file "$config" core install esp32:esp32@3.3.8
git clone --depth 1 --branch 2.1.8 https://github.com/HomeSpan/HomeSpan.git \
  "$toolchain_dir/libraries/HomeSpan"
git -C "$toolchain_dir/libraries/HomeSpan" rev-parse HEAD |
  grep -Fx '107ffc07f4455754ea89068d8cf2e992de3583e6'
if [[ "$platform" == macOS_ARM64 ]]; then
  ARDUINO_CLI_DATA_DIR="$toolchain_dir/data" bash "$repo_dir/scripts/fix-arduino-ctags-arm64.sh"
fi
echo "Toolchain installed in $toolchain_dir. Run scripts/check.sh with the paths documented in README."
