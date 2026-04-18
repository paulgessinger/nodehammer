from conan import ConanFile
from conan.tools.cmake import cmake_layout


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
        # manifold/3.2.1's upstream CMake early-returns under EMSCRIPTEN, which
        # skips header install, leaving the conan package with a broken
        # INTERFACE_INCLUDE_DIRECTORIES. Fall back to FetchContent on wasm; the
        # native package is fine (and pulls clipper2 as a transitive).
        if self.settings.os != "Emscripten":
            self.requires("manifold/3.2.1")

    def configure(self):
        self.settings.compiler.cppstd = "23"

    def layout(self):
        cmake_layout(self)
