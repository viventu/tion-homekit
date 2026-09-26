"""Exercise the publication boundary with synthetic repositories."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/prepare-public-snapshot.py"
SPEC = importlib.util.spec_from_file_location("public_snapshot", SCRIPT)
snapshot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(snapshot)


class PublicSnapshotTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "source"
        self.root.mkdir()
        self.git("init", "-q", "-b", "main")
        (self.root / "README.md").write_text("Public project\n")
        self.commit()

    def git(self, *args: str) -> None:
        subprocess.run(["git", *args], cwd=self.root, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)

    def commit(self) -> None:
        self.git("add", ".")
        self.git("-c", "user.name=Test", "-c", "user.email=test@example.invalid",
                 "commit", "-qm", "test")

    def test_snapshot_excludes_git_history(self) -> None:
        commit, entries = snapshot.scan_tree(self.root)
        output = Path(self.temp.name) / "publication"
        snapshot.write_snapshot(self.root, output, entries)
        self.assertEqual((output / "README.md").read_text(), "Public project\n")
        self.assertFalse((output / ".git").exists())
        self.assertEqual(len(commit), 40)

    def test_rejects_private_identifier_without_printing_value(self) -> None:
        identifier = ":".join(("AA", "BB", "CC", "DD", "EE", "FF"))
        (self.root / "README.md").write_text(f"device {identifier}\n")
        self.commit()
        with self.assertRaisesRegex(snapshot.PublicationError, "possible hardware address") as caught:
            snapshot.scan_tree(self.root)
        self.assertNotIn(identifier, str(caught.exception))

    def test_rejects_dirty_checkout_and_private_path(self) -> None:
        (self.root / "README.md").write_text("uncommitted\n")
        with self.assertRaisesRegex(snapshot.PublicationError, "not clean"):
            snapshot.scan_tree(self.root)
        self.git("checkout", "--", "README.md")
        (self.root / "secrets.txt").write_text("private data\n")
        self.commit()
        with self.assertRaisesRegex(snapshot.PublicationError, "private or unsafe path"):
            snapshot.scan_tree(self.root)


if __name__ == "__main__":
    unittest.main()
