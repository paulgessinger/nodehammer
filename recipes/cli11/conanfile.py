from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.files import copy, get
from conan.tools.layout import basic_layout
import os


class CLI11Conan(ConanFile):
    """CLI11, at a version conan-center does not carry yet.

    Vendored for one reason: 2.6.2 -- the newest conan-center has -- writes its
    character tables as namespace-scope references bound to temporaries
    (`inline const std::string &escapedChars(...)` in `impl/StringTools_inl.hpp`),
    and on the WebAssembly C++ ABI a destructor returns `this`, so
    `~basic_string` is a `(i32) -> i32` in the table. Clang hands *that* function
    straight to `__cxa_atexit` for a lifetime-extended temporary rather than
    wrapping it in the `void(void*)` stub a plain global gets, so musl's
    `__funcs_on_exit` calls it through a type the table disagrees with and the
    module traps with "function signature mismatch" -- after main has returned,
    on the way out. That asymmetry between a global and a reference bound to a
    temporary is llvm/llvm-project#45876, open since 2020: the fix that taught
    clang to wrap a `this`-returning destructor (`canCallMismatchedFunctionType`)
    never reached the reference-temporary path.

    CLI11 2.7.0 moved those tables into functions returning a reference to a
    by-value static (upstream #1335), which registers the properly typed stub and
    takes the trap with it. See issue #76.

    Header-only, so this is the whole recipe: fetch, copy `include/`, name the
    target the way conan-center's recipe does so nothing downstream can tell the
    difference.
    """

    name = "cli11"
    description = "CLI11 is a command line parser for C++11 and beyond."
    license = "BSD-3-Clause"
    homepage = "https://github.com/CLIUtils/CLI11"
    url = "https://github.com/CLIUtils/CLI11"
    topics = ("cli-parser", "cpp11", "command-line", "header-only")

    package_type = "header-library"
    settings = "os", "arch", "compiler", "build_type"
    no_copy_source = True

    def layout(self):
        basic_layout(self, src_folder="src")

    def source(self):
        get(self, **self.conan_data["sources"][str(self.version)], strip_root=True)

    def validate(self):
        check_min_cppstd(self, 11)

    def package_id(self):
        self.info.clear()

    def package(self):
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        copy(
            self,
            "*",
            os.path.join(self.source_folder, "include"),
            os.path.join(self.package_folder, "include"),
        )

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "CLI11")
        self.cpp_info.set_property("cmake_target_name", "CLI11::CLI11")
        self.cpp_info.set_property("pkg_config_name", "CLI11")
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
