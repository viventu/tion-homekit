#!/usr/bin/env python3
"""Build and run the host tests under sanitizers; write compile_commands.json.

Needs only Python 3.9+ and a C++17 compiler whose AddressSanitizer,
UndefinedBehaviorSanitizer and ThreadSanitizer runtimes link. CXX selects the
compiler; otherwise clang++ is tried first, then g++.

--coverage additionally instruments every test and requires every measured
line and branch to run. GCC uses gcov 9+; Clang uses matching llvm-profdata
and llvm-cov (also found through xcrun on macOS).
"""

from __future__ import annotations

import argparse
import collections
import concurrent.futures
import dataclasses
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
CORE = ROOT / "libraries" / "Tion4SCore" / "src"
TESTS = ROOT / "tests"
FIRMWARE = ROOT / "firmware" / "TionHomeKit"
BUILD = ROOT / ".build" / "host"

LIBRARY = [CORE / "tion4s" / f"{name}.cpp" for name in (
    "crc", "protocol", "session", "control", "control_transaction", "describe",
    "uart_core")]

WARNINGS = [
    "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-Wconversion", "-Wsign-conversion",
    "-Wshadow", "-Wold-style-cast", "-Wnon-virtual-dtor", "-Woverloaded-virtual",
    "-Wnull-dereference", "-Wdouble-promotion", "-Wformat=2", "-Wimplicit-fallthrough",
    "-Wcast-qual",
]
BASE_FLAGS = ["-std=c++17", "-g", "-fno-omit-frame-pointer", *WARNINGS]
# Host doubles come first. TION_TEST_REAL_ADAPTER makes the adapter wrapper
# include production code; only its driver/RTOS boundary is then replaced.
INCLUDES = [f"-I{TESTS / 'stubs'}", f"-I{CORE}", f"-I{TESTS}", f"-I{FIRMWARE}",
            f"-I{TESTS / 'platform_stubs'}"]
CONFIGS = {
    "asan": ["-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all"],
    "tsan": ["-O1", "-fsanitize=thread"],
    # Inline and static functions are emitted even when unused, so code that
    # no test calls shows up as uncovered instead of disappearing. The code
    # never throws; without exceptions gcov counts no cleanup-only branches.
    "coverage": ["-O0", "--coverage", "-fprofile-update=atomic", "-fno-exceptions",
                 "-fkeep-inline-functions", "-fkeep-static-functions"],
}
LINK = {"asan": CONFIGS["asan"][1:], "tsan": CONFIGS["tsan"][1:], "coverage": ["--coverage"]}

# Production code, including both sketches and the real ESP32 adapter.
# Driver/RTOS doubles are outside these roots and do not count as coverage.
MEASURED_ROOTS = [CORE, ROOT / "firmware"]
MEASURED_SUFFIXES = {".h", ".cpp", ".ino"}


@dataclasses.dataclass(frozen=True)
class Target:
    name: str
    sources: tuple[Path, ...]
    library: bool = True
    defines: tuple[str, ...] = ()
    config: str = "asan"


SKETCH = TESTS / "sketch.cpp"  # Includes firmware/TionHomeKit/TionHomeKit.ino.
ADAPTER = CORE / "tion4s" / "esp32_uart_service.cpp"
TARGETS = [
    Target("protocol_test", (TESTS / "protocol_test.cpp",)),
    Target("uart_core_test", (TESTS / "uart_core_test.cpp",)),
    Target("control_access_test", (TESTS / "control_access_test.cpp",)),
    Target("homekit_logic_test", (TESTS / "homekit_logic_test.cpp",)),
    Target("pairing_bootstrap_test", (TESTS / "pairing_bootstrap_test.cpp",), library=False),
    Target("firmware_test", (TESTS / "firmware_test.cpp", SKETCH)),
    Target("firmware_diagnostics_test", (TESTS / "firmware_test.cpp", SKETCH),
           defines=("TION_ENABLE_WEB_DIAGNOSTICS",)),
    Target("uart_core_thread_test", (TESTS / "uart_core_thread_test.cpp",), config="tsan"),
    Target("esp32_adapter_test", (TESTS / "esp32_adapter_test.cpp", ADAPTER),
           defines=("TION_TEST_REAL_ADAPTER",)),
    Target("platform_firmware_test", (TESTS / "platform_firmware_test.cpp", ADAPTER),
           defines=("TION_TEST_REAL_ADAPTER", "TION_ENABLE_WEB_DIAGNOSTICS")),
    Target("read_only_test", (TESTS / "read_only_test.cpp", ADAPTER),
           defines=("TION_TEST_REAL_ADAPTER",)),
]


class BuildError(Exception):
    pass


def links(compiler: str) -> bool:
    """True when every sanitizer runtime the tests use actually links."""
    if shutil.which(compiler) is None:
        return False
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / "probe.cpp"
        source.write_text("int main() { return 0; }\n")
        for config in ("asan", "tsan"):
            result = subprocess.run(
                [compiler, *LINK[config], str(source), "-o", str(Path(directory) / "probe")],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            if result.returncode != 0:
                return False
    return True


def choose_compiler() -> str:
    requested = os.environ.get("CXX")
    if requested:
        if not links(requested):
            raise SystemExit(f"CXX={requested} cannot link ASan, UBSan and TSan runtimes.")
        return requested
    for candidate in ("clang++", "g++"):
        if links(candidate):
            return candidate
    raise SystemExit("Need clang++ or g++ with ASan, UBSan and TSan runtimes; set CXX.")


def object_path(config: str, owner: str, source: Path) -> Path:
    return BUILD / config / owner / (source.name + ".o")


def compile_command(compiler: str, config: str, source: Path, defines: tuple[str, ...],
                    output: Path) -> list[str]:
    return [compiler, *BASE_FLAGS, *CONFIGS[config], *INCLUDES,
            *(f"-D{define}" for define in defines), "-MMD", "-c", str(source),
            "-o", str(output)]


def build(compiler: str, configs: list[str]) -> tuple[dict[tuple[str, str], Path], list[dict]]:
    """Compiles every target of `configs` in parallel and links the binaries."""
    jobs = []  # (config, owner, source, defines)
    for config in configs:
        # Every run rebuilds from scratch; stale outputs could only mislead.
        shutil.rmtree(BUILD / config, ignore_errors=True)
        targets = [t for t in TARGETS if config == "coverage" or t.config == config]
        if any(t.library for t in targets):
            jobs += [(config, "lib", source, ()) for source in LIBRARY]
        for target in targets:
            jobs += [(config, target.name, source, target.defines) for source in target.sources]

    # One entry per file and set of defines: clang-tidy then also checks the
    # sketch's diagnostics variant.
    database = []
    variants = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 2) as pool:
        futures = {}
        for config, owner, source, defines in jobs:
            output = object_path(config, owner, source)
            output.parent.mkdir(parents=True, exist_ok=True)
            command = compile_command(compiler, config, source, defines, output)
            futures[pool.submit(subprocess.run, command)] = (config, owner, source)
            if config != "coverage" and (source, defines) not in variants:
                variants.add((source, defines))
                database.append({"directory": str(ROOT), "file": str(source),
                                 "arguments": command})
        failed = [f"{config}/{owner}: {source.relative_to(ROOT)}"
                  for future, (config, owner, source) in futures.items()
                  if future.result().returncode != 0]
    if failed:
        raise BuildError("Compilation failed: " + ", ".join(sorted(failed)))

    binaries = {}
    for config in configs:
        for target in TARGETS:
            if config != "coverage" and target.config != config:
                continue
            inputs = [object_path(config, target.name, s) for s in target.sources]
            if target.library:
                inputs += [object_path(config, "lib", s) for s in LIBRARY]
            binary = BUILD / config / target.name / target.name
            linked = subprocess.run([compiler, *LINK[config], *map(str, inputs), "-o",
                                     str(binary), "-lpthread"])
            if linked.returncode != 0:
                raise BuildError(f"Link failed: {config}/{target.name}")
            binaries[(config, target.name)] = binary
    return binaries, database


def run_tests(binaries: dict[tuple[str, str], Path], config: str) -> bool:
    environment = dict(os.environ)
    if config == "coverage":
        environment["LLVM_PROFILE_FILE"] = str(BUILD / config / "%p-%m.profraw")
    for (binary_config, name), binary in binaries.items():
        if binary_config == config and subprocess.run([str(binary)], env=environment).returncode != 0:
            print(f"FAILED: {config}/{name}", file=sys.stderr)
            return False
    return True


def measured_sources() -> set[Path]:
    return {path for root in MEASURED_ROOTS for path in root.rglob("*")
            if path.suffix in MEASURED_SUFFIXES}


def included_sources(directory: Path) -> set[Path]:
    """Every source file the coverage build compiled or included."""
    seen = set()
    for depfile in directory.rglob("*.d"):
        text = depfile.read_text().replace("\\\n", " ")
        for token in text.split(":", 1)[1].split():
            seen.add(Path(os.path.normpath(ROOT / token)))
    return seen


def gcc_coverage_gate() -> bool:
    directory = BUILD / "coverage"
    lines: dict[Path, collections.Counter] = collections.defaultdict(collections.Counter)
    branches: dict[Path, collections.Counter] = collections.defaultdict(collections.Counter)
    for gcda in sorted(directory.rglob("*.gcda")):
        report = subprocess.run(["gcov", "--json-format", "--stdout", "--branch-probabilities",
                                 str(gcda)], cwd=gcda.parent, capture_output=True, text=True)
        if report.returncode != 0 or not report.stdout.startswith("{"):
            print(f"gcov failed for {gcda}; --coverage needs GCC's gcov 9 or newer.",
                  file=sys.stderr)
            return False
        for document in report.stdout.splitlines():
            for entry in json.loads(document)["files"]:
                path = Path(os.path.normpath(gcda.parent / entry["file"]))
                # Several instantiations or translation units may report the same
                # line; a line or branch counts as covered when any of them ran it.
                for line in entry["lines"]:
                    number = line["line_number"]
                    lines[path][number] += line["count"]
                    for index, branch in enumerate(line.get("branches", [])):
                        if not branch.get("throw"):
                            branches[path][(number, index)] += branch["count"]

    expected = measured_sources()
    missing = sorted(expected - included_sources(directory) - set(lines))
    ok = not missing
    for path in missing:
        print(f"coverage: {path.relative_to(ROOT)} is not built by any host test",
              file=sys.stderr)
    total_lines = total_branches = covered_lines = covered_branches = 0
    for path in sorted(expected & set(lines)):
        line_counts = lines[path]
        branch_counts = branches[path]
        uncovered = sorted(n for n, count in line_counts.items() if count == 0)
        partial = sorted({n for (n, _), count in branch_counts.items() if count == 0})
        total_lines += len(line_counts)
        covered_lines += len(line_counts) - len(uncovered)
        total_branches += len(branch_counts)
        covered_branches += sum(1 for count in branch_counts.values() if count)
        if uncovered or partial:
            ok = False
            relative = path.relative_to(ROOT)
            if uncovered:
                print(f"coverage: {relative}: lines never run: {uncovered}", file=sys.stderr)
            if partial:
                print(f"coverage: {relative}: branches not taken on lines: {partial}",
                      file=sys.stderr)
    print(f"Coverage: lines {covered_lines}/{total_lines}, "
          f"branches {covered_branches}/{total_branches}", flush=True)
    return ok


def llvm_tool(name: str) -> str:
    path = shutil.which(name)
    if path:
        return path
    if shutil.which("xcrun"):
        found = subprocess.run(["xcrun", "--find", name], capture_output=True, text=True)
        if found.returncode == 0:
            return found.stdout.strip()
    raise SystemExit(f"Clang coverage needs {name} matching the compiler.")


def clang_coverage_gate(binaries: dict[tuple[str, str], Path]) -> bool:
    directory = BUILD / "coverage"
    profile = directory / "merged.profdata"
    subprocess.run([llvm_tool("llvm-profdata"), "merge", "-sparse",
                    *map(str, sorted(directory.glob("*.profraw"))), "-o", str(profile)],
                   check=True)
    objects = list(binaries.values())
    report = subprocess.run(
        [llvm_tool("llvm-cov"), "export", str(objects[0]),
         *(f"--object={obj}" for obj in objects[1:]), f"--instr-profile={profile}"],
        check=True, capture_output=True, text=True)
    (directory / "coverage.json").write_text(report.stdout)
    expected = measured_sources()
    files = {Path(entry["filename"]).resolve(): entry
             for document in json.loads(report.stdout)["data"] for entry in document["files"]}
    missing = sorted(expected - included_sources(directory) - set(files))
    ok = not missing
    for path in missing:
        print(f"coverage: {path.relative_to(ROOT)} is not built by any host test", file=sys.stderr)
    totals = {kind: collections.Counter() for kind in ("lines", "branches")}
    for path in sorted(expected & set(files)):
        entry = files[path]
        for kind, total in totals.items():
            summary = entry["summary"][kind]
            total.update({key: summary[key] for key in ("count", "covered")})
            if summary["covered"] != summary["count"]:
                ok = False
                print(f"coverage: {path.relative_to(ROOT)}: {kind} "
                      f"{summary['covered']}/{summary['count']}", file=sys.stderr)
        missed = sorted({branch[0] for branch in entry["branches"]
                         if branch[4] == 0 or branch[5] == 0})
        if missed and entry["summary"]["branches"]["notcovered"]:
            print(f"coverage: branches not taken on lines: {missed}", file=sys.stderr)
        missed_lines = sorted({segment[0] for segment in entry["segments"]
                               if segment[2] == 0 and segment[3] and segment[4]})
        if missed_lines:
            print(f"coverage: uncovered region starts: {missed_lines}", file=sys.stderr)
    print("Coverage: " + ", ".join(f"{kind} {total['covered']}/{total['count']}"
                                   for kind, total in totals.items()), flush=True)
    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--coverage", action="store_true",
                        help="also require 100%% line and branch coverage (GCC or Clang)")
    arguments = parser.parse_args()

    compiler = choose_compiler()
    print(f"Host compiler: {compiler}", flush=True)
    configs = ["asan", "tsan"]
    clang = "clang" in subprocess.run([compiler, "--version"], capture_output=True,
                                      text=True, check=True).stdout.lower()
    if arguments.coverage and clang:
        CONFIGS["coverage"] = ["-O0", "-fprofile-instr-generate", "-fcoverage-mapping",
                               "-fno-exceptions", "-femit-all-decls"]
        LINK["coverage"] = ["-fprofile-instr-generate"]
        llvm_tool("llvm-cov")
        llvm_tool("llvm-profdata")
    elif arguments.coverage and shutil.which("gcov") is None:
        raise SystemExit("GCC coverage needs matching gcov 9+.")
    try:
        binaries, database = build(compiler, configs)
        if arguments.coverage:
            coverage_binaries, _ = build(compiler, ["coverage"])
    except BuildError as error:
        print(error, file=sys.stderr)
        return 1
    database.sort(key=lambda entry: entry["file"])
    (BUILD / "compile_commands.json").write_text(json.dumps(database, indent=2) + "\n")

    if not run_tests(binaries, "asan") or not run_tests(binaries, "tsan"):
        return 1
    if arguments.coverage:
        # Output of the instrumented rerun adds nothing to the sanitizer run.
        with open(os.devnull, "w", encoding="utf-8") as quiet:
            saved = os.dup(1)
            os.dup2(quiet.fileno(), 1)
            try:
                passed = run_tests(coverage_binaries, "coverage")
            finally:
                os.dup2(saved, 1)
                os.close(saved)
        if not passed:
            return 1
        if not (clang_coverage_gate(coverage_binaries) if clang else gcc_coverage_gate()):
            return 1

    return subprocess.run([sys.executable, "-m", "unittest", "discover", "-s", str(TESTS),
                           "-p", "test_*.py"]).returncode


if __name__ == "__main__":
    raise SystemExit(main())
