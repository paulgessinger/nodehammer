#!/usr/bin/env python3
"""Assert that libnodehammer.so exports the public API and nothing else.

This is the check #41 step 5 is verified by: the one thing that proves the
public/internal split holds at link time and not merely at include time. The
physical header split (§1) stops a consumer *including* an internal header;
hidden visibility plus --exclude-libs stops them *linking* an internal symbol.
Only the second is observable from outside the build, and only here.

The rule is derived rather than listed. An allowlist of expected symbols would
have to be edited every time step 6 adds a verb, and an entry nobody removed
would silently permit a leak. Instead:

  1. Every internal namespace is harvested from the tree itself, by grepping
     src/ for `namespace nodehammer::...`. A namespace added tomorrow is picked
     up tomorrow, with no list to update.
  2. An exported symbol is a leak if it is scoped into one of those, and a
     third-party leak if it is not under `nodehammer::` at all.
  3. As a backstop for a namespace the grep somehow misses, a symbol whose
     second scope component is lowercase *and* followed by `::` is treated as
     internal too — namespaces are lowercase by convention here and public
     types are not. This is deliberately secondary: it is a convention, and
     conventions are what rule 1 exists to avoid depending on.

Note what rule 3 does *not* reject: `nodehammer::tessellate(...)`. A namespace
is always followed by `::` and a free function by `(`, so a lowercase public
verb directly in `nodehammer::` is not mistaken for a namespace. That case is
the reason rule 3 tests the separator and not just the case.

Linux/ELF only. macOS is skipped by the caller: ld64 has no --exclude-libs, so
the static dependencies' symbols do remain visible in the dylib and the check
would fail for a reason that is not a defect in this project.

Usage: ci/check_shared_exports.py <path-to-libnodehammer.so> [--source-dir DIR]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Symbols every ELF shared object carries, contributed by the linker rather
# than by any translation unit.
LINKER_ARTEFACTS = {
    "_init",
    "_fini",
    "_edata",
    "_end",
    "__bss_start",
}

# c++filt renders some symbols as "<kind> for <name>"; the interesting part is
# the name, and the kind is orthogonal to whether it should be exported.
DEMANGLED_PREFIX = re.compile(
    r"^(?:typeinfo(?: name)? for |vtable for |VTT for |"
    r"guard variable for |thunk .*? to |construction vtable for .*?-in-)"
)

TOP = "nodehammer::"


def internal_namespaces(source_dir: Path) -> set[str]:
    """Harvest `namespace nodehammer::x::y` declarations from the source tree."""
    pattern = re.compile(r"^namespace\s+(nodehammer(?:::[A-Za-z_][A-Za-z0-9_]*)+)", re.M)
    found: set[str] = set()
    for path in (source_dir / "src").rglob("*"):
        if path.suffix not in {".hpp", ".cpp", ".h", ".cc", ".mm"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        found.update(m.group(1) for m in pattern.finditer(text))
    return found


def defined_dynamic_symbols(library: Path) -> list[str]:
    """Demangled names of everything the .so defines in its dynamic table."""
    raw = subprocess.run(
        ["nm", "--dynamic", "--defined-only", "--extern-only", "--format=just-symbols",
         str(library)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.split()
    if not raw:
        return []
    demangled = subprocess.run(
        ["c++filt"], input="\n".join(raw), check=True, capture_output=True, text=True
    ).stdout.splitlines()
    return [s.strip() for s in demangled if s.strip()]


def scope_of(name: str) -> str:
    """The qualified name with any signature and template arguments removed.

    Both carry their own `::` — `nodehammer::tessellate(nodehammer::SemanticScene
    const&)` has one inside the parameter list — and reading those as scope
    separators would misfile a public free function as a namespace. Truncating
    at the first `(` or `<` leaves exactly the scope chain. `operator==` and
    `operator()` survive it: what remains is still `nodehammer::operator...`,
    whose second component is not followed by `::`.
    """
    cut = len(name)
    for ch in "(<":
        pos = name.find(ch)
        if pos != -1:
            cut = min(cut, pos)
    return name[:cut]


def classify(symbol: str, internal: set[str]) -> str | None:
    """Return a reason string if the symbol must not be exported, else None."""
    name = DEMANGLED_PREFIX.sub("", symbol)

    if name in LINKER_ARTEFACTS:
        return None

    if not name.startswith(TOP):
        return "not in nodehammer:: (third-party or internal C symbol)"

    scope = scope_of(name)

    # Longest match first, so nodehammer::ir::render is preferred over
    # nodehammer::ir and the message names the namespace actually responsible.
    for ns in sorted(internal, key=len, reverse=True):
        if scope.startswith(ns + "::"):
            return f"internal namespace {ns}"

    head, sep, _ = scope[len(TOP):].partition("::")
    if sep and head[:1].islower():
        return f"looks like an internal namespace (nodehammer::{head}) not found in src/"

    return None


# Cases the rules have to get right, checked by --self-test. They exist because
# the signature-stripping above is not obvious: the first draft of this script
# rejected `nodehammer::tessellate(...)` as a namespace, having found the `::`
# in its parameter list. Public entries use step 6's planned surface, so the
# check is exercised against the API it will actually police rather than only
# against the single symbol that exists today.
SELF_TEST_CASES: list[tuple[str, bool]] = [
    # (demangled symbol, is_leak)
    ("nodehammer::version()", False),
    ("nodehammer::tessellate(nodehammer::SemanticScene const&, nodehammer::SceneConfig const&)", False),
    ("nodehammer::build(nodehammer::SemanticScene const&, nodehammer::SceneConfig const&)", False),
    ("nodehammer::applySelection(nodehammer::SemanticScene const&)", False),
    ("nodehammer::SemanticScene::read(std::filesystem::path const&)", False),
    ("nodehammer::RenderScene::write(std::filesystem::path const&) const", False),
    ("nodehammer::DiagnosticList::hasErrors() const", False),
    ("vtable for nodehammer::SemanticScene", False),
    ("nodehammer::operator==(nodehammer::Diagnostic const&, nodehammer::Diagnostic const&)", False),
    ("_init", False),
    ("__bss_start", False),
    ("nodehammer::ir::render::Scene::clear()", True),
    ("nodehammer::ir::semantic::Scene::visitBFS()", True),
    ("nodehammer::diagnostics::DiagnosticList::hasErrors() const", True),
    ("nodehammer::config::keys::kExport", True),
    ("nodehammer::viewer::ui::icon_font::glyphs", True),
    ("nodehammer::detail::zstd_io::compress(std::span<std::byte const>)", True),
    ("typeinfo for nodehammer::ir::semantic::Scene", True),
    ("ZSTD_compress", True),
    ("flatbuffers::ClassicLocale::instance_", True),
    ("manifold::Manifold::Boolean(manifold::Manifold const&)", True),
    # A namespace nobody has written yet: rule 1 cannot know it, rule 3 must.
    ("nodehammer::brandnew::Thing::f()", True),
]


def self_test(source_dir: Path) -> int:
    internal = internal_namespaces(source_dir)
    if not internal:
        print(f"error: no internal namespaces harvested from {source_dir}/src", file=sys.stderr)
        return 2

    failures = 0
    for symbol, is_leak in SELF_TEST_CASES:
        why = classify(symbol, internal)
        if (why is not None) != is_leak:
            failures += 1
            expected = "rejected" if is_leak else "accepted"
            print(f"FAIL: expected {symbol!r} to be {expected}, got {why!r}", file=sys.stderr)

    print(f"self-test: {len(SELF_TEST_CASES) - failures}/{len(SELF_TEST_CASES)} cases pass "
          f"against {len(internal)} harvested namespaces")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path, nargs="?", help="path to libnodehammer.so")
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root, used to harvest internal namespaces",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="check the classification rules against SELF_TEST_CASES and exit. "
             "Runs anywhere, including where there is no ELF library to inspect.",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test(args.source_dir)

    if args.library is None:
        parser.error("a library path is required unless --self-test is given")

    if not args.library.is_file():
        print(f"error: no such library: {args.library}", file=sys.stderr)
        return 2

    internal = internal_namespaces(args.source_dir)
    if not internal:
        print(
            f"error: found no internal namespaces under {args.source_dir}/src — "
            "the harvest is broken, and passing would be meaningless",
            file=sys.stderr,
        )
        return 2

    symbols = defined_dynamic_symbols(args.library)
    leaks = [(s, why) for s in symbols if (why := classify(s, internal)) is not None]
    public = [s for s in symbols if classify(s, internal) is None
              and DEMANGLED_PREFIX.sub("", s) not in LINKER_ARTEFACTS]

    print(f"{args.library.name}: {len(symbols)} exported symbols, "
          f"{len(internal)} internal namespaces harvested from src/")
    print("\npublic API surface:")
    for s in sorted(public):
        print(f"  {s}")

    if leaks:
        print(f"\n{len(leaks)} symbol(s) must not be exported:", file=sys.stderr)
        for symbol, why in sorted(leaks)[:50]:
            print(f"  {symbol}\n      -> {why}", file=sys.stderr)
        if len(leaks) > 50:
            print(f"  ... and {len(leaks) - 50} more", file=sys.stderr)
        print(
            "\nEvery exported entity must be declared in an installed header and "
            "marked NH_API. If one of these is meant to be public, move its "
            "declaration into include/nodehammer/ and out of a nested namespace; "
            "if it is not, it is reaching the export table by accident — check "
            "that the shared link still carries --exclude-libs,ALL and that the "
            "target went through nodehammer_set_visibility().",
            file=sys.stderr,
        )
        return 1

    if not public:
        print(
            "\nerror: nothing is exported at all. The library cannot be used, and "
            "an empty export table would pass every check above vacuously.",
            file=sys.stderr,
        )
        return 1

    print("\nOK: the export table is exactly the public API.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
