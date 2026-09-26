#!/usr/bin/env bash
# Host tests: C++ under ASan/UBSan and TSan, then the Python tests. The build
# also writes .build/host/compile_commands.json for clang-tidy and clangd.
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$repo_dir/scripts/host_tests.py" "$@"
