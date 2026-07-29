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
        # Lua scripting config front-end (the `config-lua` CLI command). Lua is
        # a small C interpreter; sol2 is its header-only C++ binding (and pulls
        # lua transitively, but we pin lua explicitly for parity with the CMake
        # find_package). Native-only: the front-end lives in nodehammer_lua,
        # linked solely by the CLI + tests, never by the wasm closure — so skip
        # the deps entirely under Emscripten and keep them out of the .wasm.
        if self.settings.os != "Emscripten":
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
            # ZIP read/write backing ZipWorkingSet (strategy doc step 5) and the
            # native ArchiveProjectFs (step 6). Unlike watcher/platformfolders
            # this is viewer-only but *not* native-only: the future web bag
            # (step 8, ZIP-in-IDB) reuses ZipWorkingSet, so miniz must build for
            # Emscripten too. miniz is plain C with no OS deps, so it does.
            self.requires("miniz/3.0.2")
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
