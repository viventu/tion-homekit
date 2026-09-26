#!/usr/bin/env python3
"""Check a committed tree and optionally copy it without private Git history."""

from __future__ import annotations

import argparse
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


MAX_FILE_BYTES = 2 * 1024 * 1024
PRIVATE_DIRECTORIES = {".arduino", ".build", ".git", ".private"}
PRIVATE_SUFFIXES = {".bin", ".dump", ".elf", ".map", ".nvs", ".pcap", ".pcapng", ".uf2"}
PRIVATE_PATTERNS = {
    "hardware address": re.compile(rb"(?<![0-9A-Fa-f])(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}(?![0-9A-Fa-f])"),
    "device hostname": re.compile(rb"(?i)\btion-(?:4s|setup)-[0-9a-f]{6}(?:\.local)?\b"),
    "private key": re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    "GitHub token": re.compile(rb"(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})"),
    "OpenAI token": re.compile(rb"sk-[A-Za-z0-9_-]{20,}"),
    "AWS access key": re.compile(rb"AKIA[0-9A-Z]{16}"),
    "home directory": re.compile(rb"/(?:Users|home)/[A-Za-z0-9._-]+/"),
}


class PublicationError(Exception):
    """A public snapshot cannot be prepared safely."""


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=root, check=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    return result.stdout


def scan_tree(root: Path) -> tuple[str, list[tuple[PurePosixPath, bytes, int]]]:
    if git(root, "status", "--porcelain", "--untracked-files=all"):
        raise PublicationError("working tree is not clean")

    commit = git(root, "rev-parse", "--verify", "HEAD^{commit}").decode().strip()
    entries = []
    problems = []
    for record in git(root, "ls-tree", "-r", "-z", "--full-tree", commit).split(b"\0"):
        if not record:
            continue
        metadata, raw_name = record.split(b"\t", 1)
        mode, kind, object_id = metadata.decode("ascii").split()
        try:
            name = raw_name.decode("utf-8")
        except UnicodeDecodeError:
            problems.append("non-UTF-8 filename")
            continue
        path = PurePosixPath(name)
        if (path.is_absolute() or ".." in path.parts or
                any(part in PRIVATE_DIRECTORIES for part in path.parts) or
                path.name.startswith("secrets.") or path.name == ".env" or
                path.name.startswith(".env.") or path.suffix.lower() in PRIVATE_SUFFIXES):
            problems.append(f"{name}: private or unsafe path")
            continue
        if kind != "blob" or mode not in {"100644", "100755"}:
            problems.append(f"{name}: unsupported Git entry")
            continue
        if int(git(root, "cat-file", "-s", object_id)) > MAX_FILE_BYTES:
            problems.append(f"{name}: file exceeds 2 MiB")
            continue
        data = git(root, "cat-file", "blob", object_id)
        if b"\0" in data:
            problems.append(f"{name}: binary file")
            continue
        for label, pattern in PRIVATE_PATTERNS.items():
            if pattern.search(data):
                problems.append(f"{name}: possible {label}")
        entries.append((path, data, 0o755 if mode == "100755" else 0o644))

    if problems:
        raise PublicationError("\n".join(problems))
    return commit, entries


def write_snapshot(root: Path, output: Path,
                   entries: list[tuple[PurePosixPath, bytes, int]]) -> None:
    output = output.resolve()
    if output == root or root in output.parents:
        raise PublicationError("output directory must be outside the repository")
    if output.exists():
        raise PublicationError("output directory already exists")
    output.mkdir(parents=True)
    for path, data, mode in entries:
        target = output.joinpath(*path.parts)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        os.chmod(target, mode)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent,
                        help="repository to inspect (defaults to this project)")
    parser.add_argument("--output", type=Path, help="new directory for a history-free snapshot")
    args = parser.parse_args()
    try:
        root = args.repo.resolve()
        if Path(git(root, "rev-parse", "--show-toplevel").decode().strip()) != root:
            raise PublicationError("--repo must be the repository root")
        commit, entries = scan_tree(root)
        if args.output:
            write_snapshot(root, args.output, entries)
    except (PublicationError, OSError, subprocess.CalledProcessError) as exc:
        print(f"Publication check failed: {exc}", file=sys.stderr)
        return 1
    print(f"Public tree OK: {len(entries)} files from {commit}")
    if args.output:
        print(f"History-free snapshot: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
