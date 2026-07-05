from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeDeps, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get
import os


class ImPlotConan(ConanFile):
    name = "implot"
    description = "Immediate mode plotting widgets for Dear ImGui."
    license = "MIT"
    homepage = "https://github.com/epezent/implot"
    url = "https://github.com/epezent/implot"
    topics = ("gui", "plotting", "imgui")

    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {"shared": [True, False], "fPIC": [True, False]}
    default_options = {"shared": False, "fPIC": True}
    implements = ["auto_shared_fpic"]
    exports_sources = "CMakeLists.txt"

    def layout(self):
        cmake_layout(self, src_folder="src")

    def requirements(self):
        self.requires("imgui/1.92.8")

    def source(self):
        get(self, **self.conan_data["sources"][str(self.version)], strip_root=True)
        copy(self, "CMakeLists.txt", self.export_sources_folder, self.source_folder)

    def validate(self):
        check_min_cppstd(self, 11)

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()
        tc = CMakeToolchain(self)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "implot")
        self.cpp_info.set_property("cmake_target_name", "ImPlot::ImPlot")
        self.cpp_info.libs = ["implot"]
