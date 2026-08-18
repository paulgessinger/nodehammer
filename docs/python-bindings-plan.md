# Python bindings and wheels — plan

Status: plan of record for #41's step 7. Written after the API work closed
(#41, steps 1–6), against the surface that actually shipped.

## Context

#41 established the minimal public API and left bindings outside it. What exists
today: five installed headers, an installed shared library with hidden
visibility, a package config with no `find_dependency`, and `tests/public/`
(#58) — a Catch2 suite that links `nodehammer_shared` and therefore sees exactly
what an external consumer sees.

The bindings are the second consumer of that contract and the first that is not
C++. The goal is a wheel carrying the library and its Python mirror, built
locally on macOS first, then in CI for macOS and Linux.

### Decisions taken up front

1. **The extension links the shared library**, not the static archive. The point
   is to exercise whether the public API is usable from outside; hidden
   visibility then makes "no internals" a link-time fact rather than a review
   rule. This reverses the assumption written into `conanfile.py:14-16`,
   `CMakeLists.txt:465` and `cmake/PicProbe.cmake:7`, and settles the question
   #50 left open.
2. **Zero-copy mesh access is deferred.** It needs `ir::render::Vertex`, an
   internal header, which decision 1 forbids. The public API can grow it when a
   consumer justifies it.
3. **abi3, floor Python 3.12.** nanobind's `STABLE_ABI` only takes effect from
   3.12 in linked builds (it needs `PyType_FromMetaclass`) and is *silently
   ignored* below that. One wheel per platform-arch instead of one per version.
4. **`Config.to_dict()` is deferred.** `configToToml` is internal
   (`src/config/config_writer.hpp:12`); promoting it would be new public surface
   with no consumer. dict → `Config` works; the reverse waits.
5. **The CLI is deferred to its own step.** There is no `cli::run` today —
   `src/cli/main.cpp:19` is a plain `main()` with global `registerCmdX`
   registrars — and every command TU includes internal headers, so it cannot be
   compiled into the extension while linking the `.so`. Making it work means
   moving the command TUs into `NH_CORE_SOURCES` so they compile *inside* the
   library. That is a real refactor, and the natural place to fix `nodehammer -V`.
6. **Wheels**: linux x86_64 + aarch64, macOS arm64. Canary on PR, full matrix on
   `v*` tags, attached to the existing GitHub release.

---

## Part 1 — The binding surface

Mirror the public API exactly, snake_case for members. Nothing invented, nothing
internal reachable. The enforcement is the same one `tests/public/` relies on
(`tests/CMakeLists.txt:190-195`): `nodehammer_lib` carries `NH_STATIC` PUBLIC and
`nodehammer_shared` carries `NH_EXPORTS` PRIVATE, so a target linking the shared
library inherits neither and sees the `NH_API` spelling an installed consumer
sees. An internal header still *compiles* in-tree; it does not link.

### Layout

```
pyproject.toml
python/nodehammer/__init__.py     # re-exports, __version__, dict-config sugar
python/nodehammer/py.typed
src/python/CMakeLists.txt
src/python/bindings.cpp           # the path docs/public-api-sketch.md:4 already names
tests/python/                     # pytest mirror of tests/public/
```

`src/python/` matches how every other C++ area is organized (`src/cli/`,
`src/api/`, `src/lua/`, `src/web/`). The Python package goes in a new top-level
`python/` rather than `src/`, because `src/` in this tree means C++ TUs compiled
into the library. `REUSE.toml`'s `path = "**"` covers all of it with no edit.

### Names

| C++ | Python |
|---|---|
| `SemanticScene::read(path, {format})` / `read(span<byte>)` | `SemanticScene.read(path_or_bytes, format="")` |
| `SemanticScene::toNhb()` / `RenderScene::toNhr()` | `.to_nhb()` / `.to_nhr()` → `bytes` |
| `nodeCount` / `logVolCount` / `shapeCount` / `materialCount` / `meshCount` / `triangleCount` | `node_count` / `log_vol_count` / … |
| `DiagnosticList::hasErrors()` | `.has_errors()` |
| `Diagnostic::Severity` | `Diagnostic.Severity` |
| `applySelection` / `deduplicate` / `tessellate` / `build` | `apply_selection` / `deduplicate` / `tessellate` / `build` |
| `Config::read` / `parse` / `check` / `checkString` | `Config.read` / `.parse` / `.check` / `.check_string` |
| `version()` | `nodehammer.version()`, and `__version__` |

### Type mapping — the six that need a decision

- **`std::span<const std::byte>` in** (`SemanticScene::read`, `RenderScene::read`):
  a lambda taking `nb::bytes`. The span is consumed within the call, so there is
  no lifetime to extend.
- **`std::vector<std::byte>` out** (`to_nhb` / `to_nhr`): copy into `nb::bytes`.
- **`std::span<const std::string_view>` out** (three `formats()`): copy to
  `list[str]`. The C++ side returns a view over library-lifetime storage
  (`src/api/semantic_scene.cpp:87-100`).
- **`std::filesystem::path`**: `nb::stl/filesystem.h` — `str` and `os.PathLike`.
- **Nested `ReadOptions` / `WriteOptions`** (three single-field structs, each a
  `std::string format`): a `format=""` keyword instead of binding the structs. A
  deliberate, documented delta — the C++ shape exists to carry a defaulted
  trailing parameter, which Python does not need.
- **`SemanticScene::read(TGeoManager&)`**: **not bound.** No ROOT in the wheel,
  and it is the one entry point whose definition is build-conditional
  (`src/api/semantic_scene.cpp:71-81`), so referencing it would be an undefined
  symbol at the extension's link.

### Errors

`docs/error-model.md` maps directly. `nodehammer::Error` is the only exported
*type* (`NH_API_TYPE`, so `catch` matches across the `.so`); a translator
produces `nodehammer.Error` carrying `code`, `context`, `observed` and
`diagnostic()`. Everything else propagates unchanged, `MemoryError` included.

**Sharp edge:** `Error::observed()` returns a span borrowed from the exception
object (`src/api/diagnostics.cpp:20-21`). The translator must **copy** it — the
span dies with the C++ exception, and a Python object holding it would dangle.

`Fatal` never appears in a returned list, so Python inherits that invariant.

### GIL

Released on everything that does I/O or real work — `read`, `write`, `to_nhb` /
`to_nhr`, `apply_selection`, `deduplicate`, `tessellate`, `build`. `build` is
minutes on a real detector. Held for the counts, `valid()` and `formats()`.

### Config from a dict (#41 §7)

Pure Python, no added C++ surface: `dict` → TOML text → `Config.parse(src, base_dir)`.
Per #52's rule, an unspecified `base_dir` means *no location*, not the working
directory — the Python sugar says so rather than inheriting a default.

---

## Part 2 — Build wiring

**Toolchain**: scikit-build-core + nanobind + Conan 2.

### Conan runs first, explicitly

`cmake-conan`'s `conan_provider.cmake` is rejected on its own documented
limitation: it supports only `CMakeConfigDeps` and tells you to run
`conan install` separately for "build settings that would otherwise be provided
by `CMakeToolchain`". Here that is disqualifying — `compiler.cppstd=23`, `libcxx`
and `CMAKE_OSX_DEPLOYMENT_TARGET` must reach both the dependencies and our own
build, with nothing to enforce agreement if they diverge.

So `conan install` runs first and its `conan_toolchain.cmake` reaches
scikit-build-core through the `CMAKE_TOOLCHAIN_FILE` environment variable (it is
an absolute machine-specific path, so it cannot live in `pyproject.toml`). The
cost is that a bare `pip install .` does not work standalone. Accepted; a `just`
recipe runs both halves, and it mirrors how CI already works
(`ci/build.sh conan-install` then `configure`).

### Three build-time properties the wheel depends on

These are the non-obvious ones. All three are properties of how the library is
*built*, not of how the wheel is *packaged*, and none can be repaired afterwards.

1. **No `SOVERSION` in a wheel build.** A wheel is a zip and pip extracts with
   `zipfile`, which does not restore symlinks, so the
   `libnodehammer.so → .so.0 → .so.0.1.3` chain cannot survive. Flattening it at
   install time does not work either: what the extension records in `DT_NEEDED` /
   `LC_LOAD_DYLIB` is the *soname* — the middle link, precisely the file that is
   a symlink in a normal install. Dropping `VERSION`/`SOVERSION` makes the soname
   `libnodehammer.so` / `@rpath/libnodehammer.dylib`, the linker emits one real
   file, and nothing needs a symlink.

2. **`INSTALL_RPATH_USE_LINK_PATH OFF` on both targets.** It is `ON` globally
   (`CMakeLists.txt:64`) for the ROOT/DD4hep case, where a `DT_NEEDED` reference
   must be findable on the machine that built it. A wheel is the opposite: every
   dependency is a static archive absorbed into the `.so`, there is nothing left
   to point at, and an absolute build-machine path in `RUNPATH` is the one thing
   auditwheel and delocate cannot fix up. The only entry should be `$ORIGIN` /
   `@loader_path`.

3. **The library is installed *into* the package directory.** auditwheel treats
   a library already inside the wheel as internal and leaves it alone — no copy,
   no hash-mangled SONAME. Install it to a system prefix instead and it gets
   grafted into `nodehammer.libs/` under a mangled name. delocate behaves the
   same way.

### Install component

Every existing install rule is tagged `COMPONENT Runtime` or `Development`
(`CMakeLists.txt:786-903`), but `Runtime` also carries the CLI executable. The
extension and its copy of the library get a third component, `Python`, selected
by `tool.scikit-build.install.components` — a positive statement of the payload
rather than a list of exclusions. `build.targets` likewise narrows the build, so
a wheel does not compile the CLI and the static archive for nothing.

### Version

`cmake/Version.cmake` derives from `git describe` and produces
`0.1.3-382-gd71418c`, which is not PEP 440 — and `0.0.0` when tags are absent,
which is what CI produces today (`actions/checkout` is shallow; visible in the
consumer's own output, `nodehammer 0.0.0: 1 nodes, 12 triangles`).

Rather than teach `Version.cmake` PEP 440, the wheel version comes from
`setuptools_scm` and is pushed *down* into CMake: scikit-build-core always
defines `SKBUILD_PROJECT_VERSION`, and `Version.cmake` prefers it when present.
One source of truth per build — and it also fixes sdists, which have no `.git`
and would otherwise configure as `0.0.0` and ship a library whose
`nodehammer::version()` disagrees with the metadata of the wheel containing it.

Use `version_scheme = "no-guess-dev"`: the default would bump the patch component
(`0.1.3` + 382 commits → `0.1.4.dev382`), naming a tag that does not exist.

Wheel jobs need `fetch-depth: 0`.

---

## Part 3 — CI

`cibuildwheel` with `build = "cp312-*"`, so one interpreter produces the single
abi3 wheel per platform. It runs `auditwheel repair` / `delocate-wheel`
automatically, and additionally runs **abi3audit** on abi3 wheels — worth having,
since a hand-written C++ library behind nanobind is exactly where accidental
non-limited-API usage would creep in.

- **Image**: `manylinux_2_28` — AlmaLinux 8 with **gcc-toolset-14**, which meets
  this project's gcc-14 floor. Its `libstdc++_nonshared.a` mechanism links the
  *new* libstdc++ symbols statically while keeping a plain
  `DT_NEEDED libstdc++.so.6`, which is what lets a C++23 build run on an old base
  system. `libstdc++.so.6` is on auditwheel's allowlist and is never vendored.
- **Conan cache**: `before-all` runs once per platform inside the container and
  does the `conan install`; `CONAN_HOME` points into cibuildwheel's `/host` mount
  so the existing `actions/cache` Conan blobs are reused. The Conan cache is not
  concurrency-safe — one per job.
- **macOS**: `MACOSX_DEPLOYMENT_TARGET` must be identical across Conan
  dependencies, `libnodehammer`, and the wheel tag. Since delocate 0.11 the
  repair step *verifies* this and fails on a mismatch. Note the split:
  scikit-build-core reads the environment variable to compute the wheel tag,
  while the compile uses `CMAKE_OSX_DEPLOYMENT_TARGET` from the Conan toolchain.
- **Cadence**: one canary identifier on PR (`--only cp312-manylinux_x86_64`), the
  full three-wheel matrix on `v*` tags. The `release` job
  (`.github/workflows/ci.yml:389-401`) downloads all artifacts and uploads
  `artifacts/**/*`, so wheels ride along — but it declares `needs: build`, so the
  wheel job must join that list or the release will race it.

### The one risk that could block Linux wheels

auditwheel enforces a symbol-version ceiling of **`GLIBCXX_3.4.24`** (GCC 7-era)
on `manylinux_2_28`; anything newer must come from `libstdc++_nonshared.a` rather
than as a dynamic reference. `<print>` is the exposure — `std::println` resolves
to `std::vprint_unicode`, which lives in the shared library at `GLIBCXX_3.4.32`.

**Resolved ahead of the wheel, because it was a bug in its own right.** The
exposure was two core TUs — seven `std::println(stderr, …)` in
`TessellationJob::take()` and five more in the DD4hep importer — and step 1
demonstrated what that means for a consumer: `import nodehammer; build(...)`
printed six lines of tessellation stats to a Python session that never asked for
them. A library does not get to decide its caller wants output.

Both now report through the diagnostics channel instead (`NH0510`, `NH0104`),
which is the pattern `take()` was already using two lines below for the
coincident-face counts. No core TU calls `std::println` any more, so the
`GLIBCXX_3.4.32` reference is gone before auditwheel ever sees it, and the
compiler floor drops to gcc 13. The CLI output improved as a side effect: it was
already printing its own summary line, so the six-line block was duplicating it.

`auditwheel show` on the first Linux wheel is still the check — this removes the
one known reference, not the possibility of others.

---

## Verification

- **Mirror `tests/public/` in pytest**, case for case. That suite already asserts
  what an external consumer sees; a Python twin proves the surface survives a
  second boundary. `SemanticScene.read("", format="synthetic")` builds a one-box
  scene (`tests/public/test_public_semantic_scene.cpp:23-32`) and `SceneConfig`
  is default-constructible and usable (`test_public_build.cpp:32-47`), so most
  cases need no fixture files.
- **Python vs CLI byte-identical export** — #41's step-7 criterion. dict → TOML →
  `Config` → `build` → `write`, compared against `nodehammer convert`.
  `tests/api/test_api_equivalence.cpp` is the C++ precedent.
- **The wheel is inspected, not assumed**: `python -m zipfile -l` (no symlinks, no
  `bin/`, no `include/`, no `lib/cmake/`), `otool -L`/`-l` or `readelf -d` (one
  rpath entry, and it is relative), `auditwheel show` / `delocate-listdeps`.
- **A fresh-venv install test**: `pip install <wheel>` into an empty environment
  and run the suite against the *installed* package, not the build tree. This is
  the Python equivalent of `ci/shared_consumer` and the only thing that proves the
  rpath and the bundled library are right.
- **`STABLE_ABI` actually engaged**: `nanobind_add_module` silently degrades to a
  non-abi3 module if `Development.SABIModule` was not found. Assert the installed
  file is `_nodehammer.abi3.so` and the tag is `cp312-abi3`, so it cannot regress
  silently.

## Sequencing

| # | Step | Verified by |
|---|---|---|
| 1 | Bindings + `NODEHAMMER_BUILD_PYTHON` wiring, built in-tree against `nodehammer_shared` | the pytest mirror of `tests/public/`, against the build tree |
| 2 | `pyproject.toml` + scikit-build-core + Conan pre-step; local macOS wheel | fresh-venv install, suite green against the installed package; wheel contents inspected |
| 3 | Linux wheel via cibuildwheel in a container | `auditwheel show` clean at `manylinux_2_28`; the `<print>` question settled |
| 4 | CI: canary on PR, matrix on tags, wheels on the release | green PR canary; a tag producing three wheels |

Each step is independently revertable, and step 1 lands with no packaging in the
tree at all.

## Open questions to settle while building

- **`nh_set_compiler_options` on `bindings.cpp`** — `-Wconversion -Wsign-conversion
  -Wshadow -Wpedantic` against nanobind's headers stays quiet only if nanobind
  marks its interface includes `SYSTEM`. Configure once with `NODEHAMMER_WERROR=ON`
  to find out; if it is noisy, mark those includes SYSTEM rather than dropping the
  helper.
- **`FetchContent` fallback inside a wheel build** — `cmake/Dependencies.cmake`
  uses `FIND_PACKAGE_ARGS`, so a Conan package that fails to resolve is silently
  downloaded and built from git. In a wheel that is an unpinned network
  dependency. Configure once with `FETCHCONTENT_FULLY_DISCONNECTED=ON`; it must
  succeed.
- **Conan toolchain vs `FindPython`** — `CMakeToolchain` sets
  `CMAKE_FIND_PACKAGE_PREFER_CONFIG` and `CMAKE_PREFIX_PATH`, which can make
  `find_package(Python)` pick a different interpreter than the one pip is building
  for. Print `Python_EXECUTABLE` and confirm it matches.
- **Dropping `VERSION`/`SOVERSION` under `NODEHAMMER_BUILD_PYTHON`** also changes a
  *system* install if both options are ever set in one configure. Warn, or scope
  it more tightly.
