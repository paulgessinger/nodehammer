#!/usr/bin/env python3
"""Reject process-terminating calls under src/cli/.

The command sources compile into libnodehammer (`NH_CORE_SOURCES` in
CMakeLists.txt), so they run inside whatever called them. A `std::exit` there is
not "the one exit path a CLI has" -- it is the caller's process ending in the
middle of a call, with no unwinding, no destructors, no `finally` and no
`atexit`. For the Python bindings that is the interpreter, and an interactive
session loses its kernel to a mistyped path.

There were 21 of these, and removing them was the bulk of the work that made
`cli::run` possible. Nothing in the type system prevents the twenty-second: a
new subcommand written the old way compiles, links, passes review by looking
exactly like the code that used to be here, and breaks only for the caller that
is not a shell. So the rule is checked rather than remembered.

What to write instead:

    throw nodehammer::Error{codes::kFatal..., "what went wrong", context};

inside the command's `runOrReport` body, which reports it and answers non-zero.
For a code that is not a failure -- the viewer's own exit status -- throw
`cli::detail::CommandFailure{rc}`.

`main.cpp` is exempt: it *is* a process, and returning from it is the same thing
as exiting. It happens not to need one.

Usage: check_cli_no_exit.py [files...]   (defaults to the tracked CLI sources)
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

# Both spellings, plus the neighbours that end a process just as abruptly.
# `quick_exit` skips even static destructors; `abort` and `terminate` take the
# process down without a status a caller could interpret.
# The lookbehind is load-bearing: `app.exit(e)` is CLI11's "print this parse
# error and give me the status", which is a *return* value and precisely the
# thing that replaced CLI11_PARSE. Matching it would flag the fix as the bug.
FORBIDDEN = re.compile(
    r"(?<![\w.>:])(?:std::)?(exit|_Exit|quick_exit|abort|terminate)\s*\("
)

# A line that mentions one of these inside a comment is describing the rule, not
# breaking it -- the headers of run_internal.hpp and cli_common.hpp both do.
COMMENT = re.compile(r"^\s*(//|\*|/\*)")

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx"}


def tracked_cli_sources() -> list[Path]:
    # prek passes the files, so this path is only for a bare `python3
    # scripts/check_cli_no_exit.py`. It falls back to a plain walk rather than
    # failing when git cannot answer -- a jj workspace has no .git of its own,
    # and the check has nothing to do with what is tracked.
    try:
        out = subprocess.run(
            ["git", "ls-files", "src/cli"], check=True, capture_output=True, text=True
        ).stdout.split()
        paths = [Path(p) for p in out]
    except (OSError, subprocess.CalledProcessError):
        paths = sorted(Path("src/cli").rglob("*"))
    return [p for p in paths if p.suffix in SOURCE_SUFFIXES]


def check(path: Path) -> list[tuple[int, str, str]]:
    if path.name == "main.cpp":
        return []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []

    hits = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if COMMENT.match(line):
            continue
        match = FORBIDDEN.search(line)
        if match:
            hits.append((lineno, match.group(1), line.strip()))
    return hits


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv] if argv else tracked_cli_sources()
    paths = [p for p in paths if p.suffix in SOURCE_SUFFIXES and "src/cli" in p.as_posix()]

    violations = [(p, *hit) for p in paths for hit in check(p)]
    if not violations:
        return 0

    print("A command may not end the process it is running in:\n")
    for path, lineno, name, line in violations:
        print(f"  {path}:{lineno}: {name}(")
        print(f"      {line}\n")
    print(
        f"{len(violations)} violation(s). These sources compile into libnodehammer,\n"
        "so this ends the caller -- for the Python bindings, the interpreter.\n"
        "Throw nodehammer::Error inside the command's runOrReport body instead.\n"
        "See the comment at the top of scripts/check_cli_no_exit.py."
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
