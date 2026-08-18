# The CLI in the library, the viewer on the web — plan

Status: plan of record. Written after the bindings (#67) and the wheel (#68)
landed, while the TestPyPI publish (#69) is open. It picks up decision 5 of
`docs/python-bindings-plan.md` — which deferred the CLI — and joins it to the
feature that turns out to depend on it.

## Why this is one plan

Two goals that look independent:

- **`nodehammer viewer --web`**: open the wasm viewer in a browser from any
  install, so a machine with no window system — or a build with no native viewer
  compiled in — still has a GUI. This is the case where nodehammer is built
  statically on a remote box and the display is the laptop in front of you.
- **A GUI from Python**: `pip install nodehammer`, call one function, get the
  viewer, with no platform-specific code beyond what the wheel already carries.

They are one feature behind two front doors. Both need code that resolves the
wasm runtime, stages a servable directory, serves it, and opens a browser — and
neither can reach that code where the CLI lives today. The wheel installs no
executable (`pyproject.toml`'s `install.components = ["Python"]`), and the CLI is
a static archive that cannot be linked into the extension. So the CLI refactor is
not a parallel workstream; it is step 0 of the viewer work.

### Decisions taken up front

1. **The CLI moves into the shared library** — not into a static archive the
   extension links. `nodehammer_cli` (`CMakeLists.txt:675`) carries
   `nodehammer_lib` PUBLIC, the *static* core, so linking it into an extension
   that already links `nodehammer_shared` puts two copies of the core in one
   process: duplicate globals, and two `type_info`s for `nodehammer::Error`, so a
   `catch` across that boundary silently misses. Relinking it against the shared
   library is not available either — every command TU includes internal headers
   (`config/config_loader.hpp`, `ir/semantic/importer.hpp`,
   `selection/selector.hpp`, `detail/file_io.hpp`) whose symbols are hidden in
   the `.so` by design; compiled outside the library they are undefined at link.
   That is the boundary working exactly as `tests/public/` proves it does.
2. **`viewer --web` is compiled unconditionally; plain `viewer` stays
   native-only.** No silent fallback either way: a build without the native
   viewer answers plain `viewer` by naming the flag that does work.
3. **The runtime is located by a ladder, and Python only supplies a rung.** No
   Python-specific serving logic in the final state.
4. **The server is C++, non-blocking, handle-based.** Blocking is a property of
   the front door, not of the API.
5. **The wasm ships as its own `py3-none-any` wheel, and `nodehammer` depends on
   it by default** — not through an extra. The `[web]` extra stays declared and
   empty so `pip install "nodehammer[web]"` keeps resolving.
6. **The filesystem bridge is deferred** and sketched only (Part 4). It is the
   piece that would make `--web` a full peer of the native viewer rather than a
   viewer of publications.

---

## Part 0 — `cli::run`

### What exists

`src/cli/main.cpp:18` is a plain `main()` that calls a series of free
`registerCmdX(CLI::App &)` registrars, each defined in its own TU. Those TUs are
built into `nodehammer_cli`, a static archive (`CMakeLists.txt:675-688`) linking
`nodehammer_lib` and `CLI11::CLI11`. The executable links that archive
(`CMakeLists.txt:778`); the viewer build instead compiles `main.cpp` together
with `cmd_viewer.cpp` and links the archive plus a viewer library
(`CMakeLists.txt:722-723`). `nodehammer_shared` is built from `NH_CORE_INPUT`
only (`CMakeLists.txt:550`) and therefore contains none of this.

### The shape

- The command TUs join the core source list, so they compile *inside* the
  library, where internal symbols are visible.
- A new `src/cli/run.cpp` exports one thing:

  ```cpp
  namespace nodehammer::cli {
      int run(std::span<const std::string_view> args, const RunOptions & = {});
  }
  ```

  in a sixth installed header, `include/nodehammer/cli.hpp`. CLI11 does not
  appear in that header — the signature is `string_view`s and an `int`, so the
  dependency stays an implementation detail.
- `src/cli/main.cpp` becomes a few lines over `cli::run`.
- **The executable keeps linking the static core.** That is what lets it register
  the native viewer command through an *internal* header, which an installed
  header could not carry: `CLI::App &` is not public surface.

### The viewer command splits, and that is forced

`src/cli/cmd_viewer.cpp` includes `viewer/app.hpp` and five `viewer/*_project_fs.hpp`
headers — sokol, Dear ImGui, and a window system. Compiling that into the shared
library drags the native viewer and its X11/GL link dependencies into every
consumer of `libnodehammer`, the wheel included, which is a manylinux problem
rather than a size problem. So:

- `cmd_viewer_native.cpp` — stays outside the library, gated on `NH_WITH_VIEWER`,
  linked only into the executable.
- `cmd_viewer_web.cpp` — inside the library, unconditional, no windowing
  dependencies at all.

This is the same cut decision 2 wants for its own reasons, which is some evidence
it is the right one. The consequence is that the CLI TUs are compiled twice, once
per linkage; they never meet in one process, so the duplication is textual rather
than a runtime hazard.

### `std::exit` is the actual work

There are 21 calls: 15 in `cmd_viewer.cpp`, 4 in `cmd_config_lua.cpp`, and one
each in `cmd_convert.cpp` and `cmd_config_flatten.cpp`. Inside a library called
from Python, each one terminates the interpreter — no traceback, no `finally`, no
`atexit`, and an interactive session loses its kernel to a mistyped path. Every
one becomes a return code or a thrown `Error`.

Mechanical, but it is the bulk of this step, and it is why this is a refactor
rather than a file move. The side effect is worth having on its own: it puts the
CLI on the error model `docs/error-model.md` already defines for the API instead
of a second convention that only looks like one because `exit` hides it.

`CLI11_PARSE` goes with them — the macro returns from `main` on a parse error.
Inside `run` it becomes an explicit `try` / `catch (const CLI::ParseError &e)
{ return app.exit(e); }`.

### Two smaller edges

- **The pager** already gates on `Console::isTTY()` (`src/cli/pager.cpp:87`), so
  a subprocess is safe. But `nh.cli.run([...])` typed into an interactive
  terminal *is* a TTY and would page, which is wrong for an API call. `RunOptions`
  carries the switch: off for the CLI, on for the binding.
- **CLI11 becomes a compile-time dependency of the shared library.** Header-only,
  so no new link dependency — only code size. `libnodehammer_cli.a` is 4.7 MB in
  Release, but that is unstripped archive members and not a prediction; measure
  the linked `.so` before and after rather than guessing. Nothing in the wasm
  closure links the CLI today (`CMakeLists.txt:674`) and nothing here changes
  that.

### What it buys

- **The wheel gets a real command** without shipping a binary:

  ```toml
  [project.scripts]
  nodehammer = "nodehammer.__main__:main"
  ```

  where `main()` calls the bound `cli.run(sys.argv[1:])` and `sys.exit`s the
  result. No second install component, no duplicated dependency set. This is what
  removes the constraint that shaped the first draft of the viewer design.
- **`nodehammer --version` and `nodehammer.__version__` become the same code path
  by construction.** The test that currently pins them together (`tests/python/test_build.py`)
  becomes a regression test rather than the guarantee.
- **`tests/api/test_api_equivalence.cpp`** can compare CLI and API in one process
  instead of spawning one.

---

## Part 1 — Locating the runtime

One ladder, in the library, shared by every front door:

| # | rung | who uses it |
|---|---|---|
| 1 | `--web-assets <dir>` / `RunOptions::web_assets` | explicit; wins |
| 2 | `NODEHAMMER_WEB_ASSETS` | users; the console-script shim |
| 3 | `<exe>/../share/nodehammer/web` | a native install tree |
| 4 | *(future)* version-keyed download cache | `project_web_package_export` Phase 2 |

Rung 3 needs the executable's own path, which is the one platform shim this adds
and fits the existing `platform_*.{cpp,mm}` convention: `/proc/self/exe`,
`_NSGetExecutablePath`, `GetModuleFileNameW`. `argv[0]` is not reliable.

**Python supplies a rung and nothing else.** Which rung depends on the door:

| entry | how the path travels |
|---|---|
| `nh.gui.show(...)` → `web::serve(...)` | **explicit parameter.** No env var. |
| `nodehammer` console script → `cli::run(argv)` | **rung 2, set only if unset.** |

argv belongs to the user on the console-script path, so injecting a flag into it
would be magic; setting the env rung when it is empty leaves an explicit
`--web-assets` winning by the ladder's own precedence. Everything programmatic
passes the path directly, which keeps it testable — no process-global state to
set up and tear down, and no stale value inherited by a subprocess the session
spawns later.

### What has to change about the install tree

The wasm bundles install to `CMAKE_INSTALL_BINDIR` today
(`CMakeLists.txt:853-860`), and `scripts/build_pages_site.sh` reads them from an
install tree's `bin/`. Rung 3 wants `share/nodehammer/web`, holding the whole
servable payload (shell, worker script, six bundles) rather than the bundles
alone. And the wasm install tree and the native install tree are *separate* CI
artifacts, so a release tarball that carries the runtime needs them merged. That
is a real packaging step, not a rename.

### The failure message matters here

A from-source native build has no wasm anywhere, and that is the *normal* state —
the wasm is a separate Emscripten build. So `viewer --web` with nothing resolved
must print the ladder it walked, naming each directory it looked in. A bare "not
found" would read as a bug in the build the user just made.

### The runtime says what it is, and the library checks

Whatever rung answers, the resolved directory has to be *checkable*. It carries a
stamp — `nh_runtime.json`, written by the same staging step that assembles it,
holding the version it was built from and a schema id — and `web::serve` compares
it against the library's own before it serves anything.

The stamp, not the wheel metadata, is the enforcement. A pip constraint (Part 3)
only guards rung 1's Python variant; rungs 1, 2 and 3 reach a directory pip never
saw, and `--web-assets ~/some-old-checkout/build/emscripten/Release` is exactly
the mistake a person makes while debugging. Metadata cannot see it. A stamp can,
and can say which directory and which version rather than rendering a blank page.

**What is compared is a schema id, not the version.** What must actually agree is
the `.nhproj` and `nh_manifest.json` shape plus the compute worker's `postMessage`
protocol — the last of which has already broken once, silently, under Closure
(`dcc4a06`, and `scripts/check_wasm_module_linkage.py` exists because of it). So:

```
NH_WEB_RUNTIME_SCHEMA   a monotonic integer, bumped only when one of those changes
```

compiled into the library and stamped into the runtime. Comparing integers is at
once **stricter** than comparing versions — it catches same-version skew from a
hand-pointed directory, which no version check can — and **looser**, in that it
does not cry wolf on every patch release where nothing about the contract moved.
The version travels in the stamp too, but only so the message can name it.

---

## Part 2 — `viewer --web`

### Surface

```
nodehammer viewer [path] [-c KEY] [-i KEY] [camera/steer opts]
nodehammer viewer --web [path] [same] [--port N] [--host H] [--no-browser]
                        [--web-assets DIR]
```

| kind | options |
|---|---|
| shared | `path`, `-c/--config`, `-i/--input`, `--title`, the camera options — which map onto the steer cascade the web viewer already reads from the URL |
| native-only | `--width`, `--height`, `--no-vsync`, `--cull`, `--screenshot*`, `--bench*` |
| web-only | `--port`, `--host`, `--no-browser`, `--web-assets` |

Native-only options **reject** in web mode rather than being ignored. `--screenshot`
in particular would otherwise look like it had worked.

`--host` defaults to `127.0.0.1`. For the remote-machine case the honest answer is
loopback plus SSH forwarding; `--host 0.0.0.0` exists and warns.

### Staging

The served root is assembled the way `scripts/stage_wasm_viewer.sh` already
assembles it, promoted from shell into product code — which leaves the script as
the dev-loop tool it actually is rather than the definition of the payload.

| input | result |
|---|---|
| a `.nhproj` | staged as-is → viewer mode, content-locked |
| a directory, or `-c`/`-i` naming loose files | packed into a transient archive → viewer mode |
| nothing | no sidecar → application mode (empty, IndexedDB, drag-and-drop) |

Both viewer-mode cases write the `nh_manifest.json` sidecar the shell already
branches on (`docs/viewer-project-strategy.md`), so this adds no web-side code
path — it reaches the two postures that already ship.

Responses carry `Cache-Control: no-store`, for the reason `scripts/serve_nocache.py`
documents: the compute worker fetches its `.wasm` from inside a Web Worker, and
browsers do not apply a hard reload's cache bypass to worker-initiated requests,
so a rebuilt worker module is otherwise served stale against a fresh main module.

### The server

Handle-based, never `serve_forever`:

```cpp
ServerHandle h = web::serve(opts);   // binds, starts a thread, returns
h.url();  h.stop();                  // and the destructor stops it
```

Two reasons, both about the Python door:

- `gui.show()` in a notebook has to return, or the session is over.
- **Ctrl-C.** With the GIL released, SIGINT lands on the Python main thread, but a
  C++ `accept()` loop never checks `PyErr_CheckSignals()`. A blocking `serve`
  would hang until `SIGKILL`. Non-blocking makes interruption Python's problem,
  which is where it is solvable.

The CLI then writes its own wait loop around the same handle — three lines, and
blocking stays a property of the front door.

A hand-rolled static server over the ~10 known files in the staged root is a few
hundred lines and no new dependency. Reach for cpp-httplib only when the routes
stop being static, i.e. if Part 4 lands.

Browser-opening lives in C++ too, even though Python's `webbrowser` module is the
nicer implementation: the CLI needs it regardless, a remote static build has no
Python, and two implementations of "open a URL" is exactly the kind of drift that
surfaces as one platform behaving differently. `--no-browser` /
`open_browser=False` on both doors.

---

## Part 3 — The `nodehammer-web` wheel

**Payload**: the Release runtime, measured — 7.0 MB raw, **2.9 MB zipped**
(gles3 and wgpu at 2.8 MB each, compute at 1.2 MB, loaders ~160 KB). Unremarkable
for a wheel, and far from PyPI's limit.

**Built from the artifact CI already produces.** The `kind: wasm` leg of the
`build` matrix uploads `nodehammer-wasm32.tar.gz`; a new job downloads it, stages
the application-mode payload, and zips it with a pure-Python backend. **No emsdk,
no compiler, no CMake in that job**, and the payload is byte-identical to what
Pages deploys.

- `py3-none-any`, one wheel for every platform.
- **Wheel-only, no sdist.** Its sources cannot be built without emsdk, so an sdist
  would be a trap rather than a courtesy.
- Contents: the staged runtime plus a `runtime_dir()` accessor over
  `importlib.resources`. No other Python code.

**Dependency direction**: `nodehammer` depends on `nodehammer-web` outright. The
wasm never runs unless asked, so a headless install pays 2.9 MB and nothing else,
and "the GUI works after `pip install nodehammer`" is worth more than that saving.
The `[web]` extra stays *declared* and empty, so `pip install "nodehammer[web]"`
keeps resolving for anyone who typed it once.

Three consequences follow, and they are why this is not a one-line change.

**The pin loosens.** An exact `== <version>` pin plus a hard dependency means a
post-release of one half demands a matching post-release of the other, and a
locally built `nodehammer` wheel cannot be installed at all without its
same-second sibling on an index. Use `nodehammer-web == X.Y.*` — same minor, any
patch, post or dev. That is the policy this repo already applies to its own CMake
package: `COMPATIBILITY SameMinorVersion` (`CMakeLists.txt:958-963`), chosen for
the same reason spelled out in the comment there — pre-1.0, the minor is where
compatibility actually lives. What is version-coupled is the `.nhproj` and
sidecar schema, which moves far more slowly than the version does.

**Publishing becomes ordered, and atomic in one direction only.** PyPI has no
transaction, so upload `nodehammer-web` *first*. If the second upload then fails,
the index holds a depended-on package with no dependent, which is inert; the other
order leaves a dependent whose dependency is missing, which is uninstallable.
Both wheels ship from one job, so a failed wasm leg publishes neither, and the
only window in which the pair is incomplete is the seconds between two uploads in
one job.

**CI must not depend on the index at all**, or that ordering becomes a trap rather
than a detail. cibuildwheel installs the wheel under test *with* resolution,
during the build, before anything is published; with the loose pin an
already-published older `0.1.x` satisfies it, which is fine for an API-level test
but means the runtime in that environment may be stale. So the browser-facing
check does not live in `test-command`. It lives in a job that downloads both
freshly built artifacts and installs them from local files with `--no-index` —
which never consults an index, and is therefore the only place the *actual* pair
is verified.

**The dependency cannot become default before the web wheel exists on an index.**
cibuildwheel installs the wheel under test with dependency resolution — that is
the point of `test-command` running against the installed package — so a hard
dependency on something nobody has published fails *every* wheel build's test
step, on every platform, before any of this is user-visible. The same applies to
`just wheel` followed by a local `pip install`, which is today's local flow. So
the order is forced: publish `nodehammer-web` standalone, then flip the
dependency. Locally, `just wheel-web` becomes the sibling recipe and
`just wheel-test` installs both.

Extra → default would have been safe in either order. Making it the default from
the start is what turns that into a sequencing requirement.

### Keeping the constraint current

A version constraint that is typed by hand is wrong from the first minor bump, so
it should not be typed at all. Both versions are derived by setuptools_scm from
the same git state, which means the constraint can be *computed by the build that
computes the version*: a small in-repo dynamic-metadata provider
(scikit-build-core takes `provider` together with `provider-path` for a local
module) emitting `nodehammer-web == {major}.{minor}.*` from the version it just
derived. Fifteen lines, and drift stops being something to police.

One thing to confirm first: the existing `[[tool.dynamic-metadata]]` entry names
only `provider`, with no field selector, so the spelling for a *second* provider
filling `dependencies` needs checking against the installed scikit-build-core
before it is written rather than assumed from the docs of another version.

That covers generation. Two more places have to agree, and neither is metadata:

- **At release time**, the publish job asserts that the two dists in the artifact
  set carry the same version, and that the platform wheel's
  `Requires-Dist: nodehammer-web` is satisfied by the web wheel actually present.
  A generated constraint cannot catch a *stale artifact* in a re-run; this can.
- **At load time**, the stamp from Part 1. That is the only check that survives
  the three rungs pip never sees.

Stated as a rule: pip resolution keeps the *wheel pair* plausible, and the stamp
keeps the *running pair* correct. Neither substitutes for the other, and only the
second one is load-bearing.

### Two PyPI projects means two Trusted Publishers

Both owner `paulgessinger`, repo `nodehammer`, workflow `ci.yml`, environment
`testpypi`. Plan for two publish steps: the OIDC exchange mints a project-scoped
token, so one step uploading both names needs verifying before it is relied on.

---

## Part 4 — Deferred: the served posture

The web viewer has exactly two postures today, both self-contained: a locked
publication, or an application over IndexedDB and drag-and-drop. Neither can
touch a filesystem on the machine that served the page, which is what the
remote-static-build case actually wants.

The viewer already abstracts over `ProjectFs` (`FilesystemProjectFs`,
`ArchiveProjectFs`, `BagProjectFs`), so the bridge is one more implementation —
`HttpProjectFs` over `fetch()` — plus a handful of routes:

```
GET  /fs/            list keys
GET  /fs/<key>       read
PUT  /fs/<key>       write
GET  /events         SSE — replaces the native file watcher
```

Two things to decide in the design rather than discover in review: a **per-launch
random token in the URL**, without which any page in that browser can drive a
filesystem over loopback; and **writes opt-in** (`--rw`), because "serve the
viewer from a remote box" and "grant a web page write access to that box" are
different requests.

### A related deferral, worth writing down

The web viewer has no `.nhr` load path: viewer mode hands it a config and a
`.nhb`, and the browser tessellates through the compute worker. So `viewer --web`
from a native install does **not** use the fast local library for the heavy
lifting — it ships semantic geometry to the browser and pays wasm build time
there, with a fast `libnodehammer` sitting in the same install. Making Python or
the CLI build natively and the browser only render is a real win on a large
detector, but it is viewer work (a prebuilt-render-scene posture), not packaging
work. Out of scope here; not out of mind.

---

## Verification

- **`cli::run` returns, it does not exit.** A Catch2 case that runs a *failing*
  command in-process and then keeps running is the regression that matters; a
  grep for `std::exit` under `src/cli/` guards the rest. The existing CLI tests
  keep invoking the executable and should not need edits — if they do, the shim
  is not a shim.
- **The `.so` size delta** from Part 0, measured and recorded here, not estimated.
- **Fresh venv**: `pip install <wheel>` then `nodehammer --version`, compared
  against `python -c "import nodehammer; print(nodehammer.__version__)"`.
- **The staged root is self-describing**: reuse the checks
  `scripts/check_pages_site.py` already makes on a posture directory, run against
  what `viewer --web` stages. Then fetch every file in it and assert 200 — that is
  the cheapest proof the payload and the server agree, and it needs no browser.
- **Both wheels, from local files, `--no-index`**: install the two freshly built
  artifacts into a fresh venv, then `nodehammer viewer --web --no-browser`,
  asserting the printed URL serves `viewer.html` and all six bundles. This is the
  only test that exercises the ladder's Python rung end to end, and it deliberately
  never consults an index, so it verifies the pair that was just built rather than
  whatever resolution happened to find. Run it once with `--no-deps` too, to see
  the missing-runtime message rather than a traceback.
- **A deliberately mismatched pair**: point `--web-assets` at a runtime whose
  stamp carries a different `NH_WEB_RUNTIME_SCHEMA` and assert the refusal names
  the directory and both ids. This is the case no wheel metadata can reach, so an
  untested check here is an unchecked one.
- **prek**: a new `.md` needs trailing-whitespace, end-of-file-fixer and LF only;
  `REUSE.toml`'s `path = "**"` covers licensing. New C++ is clang-formatted.

---

## Sequencing

| # | Step | Verified by |
|---|---|---|
| 0 | `cli::run` into the shared library; `main.cpp` becomes a shim; `cmd_viewer` splits; `std::exit` removed | existing CLI tests unchanged; in-process failure case; measured `.so` delta |
| 1 | `[project.scripts]` console script in the wheel | fresh venv: `nodehammer --version` agrees with `__version__` |
| 2 | Asset ladder; install to `share/nodehammer/web`; CI merges the wasm tree into the native one | `viewer --web` against a staged install tree |
| 3 | Staging + static server + browser open, in the library | every staged file fetched; `check_pages_site` logic reused |
| 4 | `nodehammer-web` wheel built from the wasm artifact, published standalone | fresh venv: `pip install nodehammer-web`; `runtime_dir()` holds the shell and six bundles |
| 5 | `nodehammer` depends on it — constraint *generated*, not typed; `[web]` kept as a declared empty extra; the `--no-index` pairing job | that job green; the mismatched-stamp refusal |
| 6 | *(deferred)* served posture — `HttpProjectFs`, token, `--rw` | — |

Each step is independently revertable. Step 0 lands with no new feature in the
tree at all, which is the point: it is the step that makes the rest one design
instead of two.

---

## Open questions to settle while building

- **The flag and env names.** `--web-assets` / `NODEHAMMER_WEB_ASSETS` here;
  `project_web_package_export` wrote it as `NH_WEB_ASSETS_DIR`. Nothing in `src/`
  reads an environment variable today except the pager's `PAGER`, so this is the
  first one and the prefix is a free choice: `NODEHAMMER_` matches the CMake
  options, `NH_` matches the in-code define prefix. Pick one and write it down.
- **`--port` default**: 0 (ephemeral, printed) avoids colliding with
  `just wasm-serve`; 8000 is nicer to bookmark. Ephemeral is proposed.
- **Who bumps `NH_WEB_RUNTIME_SCHEMA`, and how it is not forgotten.** The whole
  scheme rests on an integer a person has to remember to increment, which is the
  weakest part of it. The candidates are a review rule, or deriving the id from a
  hash of the shapes it stands for (the manifest keys and the worker protocol's
  message names), which trades a forgettable step for a churny one. Undecided.
- **Does `run()` throw or return** when called as an API? Proposed: return the
  int, let the console-script shim `sys.exit` it, and reserve exceptions for
  `Error`. A notebook caller who wants a raise can check the int.
- **`nodehammer` with no arguments** currently defaults to `viewer` when the
  native viewer is compiled in (`src/cli/main.cpp:51`) and prints help otherwise.
  In a wheel there is no native viewer, so the same invocation would have to mean
  `viewer --web` or keep printing help. The two answers differ in whether a bare
  `nodehammer` opens a browser, which is a product decision, not a build one.
