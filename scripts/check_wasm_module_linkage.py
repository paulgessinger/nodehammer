#!/usr/bin/env python3
"""Check that every `Module['x']` our glue reaches actually resolves at runtime.

Companion to check_em_js_closure.py, which works on source. This one works on
the *built* JS, and so catches the failures that source alone cannot show:

  - a name that is quoted correctly but was never exported, because
    EXPORTED_FUNCTIONS in cmake/CompilerOptions.cmake is a hand-maintained list
    and the C function carries no EMSCRIPTEN_KEEPALIVE. Closure is not even
    involved; the property is simply absent.
  - a name Closure renamed on the definition side, leaving the quoted read
    dangling.

Both fail identically and silently in the browser -- `Module['_foo'] is not a
function` / `can't access property "set", ... is undefined` -- and only when
that particular handler runs, which is how they reach the deployed site.

A name resolves if either:
  - the built module assigns it (`d._foo=`, `h.HEAPU8=`, `Module["x"]=`), or
  - the host page supplies it. web/index.html builds `window.Module = {...}`
    before the runtime loads, and is never minified, so keys it sets (canvas,
    print, printErr, ...) are live under their literal names.

Usage:
    check_wasm_module_linkage.py build/emscripten/Release/nodehammer-*.js
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

QUOTED_ACCESS = re.compile(r"Module\['([^']+)'\]")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}

HOST_PAGE = Path("web/index.html")


def glue_names() -> dict[str, list[str]]:
    """Module properties reached from C/C++ EM_JS bodies -> where."""
    tracked = subprocess.run(
        ["git", "ls-files", "--", "src", "include"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.split()
    found: dict[str, list[str]] = {}
    for name in tracked:
        path = Path(name)
        if path.suffix not in SOURCE_SUFFIXES:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "Module['" not in text:
            continue
        for lineno, line in enumerate(text.splitlines(), start=1):
            if line.lstrip().startswith(("//", "*", "/*")):
                continue
            for prop in QUOTED_ACCESS.findall(line):
                found.setdefault(prop, []).append(f"{path}:{lineno}")
    return found


def host_provided() -> set[str]:
    """Keys web/index.html sets on Module before the runtime loads."""
    if not HOST_PAGE.exists():
        return set()
    text = HOST_PAGE.read_text(encoding="utf-8", errors="replace")
    names = set(re.findall(r"Module\.([A-Za-z_$][A-Za-z0-9_$]*)", text))
    names |= set(re.findall(r"Module\['([^']+)'\]", text))
    # Keys of the `window.Module = { ... }` literal.
    literal = re.search(r"window\.Module\s*=\s*\{(.*?)\n\s*\};", text, re.S)
    if literal:
        names |= set(re.findall(r"^\s*([A-Za-z_$][A-Za-z0-9_$]*)\s*:", literal.group(1), re.M))
    return names


def assigned_in(js: str, name: str) -> bool:
    """Does the built module define this property under its literal name?"""
    escaped = re.escape(name)
    return bool(
        re.search(r"[.\]]" + escaped + r"\s*=", js)
        or re.search(r'\["' + escaped + r'"\]\s*=', js)
        or re.search(r"\['" + escaped + r"'\]\s*=", js)
    )


def main(argv: list[str]) -> int:
    if not argv:
        print("usage: check_wasm_module_linkage.py <built .js> [...]", file=sys.stderr)
        return 2

    names = glue_names()
    host = host_provided()
    if not names:
        print("no Module['...'] access found in glue sources -- nothing to check")
        return 0

    failed = False
    for arg in argv:
        js_path = Path(arg)
        if not js_path.exists():
            print(f"{js_path}: missing (build the wasm target first)", file=sys.stderr)
            return 2
        js = js_path.read_text(encoding="utf-8", errors="replace")

        missing = [
            (name, sites)
            for name, sites in sorted(names.items())
            if not assigned_in(js, name) and name not in host
        ]
        if missing:
            failed = True
            print(f"\n{js_path}: {len(missing)} unresolved Module propert(ies):\n")
            for name, sites in missing:
                print(f"  Module['{name}'] -- not defined in the built module")
                for site in sites:
                    print(f"      read at {site}")
                print(
                    "      -> export it (EMSCRIPTEN_KEEPALIVE, or add it to\n"
                    "         EXPORTED_FUNCTIONS / EXPORTED_RUNTIME_METHODS in\n"
                    "         cmake/CompilerOptions.cmake)\n"
                )
        else:
            print(f"{js_path}: all {len(names)} Module properties resolve")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
