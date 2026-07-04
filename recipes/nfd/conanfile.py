from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy, get, rmdir
import os


class NativeFileDialogExtendedConan(ConanFile):
    name = "nfd"
    description = "Small C library with a native file dialog on each platform."
    license = "Zlib"
    homepage = "https://github.com/btzy/nativefiledialog-extended"
    url = "https://github.com/btzy/nativefiledialog-extended"
    topics = ("dialog", "file-picker", "native")

    package_type = "library"
    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "portal": [True, False],
    }
    default_options = {"shared": False, "fPIC": True, "portal": True}
    implements = ["auto_shared_fpic"]

    def layout(self):
        cmake_layout(self, src_folder="src")

    def source(self):
        get(self, **self.conan_data["sources"][str(self.version)], strip_root=True)

    def validate(self):
        if self.settings.os == "Emscripten":
            raise ConanInvalidConfiguration("nfd is native-only; the web viewer uses browser file APIs.")
        check_min_cppstd(self, 11)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["NFD_BUILD_TESTS"] = False
        tc.cache_variables["NFD_PORTAL"] = bool(self.options.portal)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "nfd")
        self.cpp_info.set_property("cmake_target_name", "NFD::nfd")
        self.cpp_info.libs = ["nfd"]
        if self.settings.os == "Linux":
            self.cpp_info.system_libs.extend(["dbus-1"])
        elif self.settings.os == "Windows":
            self.cpp_info.system_libs.extend(["ole32", "uuid", "shell32"])
        elif self.settings.os == "Macos":
            self.cpp_info.frameworks.extend(["AppKit", "UniformTypeIdentifiers"])
