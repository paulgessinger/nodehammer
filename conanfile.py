import json
from pathlib import Path

from conan import ConanFile
from conan.tools.cmake import cmake_layout
from conan.tools.files import copy


class Nodehammer(ConanFile):
    name = "nodehammer"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

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
        # manifold/3.2.1's upstream CMake early-returns under EMSCRIPTEN, which
        # skips header install, leaving the conan package with a broken
        # INTERFACE_INCLUDE_DIRECTORIES. Fall back to FetchContent on wasm; the
        # native package is fine (and pulls clipper2 as a transitive).
        if self.settings.os != "Emscripten":
            self.requires("manifold/3.2.1")
        else:
            # Emscripten still needs manifold's license + transitives for the
            # NOTICES file. visible/headers/libs=False keeps Conan from
            # generating CMakeDeps for it (so the FetchContent target wins),
            # but the package is still downloaded and its licenses/ folder is
            # accessible to generate().
            self.requires(
                "manifold/3.2.1",
                visible=False, headers=False, libs=False,
            )

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
                for p in dst.rglob("*") if p.is_file()
            )
            if not files:
                continue
            raw_license = getattr(dep, "license", None)
            if isinstance(raw_license, (list, tuple)):
                license_str = " OR ".join(str(x) for x in raw_license)
            else:
                license_str = str(raw_license) if raw_license else ""
            manifest.append({
                "name": dep.ref.name,
                "version": str(dep.ref.version),
                "license": license_str,
                "dir": slot,
                "files": files,
            })

        manifest.sort(key=lambda e: e["name"])
        (licenses_root / "manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n"
        )
