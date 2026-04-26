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
    default_options = {"viewer": False}

    tool_requires = ("flatbuffers/25.9.23",)

    def requirements(self):
        self.requires("zstd/1.5.6")
        self.requires("catch2/3.7.1")
        self.requires("cli11/2.4.2")
        self.requires("nlohmann_json/3.12.0")
        self.requires("glm/1.0.1")
        self.requires("tomlplusplus/3.4.0")
        self.requires("tinygltf/2.9.7")
        self.requires("flatbuffers/25.9.23")
        self.requires("unordered_dense/4.8.1")
        # manifold's upstream CMake early-returns under Emscripten, skipping
        # the install/export rules. We patch that out via a vendored recipe
        # under recipes/manifold/ — `conan export` of that recipe runs in the
        # build flow (see Justfile / ci.yml), and Conan resolves to our local
        # revision. With the patch applied, manifold's Conan package works
        # for both native and wasm and we can drop the FetchContent fallback.
        self.requires("manifold/3.2.1")
        # Viewer-only runtime deps. The CCI sdl/3.x recipe silently builds an
        # empty libSDL3.a on the emscripten host profile — falling back to
        # FetchContent for wasm. ImGui is intentionally NOT here either: its
        # backend sources (imgui_impl_sdl3.cpp etc.) live in res/bindings,
        # which is awkward to wire up vs. add_subdirectory-style FetchContent.
        if self.options.viewer and self.settings.os != "Emscripten":
            self.requires("sdl/3.2.20")

    def build_requirements(self):
        # When the viewer is on, pull bgfx as a TOOL requirement so we get a
        # native-built shaderc/bin2c on PATH. tool_requires always uses the
        # build profile, so this works identically for native and emscripten
        # cross-compiles. The bgfx RUNTIME we link against still comes from
        # FetchContent (cmake/Dependencies.cmake) — the CCI bgfx recipe is
        # currently broken on emscripten, but we never link its host-context
        # output, so that doesn't bite us.
        if self.options.viewer:
            self.tool_requires("bgfx/1.129.8930-495", options={"tools": True})

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
