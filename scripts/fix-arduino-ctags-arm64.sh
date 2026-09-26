#!/usr/bin/env bash
set -euo pipefail

: "${ARDUINO_CLI_DATA_DIR:?Set ARDUINO_CLI_DATA_DIR to the isolated Arduino CLI data directory}"

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
  echo "This workaround applies only to Apple Silicon macOS." >&2
  exit 1
fi

data_dir="$ARDUINO_CLI_DATA_DIR"
packaged="$data_dir/packages/builtin/tools/ctags/5.8-arduino11/ctags"
source_dir="$data_dir/ctags-5.8-arduino11-source"
native_dir="$data_dir/ctags-5.8-arduino11-native"

if "$packaged" --version >/dev/null 2>&1; then
  echo "Packaged ctags already runs; no replacement needed."
  exit 0
fi

git clone --depth 1 --branch 5.8-arduino11 https://github.com/arduino/ctags.git "$source_dir"
git -C "$source_dir" rev-parse HEAD | grep -Fx 'abc8fca7499f44c725122881cd380a88c37abe0e'

(
  cd "$source_dir"
  ./configure --prefix="$native_dir"
  python3 - <<'PY'
from pathlib import Path

# The old macro collides with an Apple SDK attribute spelling.
for path in list(Path('.').glob('*.c')) + list(Path('.').glob('*.h')):
    data = path.read_bytes()
    if b'__unused__' in data:
        path.write_bytes(data.replace(b'__unused__', b'CTAGS_UNUSED'))
PY
  make -j4
  make install
)

mv "$packaged" "$packaged.x86_64"
cp "$native_dir/bin/ctags" "$packaged"
"$packaged" --version | head -2
