#!/usr/bin/env python3
"""Regenerate the multi-platform dependency lock; requires Conan 2.30.0."""

import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run(["conan", *args], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--update", action="store_true", help="Refresh upstream recipe revisions")
    args = parser.parse_args()
    update = ["--update"] if args.update else []
    # Export exactly the recipes used by CI before resolving their revisions.
    for name, version in [
        ("cli11", "2.7.2"), ("sokol", "2026.07.02"), ("imgui", "1.92.8"),
        ("implot", "1.0.0"), ("nfd", "1.3.0"), ("sokol-shdc", "2026.06.13"),
    ]:
        run("export", f"recipes/{name}", f"--version={version}")
    # Explicit profiles permit graph resolution on one machine without running
    # any cross compilers. Include both viewer postures and host/build contexts.
    profiles = {
        "linux": ["os=Linux", "arch=x86_64", "compiler=gcc", "compiler.version=14", "compiler.libcxx=libstdc++11"],
        "macos": ["os=Macos", "arch=armv8", "compiler=apple-clang", "compiler.version=17", "compiler.libcxx=libc++"],
        "windows": ["os=Windows", "arch=x86_64", "compiler=msvc", "compiler.version=194", "compiler.runtime=dynamic", "compiler.runtime_type=Release"],
        "wasm": ["os=Emscripten", "arch=wasm", "compiler=clang", "compiler.version=22", "compiler.libcxx=libc++"],
    }
    with tempfile.TemporaryDirectory(prefix="nh-lock-") as directory:
        locks = []
        for platform, settings in profiles.items():
            # wasm tools run on Linux. Others resolve their native tool graphs.
            build = profiles["linux"] if platform == "wasm" else settings
            for viewer in (False, True):
                lock = str(Path(directory) / f"{platform}-{viewer}.lock")
                args = []
                for context, values in [("h", settings), ("b", build)]:
                    for value in [*values, "build_type=Release", "compiler.cppstd=23"]:
                        args += [f"-s:{context}", value]
                run("lock", "create", ".", "--lockfile=", f"--lockfile-out={lock}",
                    "--build=missing", "-o", f"viewer={viewer}", *args, *update)
                locks += ["--lockfile", lock]
        run("lock", "merge", *locks, "--lockfile-out=conan.lock")


if __name__ == "__main__":
    main()
