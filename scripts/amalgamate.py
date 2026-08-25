#!/usr/bin/env python3
"""Assemble one self-contained C++ header from the modular sources.

The stb/sqlite shape: a consumer `#include`s the result anywhere for the
declarations, and exactly one translation unit defines `NH_IMPLEMENTATION`
before including it to get the definitions. There is no library to build and
nothing to link -- which is the whole point, since the consumer is an experiment
whose build we do not control and whose environment we cannot add to.

    // anywhere
    #include "nodehammer_connect.h"

    // exactly one .cpp, which includes nothing else
    #define NH_IMPLEMENTATION
    #include "nodehammer_connect.h"

Two sections come out of that, and the split is what every decision here serves:

  interface        the public headers, emitted unconditionally. Only `std` types
                   and forward declarations appear, so including this is cheap
                   no matter how much is vendored below.
  implementation   everything else -- vendored third-party headers, our internal
                   headers, and the function bodies -- inside `#ifdef
                   NH_IMPLEMENTATION`. A consumer that only wants declarations
                   never parses any of it.

Classification is by *location*, not by a hand-written list: an include that
resolves inside one of the `--inline-dir` roots gets its content pasted in, and
anything else stays a real `#include`. So the C++ standard library, ROOT and
DD4hep -- the environment the experiment already has, and the reason this is
worth shipping at all -- are left for their compiler to find, while our sources
and the dependencies they would not have are absorbed.

Written in stdlib Python rather than CMake script mode: this is a recursive
graph walk over the include tree (glm alone reaches hundreds of files), and
CMake's string handling is the wrong tool for that. CMake still owns *fetching*
-- every path this script is handed comes from a resolved Conan/CMake target --
so there is no second place where a dependency version is decided.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# `#include <foo>` / `#include "foo"`, with whatever leading space. Anything
# else on the line (a trailing comment) is kept with the line when it passes
# through, and discarded with it when it is inlined.
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\b")


class Amalgamator:
    def __init__(self, inline_dirs: list[Path], search_dirs: list[Path]) -> None:
        # Where an include may be *resolved from*. A superset of inline_dirs:
        # the generated flatbuffer headers, for instance, are found here and
        # inlined, while ROOT's headers are found by the consumer's compiler and
        # never looked for at all.
        self.search_dirs = search_dirs
        self.inline_dirs = inline_dirs
        # Every file already pasted in, so the second include of a header is a
        # no-op. This replaces `#pragma once`, which cannot work once every
        # header lives in one file.
        self.emitted: set[Path] = set()

    def resolve(self, name: str, relative_to: Path) -> Path | None:
        """Find `name` on the search path, or next to the file including it."""
        candidates = [relative_to.parent / name]
        candidates += [d / name for d in self.search_dirs]
        for c in candidates:
            if c.is_file():
                return c.resolve()
        return None

    def should_inline(self, path: Path) -> bool:
        return any(
            path.is_relative_to(d) for d in self.inline_dirs
        )

    def process(self, path: Path, out: list[str]) -> None:
        """Paste `path` into `out`, recursing into everything inlinable."""
        path = path.resolve()
        if path in self.emitted:
            return
        self.emitted.add(path)

        out.append(f"// ─── {self._label(path)} " + "─" * max(0, 60 - len(self._label(path))))
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if PRAGMA_ONCE_RE.match(line):
                # `self.emitted` is the include guard now.
                continue

            m = INCLUDE_RE.match(line)
            if not m:
                out.append(line)
                continue

            target = self.resolve(m.group(1), path)
            if target is not None and self.should_inline(target):
                self.process(target, out)
            else:
                # Not ours, or not found: the consumer's compiler resolves it,
                # and it stays exactly where it was written.
                #
                # Emphatically *not* hoisted to the top of the section, which is
                # what an earlier version did and what quom does. A third party's
                # includes are routinely conditional -- nlohmann picks between
                # <filesystem> and <experimental/filesystem> inside an `#if`, and
                # hoisting both out of it produces a header that includes the one
                # the compiler does not have. Left in place, the `#if` still
                # surrounds them. Repeats cost nothing: every header these name
                # carries its own include guard.
                out.append(line)
        out.append("")

    def _label(self, path: Path) -> str:
        for d in self.search_dirs:
            if path.is_relative_to(d):
                return str(path.relative_to(d))
        return path.name


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", required=True, type=Path)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument(
        "--inline-dir",
        action="append",
        default=[],
        type=Path,
        help="Includes resolving under this directory are pasted in. Repeatable.",
    )
    ap.add_argument(
        "--search-dir",
        action="append",
        default=[],
        type=Path,
        help="Where to look for includes. Repeatable. Implies nothing about inlining.",
    )
    ap.add_argument("--version", default="", help="Recorded in the banner.")
    args = ap.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))

    inline_dirs = [d.resolve() for d in args.inline_dir]
    # Everything inlinable is also searchable; the reverse does not hold.
    search_dirs = [d.resolve() for d in args.search_dir] + inline_dirs

    missing = [d for d in inline_dirs + search_dirs if not d.is_dir()]
    if missing:
        print(
            "amalgamate: these directories do not exist:\n  "
            + "\n  ".join(str(m) for m in missing),
            file=sys.stderr,
        )
        return 1

    amal = Amalgamator(inline_dirs, search_dirs)

    def roots(key: str) -> list[Path]:
        found = []
        for name in manifest.get(key, []):
            p = amal.resolve(name, args.output)
            if p is None:
                print(f"amalgamate: {key} entry not found: {name}", file=sys.stderr)
                raise SystemExit(1)
            found.append(p)
        return found

    interface_body: list[str] = []
    for root in roots("interface"):
        amal.process(root, interface_body)

    implementation_body: list[str] = []
    for root in roots("implementation"):
        amal.process(root, implementation_body)

    name = manifest.get("name", args.output.name)
    out: list[str] = []
    out.append(f"// {name} — generated by scripts/amalgamate.py. Do not edit.")
    out.append("//")
    out.append(f"// nodehammer {args.version}".rstrip())
    out.append("//")
    for line in manifest.get("banner", []):
        out.append(f"// {line}".rstrip())
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("// One translation unit, so nothing is imported or exported: NH_API has a")
    out.append("// branch for exactly this and it expands to nothing.")
    out.append("#ifndef NH_STATIC")
    out.append("#define NH_STATIC")
    out.append("#endif")
    out.append("")
    out.extend(interface_body)

    out.append("")
    out.append("#ifdef NH_IMPLEMENTATION")
    out.append("")

    # The backend gates the absorbed sources are written against.
    #
    # `#if NH_WITH_TGEO` guards the TGeo entry point in the library, where CMake
    # defines it for a build that compiled the importer. Here the manifest has
    # already decided -- it either named that importer or it did not -- so the
    # header answers for itself rather than making the consumer's build system
    # repeat a choice it cannot see. Defined inside the guard because they gate
    # definitions only: the public header declares every overload either way.
    defines = manifest.get("defines", [])
    if defines:
        for d in defines:
            name, _, value = d.partition("=")
            out.append(f"#ifndef {name}")
            out.append(f"#define {name} {value or '1'}")
            out.append("#endif")
        out.append("")

    out.extend(implementation_body)
    out.append("")
    out.append("#endif // NH_IMPLEMENTATION")
    out.append("")

    text = "\n".join(out)

    # Only when it differs, so a no-op regeneration does not recompile every
    # consumer -- the same courtesy cmake/GenerateSkillsData.cmake extends.
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.is_file() and args.output.read_text(encoding="utf-8") == text:
        args.output.touch()
    else:
        args.output.write_text(text, encoding="utf-8")

    lines = text.count("\n") + 1
    print(
        f"amalgamate: {args.output.name}: {len(amal.emitted)} files, {lines} lines, "
        f"{len(text) / 1024:.0f} KiB",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
