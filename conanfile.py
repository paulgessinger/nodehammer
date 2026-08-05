import json
from pathlib import Path

from conan import ConanFile
from conan.tools.cmake import cmake_layout
from conan.tools.files import copy


class Nodehammer(ConanFile):
    name = "nodehammer"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    options = {"viewer": [True, False]}
    # Every dependency archive that can be linked into a shared object — the
    # planned installed shared library, and the Python extension, which links
    # the static nodehammer_lib into a .so — has to be position independent.
    # Most recipes already default fPIC=True on Linux/macOS, but that is their
    # default and not a guarantee; pinning it makes the requirement explicit and
    # part of the package_id, so a profile that flips it forces a rebuild rather
    # than silently reusing a non-PIC binary. Pattern-scoped options are not
    # strict in Conan 2, so packages that have no fPIC option (header-only ones)
    # or that delete it (Windows, or shared=True) are skipped rather than
    # erroring. The in-tree side of the same requirement is
    # CMAKE_POSITION_INDEPENDENT_CODE in CMakeLists.txt.
    default_options = {"viewer": False, "*/*:fPIC": True}

    tool_requires = ("flatbuffers/25.12.19",)

    def requirements(self):
        self.requires("zstd/1.5.7")
        self.requires("catch2/3.15.1")
        self.requires("cli11/2.6.2")
        self.requires("nlohmann_json/3.12.0")
        self.requires("glm/1.0.3")
        self.requires("tomlplusplus/3.4.0")
        self.requires("tinygltf/2.9.7")
        self.requires("flatbuffers/25.12.19")
        self.requires("unordered_dense/4.8.1")
        # Plain ConanCenter package — manifold's own CMake now runs its
        # install/export rules unconditionally under Emscripten, so the local
        # patched recipe nodehammer used to carry (recipes/manifold/) is no
        # longer needed. ConanCenter is one patch release behind upstream
        # (3.5.1 vs 3.5.2); reintroduce a local recipe here if we need
        # something ConanCenter hasn't packaged yet.
        self.requires("manifold/3.5.1")
        # ZIP read/write backing ZipWorkingSet, ArchiveProjectFs and archive
        # export — i.e. reading and writing .nhproj, which is a capability of the
        # library rather than of the viewer that first needed it.
        #
        # Unconditional on purpose. It used to sit under `if self.options.viewer`,
        # which made the *installed shared library* vary with an application
        # feature flag: a viewer-off build produced a libnodehammer that could not
        # open a project. Since the library is what a consumer links, its feature
        # set has to be a property of the library. miniz is the cheapest possible
        # dep to make unconditional — plain C, no OS deps, builds for Emscripten
        # — and static-lib dead-strip keeps it out of any binary that does not
        # reference it.
        self.requires("miniz/3.0.2")
        # Lua scripting config front-end: the `config-lua` CLI command and
        # `Config::read`'s `.lua` branch. Lua is a small C interpreter; sol2 is
        # its header-only C++ binding (and pulls lua transitively, but we pin lua
        # explicitly for parity with the CMake find_package).
        #
        # Unconditional for the same reason miniz is: which formats a library can
        # read is a property of the library, not of the platform it happens to be
        # built for. Gated, `Config::formats()` answered differently in a browser
        # than on a desktop — for a build accident rather than a constraint, since
        # the interpreter compiles and runs perfectly well under Emscripten (see
        # `configure`). Static-lib dead-strip keeps it out of any binary that
        # never calls the `.lua` path, which today is both wasm bundles.
        self.requires("lua/5.4.6")
        self.requires("sol2/3.5.0")
        # Viewer-only runtime dep. PlatformFolders resolves the OS-appropriate
        # cache / config directory on native; not needed on web. Skip under
        # Emscripten — the wasm viewer never links it and the package fails to
        # build for that target anyway.
        if self.options.viewer and self.settings.os != "Emscripten":
            self.requires("platformfolders/4.3.0")
            # Cross-platform directory watcher (FSEvents/inotify/RDCW) backing
            # WatchedFilesystemProjectFs (strategy doc step 4). Header-only;
            # native + viewer-only like platformfolders — the web bag has no
            # on-disk tree to watch.
            self.requires("watcher/0.14.1")
        if self.options.viewer:
            self.requires("sokol/2026.07.02")
            self.requires("imgui/1.92.8")
            self.requires("implot/1.0.0")
            if self.settings.os != "Emscripten":
                self.requires("nfd/1.3.0")

    def build_requirements(self):
        # sokol-shdc is the shader compiler: a prebuilt static binary wrapped
        # by the local recipe under recipes/sokol-shdc. tool_requires resolves
        # against the build profile, so cross-compiling to Emscripten still
        # picks the host's binary (verified by Justfile passing `-pr:b default`
        # to wasm-deps). Re-exported by `just recipes` alongside manifold.
        if self.options.viewer:
            self.tool_requires("sokol-shdc/2026.06.13")

    def configure(self):
        self.settings.compiler.cppstd = "23"
        # Lua raises errors with setjmp/longjmp when the interpreter is built as
        # C. Emscripten implements that as JS-based SjLj, which is mutually
        # exclusive with the native wasm exceptions this project compiles and
        # links with everywhere (profiles/emscripten) — so the archive asks for
        # `emscripten_longjmp` and the final link, being wasm EH, has no such
        # symbol. That is the whole of what "no lua in the wasm build" ever was.
        #
        # Built as C++, LUAI_THROW is a real `throw`: there is no longjmp left to
        # support, and the profile's -fwasm-exceptions reaches the interpreter
        # like any other C++ source. Its exported symbols keep C linkage
        # (lua.h declares its own extern "C" under a C++ compiler), so sol2 and
        # src/lua/lua_config.cpp are unaffected.
        #
        # Scoped to Emscripten deliberately. Native builds have shipped on the C
        # interpreter with nothing to fix, and `compile_as_cpp` is part of lua's
        # package_id, so widening this would rebuild it everywhere for no gain.
        if self.settings.os == "Emscripten":
            self.options["lua"].compile_as_cpp = True

    def layout(self):
        cmake_layout(self)

    def generate(self):
        # Stage every host dependency's license files under the generators
        # folder, plus a manifest. CMake reads the manifest at configure time,
        # ships the per-dep folders + a concatenated THIRD-PARTY-NOTICES.txt.
        # Transitive deps (clipper2 via manifold, stb via tinygltf, ...) are
        # included automatically — Conan's graph is the source of truth.
        licenses_root = Path(self.generators_folder) / "third_party_licenses"
        licenses_root.mkdir(parents=True, exist_ok=True)

        manifest = []
        for req, dep in self.dependencies.host.items():
            if req.build or req.test:
                continue
            if not dep.package_folder:
                continue
            src = Path(dep.package_folder) / "licenses"
            if not src.exists():
                continue
            slot = f"{dep.ref.name}-{dep.ref.version}"
            dst = licenses_root / slot
            copy(self, "*", str(src), str(dst))
            files = sorted(
                str(p.relative_to(dst)).replace("\\", "/")
                for p in dst.rglob("*")
                if p.is_file()
            )
            if not files:
                continue
            raw_license = getattr(dep, "license", None)
            if isinstance(raw_license, (list, tuple)):
                license_str = " OR ".join(str(x) for x in raw_license)
            else:
                license_str = str(raw_license) if raw_license else ""
            manifest.append(
                {
                    "name": dep.ref.name,
                    "version": str(dep.ref.version),
                    "license": license_str,
                    "dir": slot,
                    "files": files,
                }
            )

        manifest.sort(key=lambda e: e["name"])
        (licenses_root / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
