#!/usr/bin/env python3
"""Offline, exact-commit source candidate; never a publication command."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import zipfile

ROOT_FILES = {"CMakeLists.txt", "CMakePresets.json", "README.md", "NOTICE", "LICENSE", ".gitignore",
              ".gitattributes", ".dockerignore", "compose.yaml"}
ROOT_DIRS = {"apps", "cmake", "docker", "examples",
             "include", "src", "tests", "tools"}
TEXT_SUFFIXES = {".cpp", ".hpp", ".h", ".in", ".cmake", ".json",
                 ".txt", ".yaml", ".yml", ".sh", ".ps1", ".py"}


def allowed(path, mode):
    parts = pathlib.PurePosixPath(path).parts
    if not parts or path.startswith("/") or "\\" in path or ".." in parts or pathlib.PurePosixPath(path).as_posix() != path:
        return False
    if mode and mode & 0o170000 != 0o100000:
        return False
    if len(parts) == 1:
        return path in ROOT_FILES
    if parts[0] not in ROOT_DIRS or any(p in {"docs", "contracts", "out", "Connect", "runtime-private", ".git"} for p in parts):
        return False
    name = parts[-1]
    if name.startswith((".env", "device-token", "private-config")) or name == "driver-config.json":
        return False
    return pathlib.PurePosixPath(name).suffix in TEXT_SUFFIXES or name in {"Dockerfile", "SHA256SUMS"}


PREFIX = "ROC-Vehicle-Client-0.2.1-rc2/"


def inspect_archive(archive):
    files = []
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            if not info.filename.startswith(PREFIX):
                raise ValueError("unexpected archive prefix")
            path = info.filename[len(PREFIX):]
            if not allowed(path, info.external_attr >> 16):
                raise ValueError("non-whitelisted source member")
            data = z.read(info)
            data.decode("utf-8")  # No unexplained binaries in this source proposal.
            files.append({"path": path, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
        if len({f["path"] for f in files}) != len(files):
            raise ValueError("duplicate source member")
        if not {"README.md", "LICENSE"}.issubset({f["path"] for f in files}):
            raise ValueError("public README and project license required")
    return sorted(files, key=lambda f: f["path"])


def record(target, archive, revision):
    files = inspect_archive(archive)
    result = {"source_revision": revision, "software_version": "0.2.1-rc2",
              "archive": archive.name, "bytes": archive.stat().st_size,
              "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
              "source_files": len(files), "private_contracts_included": False,
              "source_tree_sha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
              "project_license_present": True,
              "publication_authorized": False, "candidate_only": True, "files": files}
    manifest = target / "source-manifest.json"
    manifest.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (target / "SHA256SUMS").write_text(result["sha256"] + "  " + archive.name + "\n", encoding="utf-8")
    return result


def export_worktree(source, target):
    source = pathlib.Path(source).resolve()
    target = pathlib.Path(target).resolve()
    if source == target or source in target.parents:
        raise ValueError("output must be outside source tree")
    target.mkdir(exist_ok=False)
    archive = target / "roc-vehicle-client-0.2.1-rc2-review.zip"
    paths = [source / name for name in ROOT_FILES if (source / name).exists()]
    for name in sorted(ROOT_DIRS):
        folder = source / name
        if folder.is_symlink():
            raise ValueError("source directory link refused")
        if folder.exists():
            paths.extend(p for p in folder.rglob("*") if
                         (p.is_file() or p.is_symlink()) and "__pycache__" not in p.parts and p.suffix != ".pyc")
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for path in sorted(paths):
            relative = path.relative_to(source).as_posix()
            if path.is_symlink() or not allowed(relative, 0o100644):
                raise ValueError("non-whitelisted working source: " + relative)
            path.read_bytes().decode("utf-8")
            z.write(path, PREFIX + relative)
    # A review of a working tree is never presented as an exact committed build.
    return record(target, archive, None)


def main():
    if len(sys.argv) == 5 and sys.argv[1] == "--worktree" and sys.argv[3] == "--output":
        result = export_worktree(sys.argv[2], sys.argv[4])
    else:
        if len(sys.argv) != 2 or not re.fullmatch(r"[0-9a-f]{40}", sys.argv[1]):
            raise ValueError("exact SHA or --worktree SOURCE --output TARGET required")
        revision = sys.argv[1]
        git = ["git", "-c", "safe.directory=/workspace", "-C", "/workspace"]
        if subprocess.check_output(git + ["rev-parse", revision + "^{commit}"]).decode().strip() != revision:
            raise ValueError("source identity mismatch")
        target = pathlib.Path("/build/rc2-source-" + revision[:12])
        target.mkdir(exist_ok=False)
        archive = target / ("roc-vehicle-client-v0.2.1-rc2-source-candidate-" + revision[:12] + ".zip")
        subprocess.run(git + ["archive", "--format=zip", "--prefix=" + PREFIX, "--output=" + str(archive), revision], check=True)
        result = record(target, archive, revision)
    print(json.dumps({k: v for k, v in result.items() if k != "files"}))


if __name__ == "__main__":
    main()
