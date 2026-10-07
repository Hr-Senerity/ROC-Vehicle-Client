import importlib.util
import pathlib
import tempfile
import unittest
import zipfile

spec = importlib.util.spec_from_file_location(
    "candidate_export", pathlib.Path(__file__).parents[2] / "tools/source-candidate-export.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Whitelist(unittest.TestCase):
    def test_allowed_source_and_license(self):
        for name in ("LICENSE", "src/version.cpp", "src/build_version.hpp.in",
                     "README.md", "NOTICE", "docker/dev/Dockerfile"):
            with self.subTest(name=name):
                self.assertTrue(module.allowed(name, 0o100644))

    def test_private_binary_and_link_rejected(self):
        for name in ("out/receipt.json", "Connect/reply.md", "runtime-private/state.sqlite3",
                     ".git/config", "docs/.env", "tools/device-token", "tools/device-token.json", "docs/a.exe",
                     "docs/a.tar", "src/state.sqlite3", "../src/version.cpp",
                     "/src/version.cpp", "src\\version.cpp", "docs/out/a.md", "contracts/a.json",
                     "src/docs/a.json", "tools/private-config.json", "src/a.key", "CHANGELOG.md", "SECURITY.md",
                     "./src/version.cpp", "src//version.cpp"):
            with self.subTest(name=name):
                self.assertFalse(module.allowed(name, 0o100644))
        self.assertFalse(module.allowed("src/link.cpp", 0o120777))

    def test_review_export_omits_private_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory) / "source"
            root.mkdir()
            for name in ("LICENSE", "README.md", "CMakeLists.txt"):
                (root / name).write_text("synthetic", encoding="utf-8")
            (root / "docs").mkdir()
            (root / "docs" / "private.json").write_text("synthetic-private", encoding="utf-8")
            (root / "tools" / "__pycache__").mkdir(parents=True)
            (root / "tools" / "__pycache__" / "generated.pyc").write_bytes(b"synthetic-generated-cache")
            result = module.export_worktree(root, pathlib.Path(directory) / "output")
            self.assertIsNone(result["source_revision"])
            self.assertFalse(result["private_contracts_included"])
            self.assertEqual(result["source_files"], 3)

    def test_archive_rejects_nested_private_file(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / "bad.zip"
            with zipfile.ZipFile(archive, "w") as z:
                z.writestr(module.PREFIX + "docs/private.json", "synthetic")
            with self.assertRaises(ValueError):
                module.inspect_archive(archive)


if __name__ == "__main__":
    unittest.main()
