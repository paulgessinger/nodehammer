from conan import ConanFile
from conan.tools.cmake import cmake_layout


class Nodehammer(ConanFile):
    name = "nodehammer"
    settings = "os", "compiler", "build_type", "arch"
    generators = "CMakeDeps", "CMakeToolchain"

    requires = (
        "zstd/1.5.6",
        "catch2/3.7.1",
        "cli11/2.4.2",
        "nlohmann_json/3.12.0",
        "glm/1.0.1",
        "tomlplusplus/3.4.0",
        "tinygltf/2.9.7",
        "flatbuffers/25.9.23",
        "manifold/3.2.1",
    )
    tool_requires = ("flatbuffers/25.9.23",)

    def configure(self):
        self.settings.compiler.cppstd = "23"

    def layout(self):
        cmake_layout(self)
