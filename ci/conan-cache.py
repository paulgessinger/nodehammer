#!/usr/bin/env python3
"""Install locked dependencies, optionally restoring/saving a CI graph archive.

NH_CONAN_CACHE_ARCHIVE is unset for local builds. In CI it names the archive
restored by actions/cache, at a path visible in the current container/host.
"""

import os
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*args):
    subprocess.run(["conan", *map(str, args)], check=True)


def install(project, args):
    archive = os.environ.get("NH_CONAN_CACHE_ARCHIVE")
    if not archive:
        run("install", project, f"--lockfile={Path(project) / 'conan.lock'}", *args)
        return
    archive = Path(archive).resolve()
    restored = archive.is_file()
    if restored:
        run("cache", "restore", archive)
    with tempfile.TemporaryDirectory(prefix="nh-conan-graph-") as directory:
        graph = Path(directory) / "graph.json"
        packages = Path(directory) / "packages.json"
        run("install", project, f"--lockfile={Path(project) / 'conan.lock'}",
            *args, "--format=json", f"--out-file={graph}")
        if not restored:
            run("list", f"--graph={graph}", "--format=json", f"--out-file={packages}")
            archive.parent.mkdir(parents=True, exist_ok=True)
            # Save includes host AND build dependencies from this install only.
            # --no-source excludes extracted sources; build/download trees are
            # already omitted by Conan's archive format.
            temporary = archive.with_name(archive.stem + ".tmp.tgz")
            run("cache", "save", f"--list={packages}", "--no-source", f"--file={temporary}")
            temporary.replace(archive)
        print(f"Conan graph archive: {archive.stat().st_size:,} bytes", flush=True)


if __name__ == "__main__":
    install(sys.argv[1], sys.argv[2:])
