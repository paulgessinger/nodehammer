"""sokol-shdc — prebuilt shader compiler for sokol_gfx.

Wraps the per-platform binary released by floooh/sokol-tools-bin. There's
no source build path here — sokol-shdc is distributed only as a prebuilt
static binary, so the recipe simply downloads, verifies, and packages.

Used as a `tool_requires` from the root conanfile.py: Conan 2 then resolves
`settings.os` / `settings.arch` against the BUILD profile (not the host),
so cross-compiling to Emscripten still picks up the native macOS/Linux/
Windows binary that runs on the developer's machine.
"""

import os
import stat

from conan import ConanFile
from conan.errors import ConanInvalidConfiguration
from conan.tools.files import copy, download


class SokolShdcConan(ConanFile):
    name = "sokol-shdc"
    description = "Shader compiler / code generator for sokol_gfx (prebuilt binary)."
    license = "MIT"
    homepage = "https://github.com/floooh/sokol-tools"
    url = "https://github.com/floooh/sokol-tools-bin"
    topics = ("sokol", "shader", "compiler", "tool")

    package_type = "application"
    # arch + os are the only knobs that matter for picking which prebuilt
    # binary to fetch. No compiler/build_type — there's no source build.
    settings = "os", "arch"

    def validate(self):
        try:
            self._source_entry()
        except KeyError as e:
            raise ConanInvalidConfiguration(
                f"sokol-shdc has no prebuilt binary for {self.settings.os}/{self.settings.arch}. "
                f"Supported: Macos/{{armv8,x86_64}}, Linux/{{x86_64,armv8}}, Windows/x86_64."
            ) from e

    def _binary_name(self):
        return "sokol-shdc.exe" if self.settings.os == "Windows" else "sokol-shdc"

    def _source_entry(self):
        # Settings-dependent lookup; called from build()/validate(), not source().
        # Conan 2 forbids self.settings access in source() because source() is
        # meant to be settings-independent — but this binary literally is the
        # build output for a given host, so the "build" step is the download.
        sources = self.conan_data["sources"][str(self.version)]
        return sources[str(self.settings.os)][str(self.settings.arch)]

    def build(self):
        entry = self._source_entry()
        download(self, entry["url"], self._binary_name(), sha256=entry["sha256"])

    def package(self):
        binary = self._binary_name()
        src = os.path.join(self.build_folder, binary)
        dst_dir = os.path.join(self.package_folder, "bin")
        copy(self, binary, src=self.build_folder, dst=dst_dir)
        if self.settings.os != "Windows":
            dst = os.path.join(dst_dir, binary)
            os.chmod(dst, os.stat(dst).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)

    def package_info(self):
        # Pure tool: no headers, no libs. Just expose the bin dir on PATH so
        # consumers' VirtualBuildEnv / CMakeToolchain pick it up automatically.
        self.cpp_info.includedirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.bindirs = ["bin"]
