#!/usr/bin/env python3
"""Assert that libnodehammer.so leaks neither third-party nor internal symbols.

Two properties, and it is worth being precise about which, because an earlier
version of this script claimed a third that it could not actually check.

  A. No third-party symbol escapes. zstd, flatbuffers, manifold, miniz, lua and
     the rest are absorbed into the .so as static archives, and on ELF's flat
     symbol namespace an exported `ZSTD_compress` can interpose — or be
     interposed by — a different zstd elsewhere in the process, silently mixing
     versions. --exclude-libs,ALL is what prevents this, and this half of the
     check is a regression test on that flag: it is one line in CMakeLists.txt,
     easy to lose in a refactor, and nothing else would notice.

  B. No internal nodehammer namespace is exported. Hidden visibility already
     ensures this, so a failure here means someone marked an internal entity
     NH_API by mistake.

What is deliberately *not* asserted is "the export table is exactly the public
API". The table also carries std:: template instantiations, because libstdc++
declares `namespace std _GLIBCXX_VISIBILITY(default)` and so overrides
-fvisibility=hidden for everything in std::. That is intentional on libstdc++'s
part — vague-linkage instantiations are meant to merge across shared objects —
and this library's own API passes std:: types across the boundary anyway, so
suppressing them would buy no ABI independence it does not already lack. They
are reported separately below as tolerated, not counted as leaks.

The rules are derived rather than listed. An allowlist of expected symbols would
have to be edited every time step 6 adds a verb, and an entry nobody removed
would silently permit a leak. Instead:

  1. Every internal namespace is harvested from the tree itself, by grepping
     src/ for `namespace nodehammer::...`. A namespace added tomorrow is picked
     up tomorrow, with no list to update.
  2. An exported symbol is a leak if it is scoped into one of those, and a
     third-party leak if it is under neither `nodehammer::` nor a tolerated
     runtime prefix.
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
the static dependencies' symbols do remain visible in the dylib and property A
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
#
# The thunk spellings are enumerated rather than matched with a leading `thunk`:
# c++filt emits "non-virtual thunk to", "virtual thunk to" and "covariant return
# thunk to", none of which *start* with "thunk", so a `^thunk .*? to ` pattern
# matches nothing at all. Thunks reach the table whenever a public class uses
# multiple inheritance.
DEMANGLED_PREFIX = re.compile(
    r"^(?:typeinfo(?: name)? for |vtable for |VTT for |guard variable for |"
    r"(?:non-virtual |virtual |covariant return )?thunk to |"
    r"construction vtable for .*?-in-)"
)

TOP = "nodehammer::"

# Exported, not public API, and not a leak either. Everything here is emitted
# into our own objects by the compiler or the standard library rather than
# coming from a dependency archive, so --exclude-libs cannot reach it and
# hidden visibility does not apply (see the module docstring on std::).
TOLERATED_PREFIXES = (
    "std::",
    "__gnu_cxx::",
    "__cxxabiv1::",
)


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


def bucket(symbol: str, internal: set[str]) -> tuple[str, str | None]:
    """Sort an exported symbol into "public", "tolerated" or "leak".

    The reason string is populated for leaks, and for tolerated symbols carries
    the category so the report can group them.
    """
    name = DEMANGLED_PREFIX.sub("", symbol)

    if name in LINKER_ARTEFACTS:
        return "tolerated", "linker artefact"

    for prefix in TOLERATED_PREFIXES:
        if name.startswith(prefix):
            return "tolerated", prefix.rstrip(":")

    if not name.startswith(TOP):
        # Property A. Reaching here means a dependency's symbol survived into
        # the dynamic table, which on ELF's flat namespace is the failure that
        # actually bites — so name the flag rather than the symbol.
        return "leak", "third-party symbol — is --exclude-libs,ALL still on the link?"

    scope = scope_of(name)

    # Longest match first, so nodehammer::ir::render is preferred over
    # nodehammer::ir and the message names the namespace actually responsible.
    for ns in sorted(internal, key=len, reverse=True):
        if scope.startswith(ns + "::"):
            return "leak", f"internal namespace {ns}"

    head, sep, _ = scope[len(TOP):].partition("::")
    if sep and head[:1].islower():
        return "leak", f"looks like an internal namespace (nodehammer::{head}) not found in src/"

    return "public", None


# Cases the rules have to get right, checked by --self-test. They exist because
# the signature-stripping above is not obvious: the first draft of this script
# rejected `nodehammer::tessellate(...)` as a namespace, having found the `::`
# in its parameter list. Public entries use step 6's planned surface, so the
# check is exercised against the API it will actually police rather than only
# against the single symbol that exists today.
SELF_TEST_CASES: list[tuple[str, str]] = [
    # (demangled symbol, expected bucket)
    ("nodehammer::version()", "public"),
    ("nodehammer::tessellate(nodehammer::SemanticScene const&, nodehammer::SceneConfig const&)", "public"),
    ("nodehammer::build(nodehammer::SemanticScene const&, nodehammer::SceneConfig const&)", "public"),
    ("nodehammer::applySelection(nodehammer::SemanticScene const&)", "public"),
    ("nodehammer::SemanticScene::read(std::filesystem::path const&)", "public"),
    ("nodehammer::RenderScene::write(std::filesystem::path const&) const", "public"),
    ("nodehammer::DiagnosticList::hasErrors() const", "public"),
    ("nodehammer::operator==(nodehammer::Diagnostic const&, nodehammer::Diagnostic const&)", "public"),
    # The decorated forms a public class brings with it. All of them have to be
    # exported for a consumer to derive from the class, construct it, catch it
    # by type, or dynamic_cast it — a version script restricted to
    # `nodehammer::*` would silently drop every one, since none of these
    # demangled names *starts* with `nodehammer::`.
    ("vtable for nodehammer::SemanticScene", "public"),
    ("typeinfo for nodehammer::SemanticScene", "public"),
    ("typeinfo name for nodehammer::SemanticScene", "public"),
    ("VTT for nodehammer::SemanticScene", "public"),
    ("non-virtual thunk to nodehammer::SemanticScene::write() const", "public"),
    ("virtual thunk to nodehammer::SemanticScene::~SemanticScene()", "public"),
    ("covariant return thunk to nodehammer::SemanticScene::clone() const", "public"),
    # Property A: a dependency's symbol in the table means --exclude-libs is not
    # doing its job. This is the half of the check that earns its keep.
    ("ZSTD_compress", "leak"),
    ("flatbuffers::ClassicLocale::instance_", "leak"),
    ("manifold::Manifold::Boolean(manifold::Manifold const&)", "leak"),
    ("mz_zip_reader_init", "leak"),
    ("luaL_newstate", "leak"),
    # Property B: internal namespaces, caught by the src/ harvest.
    ("nodehammer::ir::render::Scene::clear()", "leak"),
    ("nodehammer::ir::semantic::Scene::visitBFS()", "leak"),
    ("nodehammer::diagnostics::DiagnosticList::hasErrors() const", "leak"),
    ("nodehammer::config::keys::kExport", "leak"),
    ("nodehammer::viewer::ui::icon_font::glyphs", "leak"),
    ("nodehammer::detail::zstd_io::compress(std::span<std::byte const>)", "leak"),
    ("typeinfo for nodehammer::ir::semantic::Scene", "leak"),
    # A namespace nobody has written yet: rule 1 cannot know it, rule 3 must.
    ("nodehammer::brandnew::Thing::f()", "leak"),
    # Tolerated. Real symbols from the first CI run of this job: libstdc++ gives
    # namespace std default visibility, so these are emitted from our own
    # objects and survive -fvisibility=hidden. Not leaks — see the docstring.
    ("std::_Rb_tree<std::array<unsigned int, 2ul>, std::pair<std::array<unsigned int, 2ul> const, "
     "unsigned int> >::_M_get_insert_unique_pos(std::array<unsigned int, 2ul> const&)", "tolerated"),
    ("std::__unicode::__v15_1_0::__width_edges", "tolerated"),
    ("typeinfo for std::_Sp_counted_base<(__gnu_cxx::_Lock_policy)2>", "tolerated"),
    ("__gnu_cxx::__throw_insufficient_space(char const*, char const*)", "tolerated"),
    ("_init", "tolerated"),
    ("__bss_start", "tolerated"),
]


def self_test(source_dir: Path) -> int:
    internal = internal_namespaces(source_dir)
    if not internal:
        print(f"error: no internal namespaces harvested from {source_dir}/src", file=sys.stderr)
        return 2

    failures = 0
    for symbol, expected in SELF_TEST_CASES:
        got, why = bucket(symbol, internal)
        if got != expected:
            failures += 1
            print(f"FAIL: expected {symbol!r} to be {expected}, got {got} ({why})",
                  file=sys.stderr)

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

    public: list[str] = []
    tolerated: dict[str, int] = {}
    leaks: list[tuple[str, str]] = []
    for symbol in symbols:
        kind, why = bucket(symbol, internal)
        if kind == "public":
            public.append(symbol)
        elif kind == "tolerated":
            tolerated[why or "?"] = tolerated.get(why or "?", 0) + 1
        else:
            leaks.append((symbol, why or "?"))

    print(f"{args.library.name}: {len(symbols)} exported symbols, "
          f"{len(internal)} internal namespaces harvested from src/")
    print("\npublic API surface:")
    for s in sorted(public):
        print(f"  {s}")

    if tolerated:
        summary = ", ".join(f"{n}× {k}" for k, n in sorted(tolerated.items()))
        print(f"\ntolerated (not public API, not a leak): {summary}")

    if leaks:
        print(f"\n{len(leaks)} symbol(s) must not be exported:", file=sys.stderr)
        for symbol, why in sorted(leaks)[:50]:
            print(f"  {symbol}\n      -> {why}", file=sys.stderr)
        if len(leaks) > 50:
            print(f"  ... and {len(leaks) - 50} more", file=sys.stderr)
        print(
            "\nA third-party symbol here means --exclude-libs,ALL is no longer on "
            "the shared link. A nodehammer:: one means an internal entity picked up "
            "NH_API by mistake, or a target skipped nodehammer_set_visibility(): if "
            "it is meant to be public, move its declaration into include/nodehammer/ "
            "and out of a nested namespace.",
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

    print("\nOK: no third-party and no internal symbols are exported.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
