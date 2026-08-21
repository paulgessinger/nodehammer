#!/usr/bin/env python3
"""Reject unquoted `Module.foo` property access in EM_JS bodies.

The Release wasm link (`conan-emscripten-release`, see cmake/CompilerOptions.cmake)
runs `--closure=1`. Closure renames every *unquoted* property it sees, but
emscripten publishes its exports with quoted keys -- `Module["HEAPU8"]`,
`Module["_malloc"]`, and the `EXPORTED_FUNCTIONS` wrappers. Closure preserves
those. So a body that reads `Module.HEAPU8` compiles to `h.$c`, an assignment
that never happens, and the call site dies with

    TypeError: can't access property "set", h.$c is undefined

only in Release, only when that handler fires -- which is why this class of bug
keeps reaching the deployed site. RelWithDebInfo skips Closure and looks fine.

The rule is therefore: inside EM_JS, reach everything off `Module` by quoted
subscript. Bare module-scope identifiers (`HEAPU8`, `_malloc`, `stringToUTF8`)
are safe and not flagged -- Closure renames those consistently because it can
see every use.

Only C/C++ sources are scanned: their EM_JS bodies are linked into the module
and go through Closure. Hand-written JS that stays *outside* the module
(web/index.html, src/web/compute_worker.js) is never minified and correctly
uses dot access against the literal names, so it must not be flagged here.

Usage: check_em_js_closure.py [files...]   (defaults to git-tracked C/C++ sources)
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# `Module` followed by a dot and an identifier. Computed/quoted access
# (`Module['x']`) does not match, which is exactly the form we want.
UNQUOTED = re.compile(r"\bModule\.([A-Za-z_$][A-Za-z0-9_$]*)")

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}


def tracked_sources() -> list[Path]:
    out = subprocess.run(
        ["git", "ls-files", "--", "src", "include", "tests"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    return [Path(p) for p in out if Path(p).suffix in SOURCE_SUFFIXES]


def check(path: Path) -> list[tuple[int, str, str]]:
    """Return (line number, property name, source line) for each violation."""
    hits = []
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError):
        return hits
    if "Module." not in text:
        return hits
    for lineno, line in enumerate(text.splitlines(), start=1):
        # Prose mentions the pattern freely; only code is Closure's problem.
        if line.lstrip().startswith(("//", "*", "/*")):
            continue
        for match in UNQUOTED.finditer(line):
            hits.append((lineno, match.group(1), line.strip()))
    return hits


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv] if argv else tracked_sources()
    paths = [p for p in paths if p.suffix in SOURCE_SUFFIXES]

    violations = [(p, *hit) for p in paths for hit in check(p)]
    if not violations:
        return 0

    print("Unquoted Module property access in EM_JS (Closure will rename it):\n")
    for path, lineno, prop, line in violations:
        print(f"  {path}:{lineno}: Module.{prop}")
        print(f"      {line}")
        print(f"      -> write Module['{prop}'] instead\n")
    print(
        f"{len(violations)} violation(s). These break only in the Release wasm\n"
        "build (--closure=1), at the moment the handler runs. See the comment\n"
        "at the top of scripts/check_em_js_closure.py."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
