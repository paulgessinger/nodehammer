#!/usr/bin/env python3
"""Assert that libnodehammer.so leaks neither third-party nor internal symbols.

Two properties, deliberately not three:

  A. No third-party symbol escapes. zstd, flatbuffers, manifold and the rest are
     absorbed as static archives, and on ELF's flat namespace an exported
     `ZSTD_compress` can interpose — or be interposed by — a different zstd in
     the same process, silently mixing versions. --exclude-libs,ALL prevents
     that, and this half is a regression test on that one line of CMakeLists.

  B. No internal namespace is exported. Hidden visibility already ensures it, so
     a failure means something internal was marked NH_API by mistake.

Not asserted: "the export table is exactly the public API". The table also
carries std:: template instantiations, because libstdc++ declares
`namespace std _GLIBCXX_VISIBILITY(default)` — on purpose, so they merge across
shared objects. They are reported as tolerated rather than counted as leaks.
Note that an enumeration of what *is* exported could not check that claim
anyway: a symbol wrongly *missing* leaves nothing to inspect.

The rules are derived rather than listed, so no allowlist goes stale:

  1. Internal namespaces are harvested from src/ on every run.
  2. A symbol is a leak if scoped into one of those, or if it is under neither
     `nodehammer::` nor a tolerated runtime prefix.
  3. Backstop for a namespace the harvest misses: a lowercase second component
     *followed by* `::` reads as internal. Testing the separator is what keeps
     `nodehammer::tessellate(...)` — a lowercase public verb — from matching.

Linux/ELF only; the caller skips macOS, where ld64 has no --exclude-libs and
property A would fail for a reason that is not a defect here.

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

# c++filt renders some symbols as "<kind> for <name>"; the kind is orthogonal to
# whether the entity should be exported. The three thunk spellings are
# enumerated because none of them *starts* with "thunk".
DEMANGLED_PREFIX = re.compile(
    r"^(?:typeinfo(?: name)? for |vtable for |VTT for |guard variable for |"
    r"(?:non-virtual |virtual |covariant return )?thunk to |"
    r"construction vtable for .*?-in-)"
)

TOP = "nodehammer::"

# Emitted into our own objects by the compiler or the standard library, so
# neither --exclude-libs nor hidden visibility reaches them. Not public API, not
# a leak.
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


# `operator<`, `operator>>` and friends contain characters that the bracket
# scanner below would otherwise read as nesting. Neutralised to a same-length
# placeholder before scanning, so offsets into the original string stay valid.
_OPERATOR_TOKEN = re.compile(r"operator(?:<=>|<<=|>>=|<<|>>|<=|>=|<|>|\(\)|\[\])")


def scope_of(name: str) -> str:
    """The entity's own qualified name: return type, parameters and template
    arguments removed.

    The entity is the last space-separated word at bracket depth zero before the
    parameter list. Depth matters in both directions: it leaves a trailing
    `const` alone, and it keeps a templated return type intact, so
    `std::pair<int, int> nodehammer::compute()` yields `nodehammer::compute`.

    A return type is present only sometimes — the Itanium ABI mangles it for
    function templates, so c++filt writes `char* std::__add_grouping<char>(...)`
    but plain `nodehammer::version()`. Anchoring at the start of the string
    therefore works for ordinary functions and silently fails for template
    instantiations, which are most of what a header-heavy library exports.

    Returns "" when there is no entity — RTTI over a function or builtin type,
    where what remains is a bare `bool (*)(std::string const&, void*)`.
    """
    scan = _OPERATOR_TOKEN.sub(lambda m: "operator" + "_" * (len(m.group()) - 8), name)

    depth = 0
    start = 0
    word = None
    for i, ch in enumerate(scan):
        if ch == "(" and depth == 0:
            word = name[start:i]
            break
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth = max(0, depth - 1)
        elif ch == " " and depth == 0:
            start = i + 1
    if word is None:
        word = name[start:]

    lt = word.find("<")
    if lt != -1:
        word = word[:lt]
    return word.strip()


def bucket(symbol: str, internal: set[str]) -> tuple[str, str | None]:
    """Sort a symbol into "public", "tolerated" or "leak".

    The reason is a message for leaks and a grouping key for tolerated symbols.
    """
    name = DEMANGLED_PREFIX.sub("", symbol)
    decorated = name != symbol

    if name in LINKER_ARTEFACTS:
        return "tolerated", "linker artefact"

    # Ask about the *entity*, not the raw string: a function template carries
    # its return type in front of its name, so `char* std::__add_grouping<char>`
    # is a std:: symbol that does not begin with "std::".
    scope = scope_of(name)

    if not scope:
        # No entity at all: RTTI over a function or builtin type, e.g.
        # `typeinfo for bool (*)(std::string const&, void*)`. Such a type has no
        # namespace to leak from. Only reachable via a decoration prefix — a bare
        # identifier like ZSTD_compress has an entity name and is judged below.
        if decorated:
            return "tolerated", "RTTI for a non-class type"
        return "leak", "unqualified symbol — is --exclude-libs,ALL still on the link?"

    for prefix in TOLERATED_PREFIXES:
        if scope.startswith(prefix):
            return "tolerated", prefix.rstrip(":")

    if not scope.startswith(TOP):
        # Property A: a dependency's symbol survived into the dynamic table.
        return "leak", "third-party symbol — is --exclude-libs,ALL still on the link?"

    # Longest match first, so nodehammer::ir::render is preferred over
    # nodehammer::ir and the message names the namespace actually responsible.
    for ns in sorted(internal, key=len, reverse=True):
        if scope.startswith(ns + "::"):
            return "leak", f"internal namespace {ns}"

    head, sep, _ = scope[len(TOP):].partition("::")
    if sep and head[:1].islower():
        return "leak", f"looks like an internal namespace (nodehammer::{head}) not found in src/"

    return "public", None


# Checked by --self-test, which needs no ELF library and so runs anywhere.
# Public entries use step 6's planned surface, so the rules are exercised
# against the API they will police rather than today's single symbol.
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
    # A public class brings all of these, and a consumer needs them to derive
    # from it, construct it, catch it by type or dynamic_cast it. None starts
    # with `nodehammer::`, which is why a version script cannot be used here.
    ("vtable for nodehammer::SemanticScene", "public"),
    ("typeinfo for nodehammer::SemanticScene", "public"),
    ("typeinfo name for nodehammer::SemanticScene", "public"),
    ("VTT for nodehammer::SemanticScene", "public"),
    ("non-virtual thunk to nodehammer::SemanticScene::write() const", "public"),
    ("virtual thunk to nodehammer::SemanticScene::~SemanticScene()", "public"),
    ("covariant return thunk to nodehammer::SemanticScene::clone() const", "public"),
    # Property A: --exclude-libs is not doing its job.
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
    # Real symbols from CI: libstdc++ gives namespace std default visibility, so
    # these are emitted from our own objects and survive -fvisibility=hidden.
    ("std::_Rb_tree<std::array<unsigned int, 2ul>, std::pair<std::array<unsigned int, 2ul> const, "
     "unsigned int> >::_M_get_insert_unique_pos(std::array<unsigned int, 2ul> const&)", "tolerated"),
    ("std::__unicode::__v15_1_0::__width_edges", "tolerated"),
    ("typeinfo for std::_Sp_counted_base<(__gnu_cxx::_Lock_policy)2>", "tolerated"),
    ("__gnu_cxx::__throw_insufficient_space(char const*, char const*)", "tolerated"),
    ("_init", "tolerated"),
    ("__bss_start", "tolerated"),
    # Verbatim from CI. Both shapes were misread as third-party leaks by a
    # startswith("std::") test. First: a function template mangles its return
    # type, so the name does not begin with "std::".
    ("char* std::__add_grouping<char>(char*, char, char const*, unsigned long, "
     "char const*, char const*)", "tolerated"),
    ("void std::deque<std::__cxx11::basic_string<char, std::char_traits<char>, "
     "std::allocator<char> >, std::allocator<std::__cxx11::basic_string<char, "
     "std::char_traits<char>, std::allocator<char> > > >::_M_push_back_aux"
     "<std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > "
     "const&>(std::__cxx11::basic_string<char, std::char_traits<char>, "
     "std::allocator<char> > const&)", "tolerated"),
    # Second: RTTI over a function type names no entity at all. These are
    # tinygltf's FsCallbacks signatures, but the record belongs to the *type*.
    ("typeinfo for bool (*)(std::__cxx11::basic_string<char, std::char_traits<char>, "
     "std::allocator<char> > const&, void*)", "tolerated"),
    ("typeinfo name for bool (*)(std::vector<unsigned char, std::allocator<unsigned char> >*, "
     "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> >*, "
     "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > "
     "const&, void*)", "tolerated"),
    ("typeinfo for bool (std::__cxx11::basic_string<char, std::char_traits<char>, "
     "std::allocator<char> > const&, void*)", "tolerated"),
    # What the "no entity" branch rests on: a bare C identifier also has no
    # namespace, but arrives undecorated and must stay a leak.
    ("mz_zip_writer_end", "leak"),
    # Return type with its own template arguments: the space inside <> is not at
    # depth zero.
    ("std::pair<int, int> nodehammer::computeBounds()", "public"),
    ("nodehammer::operator<<(std::ostream&, nodehammer::Diagnostic const&)", "public"),
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
            "NH_API by mistake, or a target skipped nh_set_visibility(): if "
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
